/*-------------------------------------------------------------------------
 *
 * remote_transaction.c
 *   Management of transaction spanning more than one node.
 *
 *   Since the functions defined in this file mostly allocate in
 *   CitusXactCallbackContext, we mostly try doing allocations on stack.
 *   And when it's hard to do so, we at least try freeing the heap memory
 *   immediately after an object becomes useless.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq-fe.h"
#include "miscadmin.h"

#include "access/xact.h"
#include "postmaster/postmaster.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/xid8.h"

#include "distributed/backend_data.h"
#include "distributed/citus_safe_lib.h"
#include "distributed/commands/utility_hook.h"
#include "distributed/connection_management.h"
#include "distributed/listutils.h"
#include "distributed/metadata/distobject.h"
#include "distributed/metadata_cache.h"
#include "distributed/metadata_sync.h"
#include "distributed/placement_connection.h"
#include "distributed/remote_commands.h"
#include "distributed/remote_transaction.h"
#include "distributed/transaction_identifier.h"
#include "distributed/transaction_management.h"
#include "distributed/transaction_recovery.h"
#include "distributed/worker_manager.h"


#define PREPARED_TRANSACTION_NAME_FORMAT "citus_%u_%u_"UINT64_FORMAT "_%u"


static char * BeginTransactionCommand(void);
static char * AssignDistributedTransactionIdCommand(void);
static void StartRemoteTransactionSavepointBegin(MultiConnection *connection,
												 SubTransactionId subId);
static void FinishRemoteTransactionSavepointBegin(MultiConnection *connection,
												  SubTransactionId subId);
static void StartRemoteTransactionSavepointRelease(MultiConnection *connection,
												   SubTransactionId subId);
static void FinishRemoteTransactionSavepointRelease(MultiConnection *connection,
													SubTransactionId subId);
static void StartRemoteTransactionSavepointRollback(MultiConnection *connection,
													SubTransactionId subId);
static void FinishRemoteTransactionSavepointRollback(MultiConnection *connection,
													 SubTransactionId subId);

static void Assign2PCIdentifier(MultiConnection *connection);

PG_FUNCTION_INFO_V1(start_management_transaction);
PG_FUNCTION_INFO_V1(execute_command_on_remote_nodes_as_user);
PG_FUNCTION_INFO_V1(commit_management_command_2pc);

static char *IsolationLevelName[] = {
	"READ UNCOMMITTED",
	"READ COMMITTED",
	"REPEATABLE READ",
	"SERIALIZABLE"
};

/*
 * These variables are necessary for running queries from a database that is not
 * the Citus main database. Some of these queries need to be propagated to the
 * workers and Citus main database will be used for these queries, such as
 * CREATE ROLE. For that we create a connection to the Citus main database and
 * run queries from there.
 */

/* The MultiConnection used for connecting Citus main database. */
MultiConnection *MainDBConnection = NULL;

/*
 * IsMainDBCommand is true if this is a query in the Citus main database that is started
 * by a query from a different database.
 */
bool IsMainDBCommand = false;

/*
 * The transaction id of the query from the other database that started the
 * main database query.
 */
FullTransactionId OuterXid;

/*
 * Shows if this is the Citus main database or not. We needed a variable instead of
 * checking if this database's name is the same as MainDb because we sometimes need
 * this value outside a transaction where we cannot reach the current database name.
 */
bool IsMainDB = true;

/*
 * Name of a superuser role to be used during main database connections.
 */
char *SuperuserRole = NULL;

/*
 * IsMainDBCommandInXact shows if the query sent to the main database requires
 * a transaction
 */
bool IsMainDBCommandInXact = true;


/*
 * start_management_transaction starts a management transaction
 * in the main database by recording the outer transaction's transaction id and setting
 * IsMainDBCommand to true.
 */
Datum
start_management_transaction(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);
	EnsureSuperUser();

	OuterXid = PG_GETARG_FULLTRANSACTIONID(0);
	IsMainDBCommand = true;

	Use2PCForCoordinatedTransaction();

	PG_RETURN_VOID();
}


/*
 * execute_command_on_remote_nodes_as_user executes the query on the nodes
 * other than the current node, using the user passed.
 */
Datum
execute_command_on_remote_nodes_as_user(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);
	EnsureSuperUser();

	text *queryText = PG_GETARG_TEXT_P(0);
	char *query = text_to_cstring(queryText);

	text *usernameText = PG_GETARG_TEXT_P(1);
	char *username = text_to_cstring(usernameText);

	StringInfo queryToSend = makeStringInfo();

	appendStringInfo(queryToSend, "%s;%s;%s", DISABLE_METADATA_SYNC, query,
					 ENABLE_METADATA_SYNC);

	SendCommandToWorkersAsUser(REMOTE_NODES, username, queryToSend->data);
	PG_RETURN_VOID();
}


/*
 * commit_management_command_2pc is a wrapper UDF for
 * CoordinatedRemoteTransactionsCommit
 */
Datum
commit_management_command_2pc(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);
	EnsureSuperUser();

	RecoverTwoPhaseCommits();

	PG_RETURN_VOID();
}


/*
 * RunCitusMainDBQuery creates a connection to Citus main database if necessary
 * and runs the query over the connection in the main database.
 */
void
RunCitusMainDBQuery(char *query)
{
	if (MainDBConnection == NULL)
	{
		if (strlen(SuperuserRole) == 0)
		{
			ereport(ERROR, (errmsg("No superuser role is given for Citus main "
								   "database connection"),
							errhint("Set citus.superuser to a superuser role name")));
		}
		int flags = 0;
		MainDBConnection = GetNodeUserDatabaseConnection(flags, LocalHostName,
														 PostPortNumber,
														 SuperuserRole,
														 MainDb);

		if (IsMainDBCommandInXact)
		{
			RemoteTransactionBegin(MainDBConnection);
		}
	}

	SendRemoteCommand(MainDBConnection, query);

	PGresult *result = GetRemoteCommandResult(MainDBConnection, true);

	if (!IsResponseOK(result))
	{
		ReportResultError(MainDBConnection, result, ERROR);
	}

	ForgetResults(MainDBConnection);
}


/*
 * CleanCitusMainDBConnection closes and removes the connection to Citus main database.
 */
