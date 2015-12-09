/*-------------------------------------------------------------------------
 *
 * execRemote.h
 *
 *	  Functions to execute commands on multiple Datanodes
 *
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 * src/include/pgxc/execRemote.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef EXECREMOTE_H
#define EXECREMOTE_H
#include "locator.h"
#include "nodes/nodes.h"
#include "pgxcnode.h"
#include "planner.h"
#ifdef XCP
#include "squeue.h"
#include "remotecopy.h"
#endif
#include "access/tupdesc.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "tcop/dest.h"
#include "tcop/pquery.h"
#include "utils/snapshot.h"

/* GUC parameters */
extern bool EnforceTwoPhaseCommit;

/* Outputs of handle_response() */
#define RESPONSE_EOF EOF
#define RESPONSE_COMPLETE 0
#define RESPONSE_SUSPENDED 1
#define RESPONSE_TUPDESC 2
#define RESPONSE_DATAROW 3
#define RESPONSE_COPY 4
#define RESPONSE_BARRIER_OK 5
#ifdef XCP
#define RESPONSE_ERROR 6
#define RESPONSE_READY 10
#define RESPONSE_WAITXIDS 11
#define RESPONSE_ASSIGN_GXID 12
#endif

typedef enum
{
	REQUEST_TYPE_NOT_DEFINED,	/* not determined yet */
	REQUEST_TYPE_COMMAND,		/* OK or row count response */
	REQUEST_TYPE_QUERY,			/* Row description response */
	REQUEST_TYPE_COPY_IN,		/* Copy In response */
	REQUEST_TYPE_COPY_OUT,		/* Copy Out response */
	REQUEST_TYPE_ERROR			/* Error, ignore responses */
}	RequestType;

/*
 * Type of requests associated to a remote COPY OUT
 */
typedef enum
{
	REMOTE_COPY_NONE,		/* Not defined yet */
	REMOTE_COPY_STDOUT,		/* Send back to client */
	REMOTE_COPY_FILE,		/* Write in file */
	REMOTE_COPY_TUPLESTORE	/* Store data in tuplestore */
} RemoteCopyType;

/* Combines results of INSERT statements using multiple values */
typedef struct CombineTag
{
	CmdType cmdType;						/* DML command type */
	char	data[COMPLETION_TAG_BUFSIZE];	/* execution result combination data */
} CombineTag;

/*
 * Common part for all plan state nodes needed to access remote datanodes
 * ResponseCombiner must be the first field of the plan state node so we can
 * typecast
 */
typedef struct ResponseCombiner
{
	ScanState	ss;						/* its first field is NodeTag */
	int			node_count;				/* total count of participating nodes */
	PGXCNodeHandle **connections;		/* Datanode connections being combined */
	int			conn_count;				/* count of active connections */
	int			current_conn;			/* used to balance load when reading from connections */
	CombineType combine_type;			/* see CombineType enum */
	int			command_complete_count; /* count of received CommandComplete messages */
	RequestType request_type;			/* see RequestType enum */
	TupleDesc	tuple_desc;				/* tuple descriptor to be referenced by emitted tuples */
	int			description_count;		/* count of received RowDescription messages */
	int			copy_in_count;			/* count of received CopyIn messages */
	int			copy_out_count;			/* count of received CopyOut messages */
	FILE	   *copy_file;      		/* used if copy_dest == COPY_FILE */
	uint64		processed;				/* count of data rows handled */
	char		errorCode[5];			/* error code to send back to client */
	char	   *errorMessage;			/* error message to send back to client */
	char	   *errorDetail;			/* error detail to send back to client */
	char	   *errorHint;				/* error hint to send back to client */
	Oid			returning_node;			/* returning replicated node */
	RemoteDataRow currentRow;			/* next data ro to be wrapped into a tuple */
	/* TODO use a tuplestore as a rowbuffer */
	List 	   *rowBuffer;				/* buffer where rows are stored when connection
										 * should be cleaned for reuse by other RemoteQuery */
	/*
	 * To handle special case - if there is a simple sort and sort connection
	 * is buffered. If EOF is reached on a connection it should be removed from
	 * the array, but we need to know node number of the connection to find
	 * messages in the buffer. So we store nodenum to that array if reach EOF
	 * when buffering
	 */
	Oid 	   *tapenodes;
	/*
	 * If some tape (connection) is buffered, contains a reference on the cell
	 * right before first row buffered from this tape, needed to speed up
	 * access to the data
	 */
	ListCell  **tapemarks;
	bool		merge_sort;             /* perform mergesort of node tuples */
	bool		extended_query;         /* running extended query protocol */
	bool		probing_primary;		/* trying replicated on primary node */
	void	   *tuplesortstate;			/* for merge sort */
	/* COPY support */
	RemoteCopyType remoteCopyType;
	Tuplestorestate *tuplestorestate;
	/* cursor support */
	char	   *cursor;					/* cursor name */
	char	   *update_cursor;			/* throw this cursor current tuple can be updated */
	int			cursor_count;			/* total count of participating nodes */
	PGXCNodeHandle **cursor_connections;/* data node connections being combined */
}	ResponseCombiner;