void
CleanCitusMainDBConnection(void)
{
	if (MainDBConnection == NULL)
	{
		return;
	}
	CloseConnection(MainDBConnection);
	MainDBConnection = NULL;
}


/*
 * StartRemoteTransactionBegin initiates beginning the remote transaction in
 * a non-blocking manner. The function sends "BEGIN" followed by
 * assign_distributed_transaction_id() to assign the distributed transaction
 * id on the remote node.
 */
void
StartRemoteTransactionBegin(struct MultiConnection *connection)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;

	Assert(transaction->transactionState == REMOTE_TRANS_NOT_STARTED);

	/* remember transaction as being in-progress */
	dlist_push_tail(&InProgressTransactions, &connection->transactionNode);
	connection->transactionInProgress = true;

	transaction->transactionState = REMOTE_TRANS_STARTING;

	StringInfo beginAndSetDistributedTransactionId = makeStringInfo();

	/*
	 * Explicitly specify READ COMMITTED, the default on the remote
	 * side might have been changed, and that would cause problematic
	 * behaviour.
	 */
	appendStringInfoString(beginAndSetDistributedTransactionId,
						   BeginTransactionCommand());

	/* append context for in-progress SAVEPOINTs for this transaction */
	List *activeSubXacts = ActiveSubXactContexts();
	transaction->lastSuccessfulSubXact = TopSubTransactionId;
	transaction->lastQueuedSubXact = TopSubTransactionId;

	SubXactContext *subXactState = NULL;
	foreach_declared_ptr(subXactState, activeSubXacts)
	{
		/* append SET LOCAL state from when SAVEPOINT was encountered... */
		if (subXactState->setLocalCmds != NULL)
		{
			appendStringInfoString(beginAndSetDistributedTransactionId,
								   subXactState->setLocalCmds->data);
		}

		/* ... then append SAVEPOINT to enter this subxact */
		appendStringInfo(beginAndSetDistributedTransactionId,
						 "SAVEPOINT savepoint_%u;", subXactState->subId);
		transaction->lastQueuedSubXact = subXactState->subId;
	}

	/* we've pushed into deepest subxact: apply in-progress SET context */
	if (activeSetStmts != NULL)
	{
		appendStringInfoString(beginAndSetDistributedTransactionId, activeSetStmts->data);
	}

	char *assignDistributedTransactionIdCommand = AssignDistributedTransactionIdCommand();

	/* add SELECT assign_distributed_transaction_id ... */
	appendStringInfoString(beginAndSetDistributedTransactionId,
						   assignDistributedTransactionIdCommand);

	pfree(assignDistributedTransactionIdCommand);

	bool success = SendRemoteCommand(connection,
									 beginAndSetDistributedTransactionId->data);

	pfree(beginAndSetDistributedTransactionId->data);
	pfree(beginAndSetDistributedTransactionId);

	if (!success)
	{
		const bool raiseErrors = true;

		HandleRemoteTransactionConnectionError(connection, raiseErrors);
	}

	transaction->beginSent = true;
}


/*
 * BeginAndSetDistributedTransactionIdCommand returns a command which starts
 * a transaction and assigns the current distributed transaction id.
 */
StringInfo
BeginAndSetDistributedTransactionIdCommand(void)
{
	StringInfo beginAndSetDistributedTransactionId = makeStringInfo();

	/*
	 * Explicitly specify READ COMMITTED, the default on the remote
	 * side might have been changed, and that would cause problematic
	 * behaviour.
	 */
	appendStringInfoString(beginAndSetDistributedTransactionId,
						   BeginTransactionCommand());

	appendStringInfoString(beginAndSetDistributedTransactionId,
						   AssignDistributedTransactionIdCommand());

	return beginAndSetDistributedTransactionId;
}


/*
 * BeginTransactionCommand returns the BEGIN command to use for the current isolation
 * level.
 *
 * Transactions have 3 properties that we care about here:
 * - XactIsoLevel (isolation level)
 * - XactDeferrable (deferrable)
 * - XactReadOnly (read only)
 *
 * These properties can be set in several ways:
 * - via BEGIN TRANSACTION ISOLATION LEVEL ...
 * - via default_transaction_isolation, ...
 * - via SET TRANSACTION .. (or plain SET transaction_isolation ...)
 *
 * We want to make sure that the properties that are passed to the worker nodes
 * match the coordinator as much as possible. However, we do not want to waste
 * bytes repeating the current values ad infinitum.
 *
 * The trade-off we make is that we send the isolation level in all cases,
 * but only set deferrable and read-only if they were explicitly specified
 * in the BEGIN by the user. The implication is that we may not follow the
 * default_transaction_* settings on the coordinator if they differ on the
 * worker.
 */
static char *
BeginTransactionCommand(void)
{
	StringInfo beginCommand = makeStringInfo();

	/*
	 * XactIsoLevel can only be set at the start of the transaction, before the
	 * first query. Since Citus does not send BEGIN until the first query, we
	 * can simply use the current values, and they will match the values for the
	 * outer transaction after any BEGIN and SET TRANSACTION that may have occurred.
	 */
	appendStringInfo(beginCommand, "BEGIN TRANSACTION ISOLATION LEVEL %s",
					 IsolationLevelName[XactIsoLevel]);

	if (BeginXactDeferrable == BeginXactDeferrable_Enabled)
	{
		appendStringInfoString(beginCommand, " DEFERRABLE");
	}
	else if (BeginXactDeferrable == BeginXactDeferrable_Disabled)
	{
		appendStringInfoString(beginCommand, " NOT DEFERRABLE");
	}

	if (BeginXactReadOnly == BeginXactReadOnly_Enabled)
	{
		appendStringInfoString(beginCommand, " READ ONLY");
	}
	else if (BeginXactReadOnly == BeginXactReadOnly_Disabled)
	{
		appendStringInfoString(beginCommand, " READ WRITE");
	}

	appendStringInfoChar(beginCommand, ';');

	return beginCommand->data;
}


/*
 * AssignDistributedTransactionIdCommand returns a command to set the local
 * distributed transaction ID on a remote transaction.
 */
static char *
AssignDistributedTransactionIdCommand(void)
{
	StringInfo assignDistributedTransactionId = makeStringInfo();

	/*
	 * Append BEGIN and assign_distributed_transaction_id() statements into a single command
	 * and send both in one step. The reason is purely performance, we don't want
	 * seperate roundtrips for these two statements.
	 */
	DistributedTransactionId *distributedTransactionId =
		GetCurrentDistributedTransactionId();
	const char *timestamp = timestamptz_to_str(distributedTransactionId->timestamp);
	appendStringInfo(assignDistributedTransactionId,
					 "SELECT assign_distributed_transaction_id(%d, " UINT64_FORMAT
					 ", '%s');",
					 distributedTransactionId->initiatorNodeIdentifier,
					 distributedTransactionId->transactionNumber,
					 timestamp);

	/* free the StringInfo but not the buffer itself */
	char *command = assignDistributedTransactionId->data;
	pfree(assignDistributedTransactionId);

	return command;
}


/*
 * FinishRemoteTransactionBegin finishes the work StartRemoteTransactionBegin
 * initiated. It blocks if necessary (i.e. if PQisBusy() would return true).
 */
void
FinishRemoteTransactionBegin(struct MultiConnection *connection)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;
	bool raiseErrors = true;

	Assert(transaction->transactionState == REMOTE_TRANS_STARTING);

	bool clearSuccessful = ClearResults(connection, raiseErrors);
	if (clearSuccessful)
	{
		transaction->transactionState = REMOTE_TRANS_STARTED;
		transaction->lastSuccessfulSubXact = transaction->lastQueuedSubXact;
	}

	if (!transaction->transactionFailed)
	{
		Assert(PQtransactionStatus(connection->pgConn) == PQTRANS_INTRANS);
	}
}


/*
 * RemoteTransactionBegin begins a remote transaction in a blocking manner.
 */
void
RemoteTransactionBegin(struct MultiConnection *connection)
{
	StartRemoteTransactionBegin(connection);
	FinishRemoteTransactionBegin(connection);
}


/*
 * RemoteTransactionListBegin sends BEGIN over all connections in the
 * given connection list and waits for all of them to finish.
 */
void
RemoteTransactionListBegin(List *connectionList)
{
	MultiConnection *connection = NULL;

	/* send BEGIN to all nodes */
	foreach_declared_ptr(connection, connectionList)
	{
		StartRemoteTransactionBegin(connection);
	}

	/* wait for BEGIN to finish on all nodes */
	foreach_declared_ptr(connection, connectionList)
	{
		FinishRemoteTransactionBegin(connection);
	}
}


/*
 * StartRemoteTransactionCommit initiates transaction commit in a non-blocking
 * manner.  If the transaction is in a failed state, it'll instead get rolled
 * back.
 */
void
StartRemoteTransactionCommit(MultiConnection *connection)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;
	const bool raiseErrors = false;

	/* can only commit if transaction is in progress */
	Assert(transaction->transactionState != REMOTE_TRANS_NOT_STARTED);

	/* can't commit if we already started to commit or abort */
	Assert(transaction->transactionState < REMOTE_TRANS_1PC_ABORTING);

	if (transaction->transactionFailed)
	{
		/* abort the transaction if it failed */
		transaction->transactionState = REMOTE_TRANS_1PC_ABORTING;

		/*
		 * Try sending an ROLLBACK; Depending on the state that won't
		 * succeed, but let's try.  Have to clear previous results
		 * first.
		 */
		ForgetResults(connection); /* try to clear pending stuff */
		if (!SendRemoteCommand(connection, "ROLLBACK"))
		{
			/* no point in reporting a likely redundant message */
		}
	}
	else if (transaction->transactionState == REMOTE_TRANS_PREPARED)
	{
		/*
		 * Commit the prepared transaction.
		 *
		 * We need to allocate 420 bytes for command buffer (including '\0'):
		 *  - len("COMMIT PREPARED ") = 16
		 *  - maximum quoted length of transaction->preparedName = 2 * 200 + 3 = 403
		 */
		char command[420];
		char *quotedPrepName = quote_literal_cstr(transaction->preparedName);
		SafeSnprintf(command, sizeof(command), "COMMIT PREPARED %s", quotedPrepName);
		pfree(quotedPrepName);

		transaction->transactionState = REMOTE_TRANS_2PC_COMMITTING;

		if (!SendRemoteCommand(connection, command))
		{
			HandleRemoteTransactionConnectionError(connection, raiseErrors);
		}
	}
	else
	{
		/* initiate remote transaction commit */
		transaction->transactionState = REMOTE_TRANS_1PC_COMMITTING;

		if (!SendRemoteCommand(connection, "COMMIT"))
		{
			/*
			 * For a moment there I thought we were in trouble.
			 *
			 * Failing in this state means that we don't know whether the
			 * commit has succeeded.
			 */
			HandleRemoteTransactionConnectionError(connection, raiseErrors);
		}
	}
}


/*
 * FinishRemoteTransactionCommit finishes the work
 * StartRemoteTransactionCommit initiated. It blocks if necessary (i.e. if
 * PQisBusy() would return true).
 */
void
FinishRemoteTransactionCommit(MultiConnection *connection)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;
	const bool raiseErrors = false;

	Assert(transaction->transactionState == REMOTE_TRANS_1PC_ABORTING ||
		   transaction->transactionState == REMOTE_TRANS_1PC_COMMITTING ||
		   transaction->transactionState == REMOTE_TRANS_2PC_COMMITTING);

	PGresult *result = GetRemoteCommandResult(connection, raiseErrors);

	if (!IsResponseOK(result))
	{
		HandleRemoteTransactionResultError(connection, result, raiseErrors);

		/*
		 * Failing in this state means that we will often not know whether
		 * the commit has succeeded (particularly in case of network
		 * troubles).
		 *
		 * XXX: It might be worthwhile to discern cases where we got a
		 * proper error back from postgres (i.e. COMMIT was received but
		 * produced an error) from cases where the connection failed
		 * before getting a reply.
		 */

		if (transaction->transactionState == REMOTE_TRANS_1PC_COMMITTING)
		{
			ereport(WARNING, (errmsg("failed to commit transaction on %s:%d",
									 connection->hostname, connection->port)));
		}
		else if (transaction->transactionState == REMOTE_TRANS_2PC_COMMITTING)
		{
			ereport(WARNING, (errmsg("failed to commit transaction on %s:%d",
									 connection->hostname, connection->port)));
		}
	}
	else if (transaction->transactionState == REMOTE_TRANS_1PC_ABORTING ||
			 transaction->transactionState == REMOTE_TRANS_2PC_ABORTING)
	{
		transaction->transactionState = REMOTE_TRANS_ABORTED;
	}
	else
	{
		transaction->transactionState = REMOTE_TRANS_COMMITTED;
	}

	PQclear(result);

	ForgetResults(connection);
}