typedef struct RemoteQueryState
{
	ResponseCombiner combiner;			/* see ResponseCombiner struct */
	bool		query_Done;				/* query has been sent down to Datanodes */
	/*
	 * While we are not supporting grouping use this flag to indicate we need
	 * to initialize collecting of aggregates from the DNs
	 */
	bool		initAggregates;
	/* Simple DISTINCT support */
	FmgrInfo   *eqfunctions; 			/* functions to compare tuples */
	MemoryContext tmp_ctx;				/* separate context is needed to compare tuples */
	/* Support for parameters */
	char	   *paramval_data;		/* parameter data, format is like in BIND */
	int			paramval_len;		/* length of parameter values data */

	int			eflags;			/* capability flags to pass to tuplestore */
	bool		eof_underlying; /* reached end of underlying plan? */
}	RemoteQueryState;

typedef struct RemoteParam
{
	ParamKind 	paramkind;		/* kind of parameter */
	int			paramid;		/* numeric ID for parameter */
	Oid			paramtype;		/* pg_type OID of parameter's datatype */
} RemoteParam;


/*
 * Execution state of a RemoteSubplan node
 */
typedef struct RemoteSubplanState
{
	ResponseCombiner combiner;			/* see ResponseCombiner struct */
	char	   *subplanstr;				/* subplan encoded as a string */
	bool		bound;					/* subplan is sent down to the nodes */
	bool		local_exec; 			/* execute subplan on this datanode */
	Locator    *locator;				/* determine destination of tuples of
										 * locally executed plan */
	int 	   *dest_nodes;				/* allocate once */
	List	   *execNodes;				/* where to execute subplan */
	/* should query be executed on all (true) or any (false) node specified
	 * in the execNodes list */
	bool 		execOnAll;
	int			nParamRemote;	/* number of params sent from the master node */
	RemoteParam *remoteparams;  /* parameter descriptors */
} RemoteSubplanState;


/*
 * Data needed to set up a PreparedStatement on the remote node and other data
 * for the remote executor
 */
typedef struct RemoteStmt
{
	NodeTag		type;

	CmdType		commandType;	/* select|insert|update|delete */

	bool		hasReturning;	/* is it insert|update|delete RETURNING? */

	struct Plan *planTree;				/* tree of Plan nodes */

	List	   *rtable;					/* list of RangeTblEntry nodes */

	/* rtable indexes of target relations for INSERT/UPDATE/DELETE */
	List	   *resultRelations;	/* integer list of RT indexes, or NIL */

	List	   *subplans;		/* Plan trees for SubPlan expressions */

	int			nParamExec;		/* number of PARAM_EXEC Params used */

	int			nParamRemote;	/* number of params sent from the master node */

	RemoteParam *remoteparams;  /* parameter descriptors */

	List	   *rowMarks;

	char		distributionType;

	AttrNumber	distributionKey;

	List	   *distributionNodes;

	List	   *distributionRestrict;
} RemoteStmt;