/*
 * RemoteTransactionCommit commits (or aborts, if the transaction failed) a
 * remote transaction in a blocking manner.
 */
void
RemoteTransactionCommit(MultiConnection *connection)
{
	StartRemoteTransactionCommit(connection);
	FinishRemoteTransactionCommit(connection);
}


/*
 * StartRemoteTransactionAbort initiates abortin the transaction in a
 * non-blocking manner.
 */
void
StartRemoteTransactionAbort(MultiConnection *connection)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;
	const bool raiseErrors = false;

	Assert(transaction->transactionState != REMOTE_TRANS_NOT_STARTED);

	/*
	 * Clear previous results, so we have a better chance to send ROLLBACK
	 * [PREPARED]. If we've previously sent a PREPARE TRANSACTION, we always
	 * want to wait for that result, as that shouldn't take long and will
	 * reserve resources.  But if there's another query running, we don't want
	 * to wait, because a long running statement may be running, so force it to
	 * be killed in that case.
	 */
	if (transaction->transactionState == REMOTE_TRANS_PREPARING ||
		transaction->transactionState == REMOTE_TRANS_PREPARED)
	{
		ForgetResults(connection);

		/*
		 * Await PREPARE TRANSACTION results, closing the connection would leave it dangling.
		 *
		 * We need to allocate 422 bytes for command buffer (including '\0'):
		 *  - len("ROLLBACK PREPARED ") = 18
		 *  - maximum quoted length of transaction->preparedName = 2 * 200 + 3 = 403
		 */
		char command[422];
		char *quotedPrepName = quote_literal_cstr(transaction->preparedName);
		SafeSnprintf(command, sizeof(command), "ROLLBACK PREPARED %s", quotedPrepName);
		pfree(quotedPrepName);

		if (!SendRemoteCommand(connection, command))
		{
			HandleRemoteTransactionConnectionError(connection, raiseErrors);
		}
		else
		{
			transaction->transactionState = REMOTE_TRANS_2PC_ABORTING;
		}
	}
	else
	{
		/*
		 * In case of a cancellation, the connection might still be working
		 * on some commands. Try to consume the results such that the
		 * connection can be reused, but do not want to wait for commands
		 * to finish. Instead we just close the connection if the command
		 * is still busy.
		 */
		if (!ClearResultsIfReady(connection))
		{
			ShutdownConnection(connection);

			/* FinishRemoteTransactionAbort will emit warning */
			return;
		}

		if (!SendRemoteCommand(connection, "ROLLBACK"))
		{
			/* no point in reporting a likely redundant message */
			MarkRemoteTransactionFailed(connection, raiseErrors);
		}
		else
		{
			transaction->transactionState = REMOTE_TRANS_1PC_ABORTING;
		}
	}
}


/*
 * FinishRemoteTransactionAbort finishes the work StartRemoteTransactionAbort
 * initiated. It blocks if necessary (i.e. if PQisBusy() would return true).
 */
void
FinishRemoteTransactionAbort(MultiConnection *connection)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;
	const bool raiseErrors = false;

	if (transaction->transactionState == REMOTE_TRANS_2PC_ABORTING)
	{
		PGresult *result = GetRemoteCommandResult(connection, raiseErrors);
		if (!IsResponseOK(result))
		{
			HandleRemoteTransactionResultError(connection, result, raiseErrors);
		}

		PQclear(result);
	}

	/*
	 * Try to consume results of any in-progress commands. In the 1PC case
	 * this is also where we consume the result of the ROLLBACK.
	 *
	 * If we don't succeed the connection will be in a bad state, so we close it.
	 */
	if (!ClearResults(connection, raiseErrors))
	{
		ShutdownConnection(connection);
	}

	transaction->transactionState = REMOTE_TRANS_ABORTED;
}


/*
 * RemoteTransactionAbort aborts a remote transaction in a blocking manner.
 */
void
RemoteTransactionAbort(MultiConnection *connection)
{
	StartRemoteTransactionAbort(connection);
	FinishRemoteTransactionAbort(connection);
}


/*
 * StartRemoteTransactionPrepare initiates preparing the transaction in a
 * non-blocking manner.
 */
void
StartRemoteTransactionPrepare(struct MultiConnection *connection)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;
	const bool raiseErrors = true;

	/* can't prepare a nonexistant transaction */
	Assert(transaction->transactionState != REMOTE_TRANS_NOT_STARTED);

	/* can't prepare in a failed transaction */
	Assert(!transaction->transactionFailed);

	/* can't prepare if already started to prepare/abort/commit */
	Assert(transaction->transactionState < REMOTE_TRANS_PREPARING);

	Assign2PCIdentifier(connection);

	/* log transactions to workers in pg_dist_transaction */
	WorkerNode *workerNode = FindWorkerNode(connection->hostname, connection->port);
	if (workerNode != NULL)
	{
		LogTransactionRecord(workerNode->groupId, transaction->preparedName, OuterXid);
	}

	/*
	 * We need to allocate 424 bytes for command buffer (including '\0'):
	 *  - len("PREPARE TRANSACTION ") = 20
	 *  - maximum quoted length of transaction->preparedName = 2 * 200 + 3 = 403
	 */
	char command[424];
	char *quotedPrepName = quote_literal_cstr(transaction->preparedName);
	SafeSnprintf(command, sizeof(command), "PREPARE TRANSACTION %s", quotedPrepName);
	pfree(quotedPrepName);

	if (!SendRemoteCommand(connection, command))
	{
		HandleRemoteTransactionConnectionError(connection, raiseErrors);
	}
	else
	{
		transaction->transactionState = REMOTE_TRANS_PREPARING;
	}
}


/*
 * FinishRemoteTransactionPrepare finishes the work
 * StartRemoteTransactionPrepare initiated. It blocks if necessary (i.e. if
 * PQisBusy() would return true).
 */
void
FinishRemoteTransactionPrepare(struct MultiConnection *connection)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;
	const bool raiseErrors = true;

	Assert(transaction->transactionState == REMOTE_TRANS_PREPARING);

	PGresult *result = GetRemoteCommandResult(connection, raiseErrors);

	if (!IsResponseOK(result))
	{
		transaction->transactionState = REMOTE_TRANS_ABORTED;
		HandleRemoteTransactionResultError(connection, result, raiseErrors);
	}
	else
	{
		transaction->transactionState = REMOTE_TRANS_PREPARED;
	}

	PQclear(result);

	/*
	 * Try to consume results of PREPARE TRANSACTION command. If we don't
	 * succeed, rollback the transaction. Note that we've not committed on
	 * any node yet, and we're not sure about the state of the worker node.
	 * So rollbacking seems to be the safest action if the worker is
	 * in a state where it can actually rollback.
	 */
	if (!ClearResults(connection, raiseErrors))
	{
		ereport(ERROR, (errmsg("failed to prepare transaction '%s' on host %s:%d",
							   transaction->preparedName, connection->hostname,
							   connection->port),
						errhint("Try re-running the command.")));
	}
}


/*
 * RemoteTransactionBeginIfNecessary is a convenience wrapper around
 * RemoteTransactionsBeginIfNecessary(), for a single connection.
 */
void
RemoteTransactionBeginIfNecessary(MultiConnection *connection)
{
	/* just delegate */
	if (InCoordinatedTransaction())
	{
		List *connectionList = list_make1(connection);

		RemoteTransactionsBeginIfNecessary(connectionList);
		list_free(connectionList);
	}
}


/*
 * RemoteTransactionsBeginIfNecessary begins, if necessary according to this
 * session's coordinated transaction state, and the remote transaction's
 * state, an explicit transaction on all the connections.  This is done in
 * parallel, to lessen latency penalties.
 */
void
RemoteTransactionsBeginIfNecessary(List *connectionList)
{
	MultiConnection *connection = NULL;

	/*
	 * Don't do anything if not in a coordinated transaction. That allows the
	 * same code to work both in situations that uses transactions, and when
	 * not.
	 */
	if (!InCoordinatedTransaction())
	{
		return;
	}

	/* issue BEGIN to all connections needing it */
	foreach_declared_ptr(connection, connectionList)
	{
		RemoteTransaction *transaction = &connection->remoteTransaction;

		/* can't send BEGIN if a command already is in progress */
		Assert(PQtransactionStatus(connection->pgConn) != PQTRANS_ACTIVE);

		/*
		 * If a transaction already is in progress (including having failed),
		 * don't start it again. That's quite normal if a piece of code allows
		 * cached connections.
		 */
		if (transaction->transactionState != REMOTE_TRANS_NOT_STARTED)
		{
			continue;
		}

		StartRemoteTransactionBegin(connection);
	}

	bool raiseInterrupts = true;
	WaitForAllConnections(connectionList, raiseInterrupts);

	/* get result of all the BEGINs */
	foreach_declared_ptr(connection, connectionList)
	{
		RemoteTransaction *transaction = &connection->remoteTransaction;

		/*
		 * Only handle BEGIN results on connections that are in process of
		 * starting a transaction, and haven't already failed (e.g. by not
		 * being able to send BEGIN due to a network failure).
		 */
		if (transaction->transactionFailed ||
			transaction->transactionState != REMOTE_TRANS_STARTING)
		{
			continue;
		}

		FinishRemoteTransactionBegin(connection);
	}
}


/*
 * HandleRemoteTransactionConnectionError records a transaction as having failed
 * and throws a connection error if the transaction was critical and raiseErrors
 * is true, or a warning otherwise.
 */
void
HandleRemoteTransactionConnectionError(MultiConnection *connection, bool raiseErrors)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;

	transaction->transactionFailed = true;

	if (transaction->transactionCritical && raiseErrors)
	{
		ReportConnectionError(connection, ERROR);
	}
	else
	{
		ReportConnectionError(connection, WARNING);
	}
}


/*
 * HandleRemoteTransactionResultError records a transaction as having failed
 * and throws a result error if the transaction was critical and raiseErrors
 * is true, or a warning otherwise.
 */
void
HandleRemoteTransactionResultError(MultiConnection *connection, PGresult *result, bool
								   raiseErrors)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;

	transaction->transactionFailed = true;

	if (transaction->transactionCritical && raiseErrors)
	{
		ReportResultError(connection, result, ERROR);
	}
	else
	{
		ReportResultError(connection, result, WARNING);
	}
}


/*
 * MarkRemoteTransactionFailed records a transaction as having failed.
 *
 * If the connection is marked as critical, and allowErrorPromotion is true,
 * this routine will ERROR out. The allowErrorPromotion case is primarily
 * required for the transaction management code itself. Usually it is helpful
 * to fail as soon as possible. If !allowErrorPromotion transaction commit
 * will instead issue an error before committing on any node.
 */
void
MarkRemoteTransactionFailed(MultiConnection *connection, bool allowErrorPromotion)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;

	transaction->transactionFailed = true;

	/*
	 * If the connection is marked as critical, fail the entire coordinated
	 * transaction. If allowed.
	 */
	if (transaction->transactionCritical && allowErrorPromotion)
	{
		ereport(ERROR, (errmsg("failure on connection marked as essential: %s:%d",
							   connection->hostname, connection->port)));
	}
}


/*
 * MarkRemoteTransactionCritical signals that failures on this remote
 * transaction should fail the entire coordinated transaction.
 */
void
MarkRemoteTransactionCritical(struct MultiConnection *connection)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;

	transaction->transactionCritical = true;
}


/*
 * ResetRemoteTransaction resets the state of the transaction after the end of
 * the main transaction, if the connection is being reused.
 */
void
ResetRemoteTransaction(struct MultiConnection *connection)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;

	/* unlink from list of open transactions, if necessary */
	if (connection->transactionInProgress)
	{
		/* XXX: Should we error out for a critical transaction? */

		dlist_delete(&connection->transactionNode);
		connection->transactionInProgress = false;
		memset(&connection->transactionNode, 0, sizeof(connection->transactionNode));
	}

	/* just reset the entire state, relying on 0 being invalid/false */
	memset(transaction, 0, sizeof(*transaction));

	ResetShardPlacementAssociation(connection);

	/* reset copy state */
	connection->copyBytesWrittenSinceLastFlush = 0;
}