typedef void (*xact_callback) (bool isCommit, void *args);

/* Copy command just involves Datanodes */
extern void DataNodeCopyBegin(RemoteCopyData *rcstate);
extern int DataNodeCopyIn(char *data_row, int len, int conn_count,
						  PGXCNodeHandle** copy_connections);
extern uint64 DataNodeCopyOut(PGXCNodeHandle** copy_connections,
							  int conn_count, FILE* copy_file);
extern uint64 DataNodeCopyStore(PGXCNodeHandle** copy_connections,
								int conn_count, Tuplestorestate* store);
extern void DataNodeCopyFinish(int conn_count, PGXCNodeHandle** connections);
extern int DataNodeCopyInBinaryForAll(char *msg_buf, int len, int conn_count,
									  PGXCNodeHandle** connections);
extern bool DataNodeCopyEnd(PGXCNodeHandle *handle, bool is_error);

extern RemoteQueryState *ExecInitRemoteQuery(RemoteQuery *node, EState *estate, int eflags);
extern TupleTableSlot* ExecRemoteQuery(RemoteQueryState *step);
extern void ExecEndRemoteQuery(RemoteQueryState *step);
extern void RemoteSubplanMakeUnique(Node *plan, int unique);
extern RemoteSubplanState *ExecInitRemoteSubplan(RemoteSubplan *node, EState *estate, int eflags);
extern void ExecFinishInitRemoteSubplan(RemoteSubplanState *node);
extern TupleTableSlot* ExecRemoteSubplan(RemoteSubplanState *node);
extern void ExecEndRemoteSubplan(RemoteSubplanState *node);
extern void ExecReScanRemoteSubplan(RemoteSubplanState *node);
extern void ExecRemoteUtility(RemoteQuery *node);

extern bool	is_data_node_ready(PGXCNodeHandle * conn);

extern int handle_response(PGXCNodeHandle *conn, ResponseCombiner *combiner);
extern void HandleCmdComplete(CmdType commandType, CombineTag *combine, const char *msg_body,
									size_t len);

#define CHECK_OWNERSHIP(conn, node) \
	do { \
		if ((conn)->state == DN_CONNECTION_STATE_QUERY && \
				(conn)->combiner && \
				(conn)->combiner != (ResponseCombiner *) (node)) \
			BufferConnection(conn); \
		(conn)->combiner = (ResponseCombiner *) (node); \
	} while(0)

extern TupleTableSlot *FetchTuple(ResponseCombiner *combiner);
extern void InitResponseCombiner(ResponseCombiner *combiner, int node_count,
					   CombineType combine_type);
extern void CloseCombiner(ResponseCombiner *combiner);
extern void BufferConnection(PGXCNodeHandle *conn);

extern void ExecRemoteQueryReScan(RemoteQueryState *node, ExprContext *exprCtxt);

extern int ParamListToDataRow(ParamListInfo params, char** result);

extern void ExecCloseRemoteStatement(const char *stmt_name, List *nodelist);
extern char *PrePrepare_Remote(char *prepareGID, bool localNode, bool implicit);
extern void PostPrepare_Remote(char *prepareGID, bool implicit);
extern void PreCommit_Remote(char *prepareGID, char *nodestring, bool preparedLocalNode);
extern bool	PreAbort_Remote(void);
extern void AtEOXact_Remote(void);
extern bool IsTwoPhaseCommitRequired(bool localWrite);
extern bool FinishRemotePreparedTransaction(char *prepareGID, bool commit);

extern void pgxc_all_success_nodes(ExecNodes **d_nodes, ExecNodes **c_nodes, char **failednodes_msg);
extern void AtEOXact_DBCleanup(bool isCommit);

extern void set_dbcleanup_callback(xact_callback function, void *paraminfo, int paraminfo_size);

#endif