/*
 * CoordinatedRemoteTransactionsPrepare PREPAREs a 2PC transaction on all
 * non-failed transactions participating in the coordinated transaction.
 */
void
CoordinatedRemoteTransactionsPrepare(void)
{
	dlist_iter iter;
	List *connectionList = NIL;

	/* issue PREPARE TRANSACTION; to all relevant remote nodes */

	/* asynchronously send PREPARE */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;

		Assert(transaction->transactionState != REMOTE_TRANS_NOT_STARTED);

		/* can't PREPARE a transaction that failed */
		if (transaction->transactionFailed)
		{
			continue;
		}

		/*
		 * Check if any DML or DDL is executed over the connection on any
		 * placement/table. If yes, we start preparing the transaction, otherwise
		 * we skip prepare since the connection didn't perform any write (read-only)
		 */
		if (ConnectionModifiedPlacement(connection))
		{
			StartRemoteTransactionPrepare(connection);
			connectionList = lappend(connectionList, connection);
		}
	}

	bool raiseInterrupts = true;
	WaitForAllConnections(connectionList, raiseInterrupts);

	/* Wait for result */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;

		if (transaction->transactionState != REMOTE_TRANS_PREPARING)
		{
			/*
			 * Verify that either the transaction failed, hence we couldn't prepare
			 * or the connection didn't modify any placement
			 */
			Assert(transaction->transactionFailed ||
				   !ConnectionModifiedPlacement(connection));
			continue;
		}

		FinishRemoteTransactionPrepare(connection);
	}

	CurrentCoordinatedTransactionState = COORD_TRANS_PREPARED;

	list_free(connectionList);
}


/*
 * CoordinatedRemoteTransactionsCommit performs distributed transactions
 * handling at commit time. This will be called at XACT_EVENT_PRE_COMMIT if
 * 1PC commits are used - so shards can still be invalidated - and at
 * XACT_EVENT_COMMIT if 2PC is being used.
 *
 * Note that this routine has to issue rollbacks for failed transactions.
 */
void
CoordinatedRemoteTransactionsCommit(void)
{
	dlist_iter iter;
	List *connectionList = NIL;

	/*
	 * Issue appropriate transaction commands to remote nodes. If everything
	 * went well that's going to be COMMIT or COMMIT PREPARED, if individual
	 * connections had errors, some or all of them might require a ROLLBACK.
	 *
	 * First send the command asynchronously over all connections.
	 */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;

		if (transaction->transactionState == REMOTE_TRANS_NOT_STARTED ||
			transaction->transactionState == REMOTE_TRANS_1PC_COMMITTING ||
			transaction->transactionState == REMOTE_TRANS_2PC_COMMITTING ||
			transaction->transactionState == REMOTE_TRANS_COMMITTED ||
			transaction->transactionState == REMOTE_TRANS_ABORTED)
		{
			continue;
		}

		StartRemoteTransactionCommit(connection);
		connectionList = lappend(connectionList, connection);
	}

	bool raiseInterrupts = false;
	WaitForAllConnections(connectionList, raiseInterrupts);

	/* wait for the replies to the commands to come in */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;

		/* nothing to do if not committing / aborting */
		if (transaction->transactionState != REMOTE_TRANS_1PC_COMMITTING &&
			transaction->transactionState != REMOTE_TRANS_2PC_COMMITTING &&
			transaction->transactionState != REMOTE_TRANS_1PC_ABORTING &&
			transaction->transactionState != REMOTE_TRANS_2PC_ABORTING)
		{
			continue;
		}

		FinishRemoteTransactionCommit(connection);
	}

	list_free(connectionList);
}


/*
 * CoordinatedRemoteTransactionsAbort performs distributed transactions
 * handling at abort time.
 *
 * This issues ROLLBACKS and ROLLBACK PREPARED depending on whether the remote
 * transaction has been prepared or not.
 */
void
CoordinatedRemoteTransactionsAbort(void)
{
	dlist_iter iter;
	List *connectionList = NIL;

	/* asynchronously send ROLLBACK [PREPARED] */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;

		if (transaction->transactionState == REMOTE_TRANS_NOT_STARTED ||
			transaction->transactionState == REMOTE_TRANS_1PC_ABORTING ||
			transaction->transactionState == REMOTE_TRANS_2PC_ABORTING ||
			transaction->transactionState == REMOTE_TRANS_ABORTED)
		{
			continue;
		}

		StartRemoteTransactionAbort(connection);
		connectionList = lappend(connectionList, connection);
	}

	bool raiseInterrupts = false;
	WaitForAllConnections(connectionList, raiseInterrupts);

	/* and wait for the results */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;

		if (transaction->transactionState != REMOTE_TRANS_1PC_ABORTING &&
			transaction->transactionState != REMOTE_TRANS_2PC_ABORTING)
		{
			continue;
		}

		FinishRemoteTransactionAbort(connection);
	}

	list_free(connectionList);
}


/*
 * CoordinatedRemoteTransactionsSavepointBegin sends the SAVEPOINT command for
 * the given sub-transaction id to all connections participating in the current
 * transaction.
 */
void
CoordinatedRemoteTransactionsSavepointBegin(SubTransactionId subId)
{
	dlist_iter iter;
	const bool raiseInterrupts = true;
	List *connectionList = NIL;

	/* asynchronously send SAVEPOINT */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;
		if (transaction->transactionFailed)
		{
			continue;
		}

		StartRemoteTransactionSavepointBegin(connection, subId);
		connectionList = lappend(connectionList, connection);
	}

	WaitForAllConnections(connectionList, raiseInterrupts);

	/* and wait for the results */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;
		if (transaction->transactionFailed)
		{
			continue;
		}

		FinishRemoteTransactionSavepointBegin(connection, subId);

		if (!transaction->transactionFailed)
		{
			transaction->lastSuccessfulSubXact = subId;
		}
	}

	list_free(connectionList);
}


/*
 * CoordinatedRemoteTransactionsSavepointRelease sends the RELEASE SAVEPOINT
 * command for the given sub-transaction id to all connections participating in
 * the current transaction.
 */
void
CoordinatedRemoteTransactionsSavepointRelease(SubTransactionId subId)
{
	dlist_iter iter;
	const bool raiseInterrupts = true;
	List *connectionList = NIL;

	/* asynchronously send RELEASE SAVEPOINT */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;
		if (transaction->transactionFailed)
		{
			continue;
		}

		StartRemoteTransactionSavepointRelease(connection, subId);
		connectionList = lappend(connectionList, connection);
	}

	WaitForAllConnections(connectionList, raiseInterrupts);

	/* and wait for the results */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;
		if (transaction->transactionFailed)
		{
			continue;
		}

		FinishRemoteTransactionSavepointRelease(connection, subId);
	}

	list_free(connectionList);
}


/*
 * CoordinatedRemoteTransactionsSavepointRollback sends the ROLLBACK TO SAVEPOINT
 * command for the given sub-transaction id to all connections participating in
 * the current transaction.
 */
void
CoordinatedRemoteTransactionsSavepointRollback(SubTransactionId subId)
{
	dlist_iter iter;
	const bool raiseInterrupts = false;
	List *connectionList = NIL;

	/* asynchronously send ROLLBACK TO SAVEPOINT */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;

		/* cancel any ongoing queries before issuing rollback */
		SendCancelationRequest(connection);

		/* clear results, but don't show cancelation warning messages from workers. */
		ClearResultsDiscardWarnings(connection, raiseInterrupts);

		if (transaction->transactionFailed)
		{
			if (transaction->lastSuccessfulSubXact <= subId)
			{
				transaction->transactionRecovering = true;

				/*
				 * Clear the results of the failed query so we can send the ROLLBACK
				 * TO SAVEPOINT command for a savepoint that can recover the transaction
				 * from failure.
				 */
				ForgetResults(connection);
			}
			else
			{
				continue;
			}
		}
		StartRemoteTransactionSavepointRollback(connection, subId);
		connectionList = lappend(connectionList, connection);
	}

	WaitForAllConnections(connectionList, raiseInterrupts);

	/* and wait for the results */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;
		if (transaction->transactionFailed && !transaction->transactionRecovering)
		{
			continue;
		}

		FinishRemoteTransactionSavepointRollback(connection, subId);

		/*
		 * We unclaim the connection now so it can be used again when
		 * continuing after the ROLLBACK TO SAVEPOINT.
		 * XXX: We do not undo our hadDML/hadDDL flags. This could result in
		 * some queries not being allowed on Citus that would actually be fine
		 * to execute.  Changing this would require us to keep track for each
		 * savepoint which placement connections had DDL/DML executed at that
		 * point and if they were already. We also do not call
		 * ResetShardPlacementAssociation. This might result in suboptimal
		 * parallelism, because of placement associations that are not really
		 * necessary anymore because of ROLLBACK TO SAVEPOINT. To change this
		 * we would need to keep track of when a connection becomes associated
		 * to a placement.
		 */
		UnclaimConnection(connection);
	}

	list_free(connectionList);
}


/*
 * StartRemoteTransactionSavepointBegin initiates SAVEPOINT command for the given
 * subtransaction id in a non-blocking manner.
 */
static void
StartRemoteTransactionSavepointBegin(MultiConnection *connection, SubTransactionId subId)
{
	const bool raiseErrors = true;

	/*
	 * We need to allocate 31 bytes for command buffer (including '\0'):
	 *  - len("SAVEPOINT savepoint_") = 20
	 *  - maximum length of str(subId) = 10
	 */
	char savepointCommand[31];
	SafeSnprintf(savepointCommand, sizeof(savepointCommand), "SAVEPOINT savepoint_%u",
				 subId);

	if (!SendRemoteCommand(connection, savepointCommand))
	{
		HandleRemoteTransactionConnectionError(connection, raiseErrors);
	}
}


/*
 * FinishRemoteTransactionSavepointBegin finishes the work
 * StartRemoteTransactionSavepointBegin initiated. It blocks if necessary (i.e.
 * if PQisBusy() would return true).
 */
static void
FinishRemoteTransactionSavepointBegin(MultiConnection *connection, SubTransactionId subId)
{
	const bool raiseErrors = true;
	PGresult *result = GetRemoteCommandResult(connection, raiseErrors);
	if (!IsResponseOK(result))
	{
		HandleRemoteTransactionResultError(connection, result, raiseErrors);
	}

	PQclear(result);
	ForgetResults(connection);
}


/*
 * StartRemoteTransactionSavepointRelease initiates RELEASE SAVEPOINT command for
 * the given subtransaction id in a non-blocking manner.
 */
static void
StartRemoteTransactionSavepointRelease(MultiConnection *connection,
									   SubTransactionId subId)
{
	const bool raiseErrors = true;

	/*
	 * We need to allocate 39 bytes for command buffer (including '\0'):
	 *  - len("RELEASE SAVEPOINT savepoint_") = 28
	 *  - maximum length of str(subId) = 10
	 */
	char savepointCommand[39];
	SafeSnprintf(savepointCommand, sizeof(savepointCommand),
				 "RELEASE SAVEPOINT savepoint_%u", subId);

	if (!SendRemoteCommand(connection, savepointCommand))
	{
		HandleRemoteTransactionConnectionError(connection, raiseErrors);
	}
}


/*
 * FinishRemoteTransactionSavepointRelease finishes the work
 * StartRemoteTransactionSavepointRelease initiated. It blocks if necessary (i.e.
 * if PQisBusy() would return true).
 */
static void
FinishRemoteTransactionSavepointRelease(MultiConnection *connection,
										SubTransactionId subId)
{
	const bool raiseErrors = true;
	PGresult *result = GetRemoteCommandResult(connection, raiseErrors);
	if (!IsResponseOK(result))
	{
		HandleRemoteTransactionResultError(connection, result, raiseErrors);
	}

	PQclear(result);
	ForgetResults(connection);
}


/*
 * StartRemoteTransactionSavepointRollback initiates ROLLBACK TO SAVEPOINT command
 * for the given subtransaction id in a non-blocking manner.
 */
static void
StartRemoteTransactionSavepointRollback(MultiConnection *connection,
										SubTransactionId subId)
{
	const bool raiseErrors = false;

	/*
	 * We need to allocate 43 bytes for command buffer (including '\0'):
	 *  - len("ROLLBACK TO SAVEPOINT savepoint_") = 32
	 *  - maximum length of str(subId) = 10
	 */
	char savepointCommand[43];
	SafeSnprintf(savepointCommand, sizeof(savepointCommand),
				 "ROLLBACK TO SAVEPOINT savepoint_%u", subId);

	if (!SendRemoteCommand(connection, savepointCommand))
	{
		HandleRemoteTransactionConnectionError(connection, raiseErrors);
	}
}


/*
 * FinishRemoteTransactionSavepointRollback finishes the work
 * StartRemoteTransactionSavepointRollback initiated. It blocks if necessary (i.e.
 * if PQisBusy() would return true). It also recovers the transaction from failure
 * if transaction is recovering and the rollback command succeeds.
 */
static void
FinishRemoteTransactionSavepointRollback(MultiConnection *connection, SubTransactionId
										 subId)
{
	const bool raiseErrors = false;
	RemoteTransaction *transaction = &connection->remoteTransaction;

	PGresult *result = GetRemoteCommandResult(connection, raiseErrors);
	if (!IsResponseOK(result))
	{
		HandleRemoteTransactionResultError(connection, result, raiseErrors);
	}

	/* ROLLBACK TO SAVEPOINT succeeded, check if it recovers the transaction */
	else if (transaction->transactionRecovering)
	{
		transaction->transactionFailed = false;
		transaction->transactionRecovering = false;
	}

	PQclear(result);
	ForgetResults(connection);

	/* reset transaction state so the executor can accept next commands in transaction */
	transaction->transactionState = REMOTE_TRANS_STARTED;
}


/*
 * CheckRemoteTransactionsHealth checks if any of the participating transactions in a
 * coordinated transaction failed, and what consequence that should have.
 * This needs to be called before the coordinated transaction commits (but
 * after they've been PREPAREd if 2PC is in use).
 */
void
CheckRemoteTransactionsHealth(void)
{
	dlist_iter iter;

	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;
		PGTransactionStatusType status = PQtransactionStatus(connection->pgConn);

		/* if the connection is in a bad state, so is the transaction's state */
		if (status == PQTRANS_INERROR || status == PQTRANS_UNKNOWN)
		{
			transaction->transactionFailed = true;
		}

		/*
		 * If a critical connection is marked as failed (and no error has been
		 * raised yet) do so now.
		 */
		if (transaction->transactionFailed && transaction->transactionCritical)
		{
			ereport(ERROR, (errmsg("failure on connection marked as essential: %s:%d",
								   connection->hostname, connection->port)));
		}
	}
}


/*
 * Assign2PCIdentifier computes the 2PC transaction name to use for a
 * transaction. Every prepared transaction should get a new name, i.e. this
 * function will need to be called again.
 *
 * The format of the name is:
 *
 * citus_<source group>_<pid>_<distributed transaction number>_<connection number>
 *
 * (at most 5+1+10+1+10+1+20+1+10 = 59 characters, while limit is 64)
 *
 * The source group is used to distinguish 2PCs started by different
 * coordinators. A coordinator will only attempt to recover its own 2PCs.
 *
 * The pid is used to distinguish different processes on the coordinator, mainly
 * to provide some entropy across restarts.
 *
 * The distributed transaction number is used to distinguish different
 * transactions originating from the same node (since restart).
 *
 * The connection number is used to distinguish connections made to a node
 * within the same transaction.
 *
 */
static void
Assign2PCIdentifier(MultiConnection *connection)
{
	/* local sequence number used to distinguish different connections */
	static uint32 connectionNumber = 0;

	/* transaction identifier that is unique across processes */
	uint64 transactionNumber = CurrentDistributedTransactionNumber();

	/* print all numbers as unsigned to guarantee no minus symbols appear in the name */
	SafeSnprintf(connection->remoteTransaction.preparedName, NAMEDATALEN,
				 PREPARED_TRANSACTION_NAME_FORMAT, GetLocalGroupId(), MyProcPid,
				 transactionNumber, connectionNumber++);
}


/*
 * ParsePreparedTransactionName parses a prepared transaction name to extract
 * the initiator group ID, initiator process ID, distributed transaction number,
 * and the connection number. If the transaction name does not match the expected
 * format ParsePreparedTransactionName returns false, and true otherwise.
 */
bool
ParsePreparedTransactionName(char *preparedTransactionName,
							 int32 *groupId, int *procId,
							 uint64 *transactionNumber,
							 uint32 *connectionNumber)
{
	char *currentCharPointer = preparedTransactionName;

	currentCharPointer = strchr(currentCharPointer, '_');
	if (currentCharPointer == NULL)
	{
		return false;
	}

	/* step ahead of the current '_' character */
	++currentCharPointer;

	*groupId = strtol(currentCharPointer, NULL, 10);

	if ((*groupId == COORDINATOR_GROUP_ID && errno == EINVAL) ||
		(*groupId == INT_MAX && errno == ERANGE))
	{
		return false;
	}

	currentCharPointer = strchr(currentCharPointer, '_');
	if (currentCharPointer == NULL)
	{
		return false;
	}

	/* step ahead of the current '_' character */
	++currentCharPointer;

	*procId = strtol(currentCharPointer, NULL, 10);
	if ((*procId == 0 && errno == EINVAL) ||
		(*procId == INT_MAX && errno == ERANGE))
	{
		return false;
	}

	currentCharPointer = strchr(currentCharPointer, '_');
	if (currentCharPointer == NULL)
	{
		return false;
	}

	/* step ahead of the current '_' character */
	++currentCharPointer;

	*transactionNumber = strtou64(currentCharPointer, NULL, 10);
	if ((*transactionNumber == 0 && errno != 0) ||
		(*transactionNumber == ULLONG_MAX && errno == ERANGE))
	{
		return false;
	}

	currentCharPointer = strchr(currentCharPointer, '_');
	if (currentCharPointer == NULL)
	{
		return false;
	}

	/* step ahead of the current '_' character */
	++currentCharPointer;

	*connectionNumber = strtoul(currentCharPointer, NULL, 10);
	if ((*connectionNumber == 0 && errno == EINVAL) ||
		(*connectionNumber == UINT_MAX && errno == ERANGE))
	{
		return false;
	}

	return true;
}
