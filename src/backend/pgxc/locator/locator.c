/*-------------------------------------------------------------------------
 *
 * locator.c
 *		Functions that help manage table location information such as
 * partitioning and replication information.
 *
 *
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 *
 * IDENTIFICATION
 *		$$
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "postgres.h"
#include "access/skey.h"
#include "access/gtm.h"
#include "access/relscan.h"
#include "catalog/indexing.h"
#include "catalog/pg_type.h"
#include "nodes/pg_list.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/tqual.h"
#include "utils/syscache.h"
#include "nodes/nodes.h"
#include "optimizer/clauses.h"
#include "parser/parse_coerce.h"
#include "pgxc/nodemgr.h"
#include "pgxc/locator.h"
#include "pgxc/pgxc.h"
#include "pgxc/pgxcnode.h"

#include "catalog/pgxc_class.h"
#include "catalog/pgxc_node.h"
#include "catalog/namespace.h"
#include "access/hash.h"
#ifdef XCP
#include "utils/date.h"
#include "utils/memutils.h"

/*
 * Locator details are private
 */
struct _Locator
{
	/*
	 * Determine target nodes for value.
	 * Resulting nodes are stored to the results array.
	 * Function returns number of node references written to the array.
	 */
	int			(*locatefunc) (Locator *self, Datum value, bool isnull,
								bool *hasprimary);
	Oid			dataType; 		/* values of that type are passed to locateNodes function */
	LocatorListType listType;
	bool		primary;
	/* locator-specific data */
	/* XXX: move them into union ? */
	int			roundRobinNode; /* for LOCATOR_TYPE_RROBIN */
	LocatorHashFunc	hashfunc; /* for LOCATOR_TYPE_HASH */
	int 		valuelen; /* 1, 2 or 4 for LOCATOR_TYPE_MODULO */

	int			nodeCount; /* How many nodes are in the map */
	void	   *nodeMap; /* map index to node reference according to listType */
	void	   *results; /* array to output results */
};
#endif

Oid		primary_data_node = InvalidOid;
int		num_preferred_data_nodes = 0;
Oid		preferred_data_node[MAX_PREFERRED_NODES];

#ifdef XCP
static int modulo_value_len(Oid dataType);
static LocatorHashFunc hash_func_ptr(Oid dataType);
static int locate_static(Locator *self, Datum value, bool isnull,
			  bool *hasprimary);
static int locate_roundrobin(Locator *self, Datum value, bool isnull,
			  bool *hasprimary);
static int locate_hash_insert(Locator *self, Datum value, bool isnull,
			  bool *hasprimary);
static int locate_hash_select(Locator *self, Datum value, bool isnull,
			  bool *hasprimary);
static int locate_modulo_insert(Locator *self, Datum value, bool isnull,
			  bool *hasprimary);
static int locate_modulo_select(Locator *self, Datum value, bool isnull,
			  bool *hasprimary);
#endif

static const unsigned int xc_mod_m[] =
{
  0x00000000, 0x55555555, 0x33333333, 0xc71c71c7,
  0x0f0f0f0f, 0xc1f07c1f, 0x3f03f03f, 0xf01fc07f,
  0x00ff00ff, 0x07fc01ff, 0x3ff003ff, 0xffc007ff,
  0xff000fff, 0xfc001fff, 0xf0003fff, 0xc0007fff,
  0x0000ffff, 0x0001ffff, 0x0003ffff, 0x0007ffff,
  0x000fffff, 0x001fffff, 0x003fffff, 0x007fffff,
  0x00ffffff, 0x01ffffff, 0x03ffffff, 0x07ffffff,
  0x0fffffff, 0x1fffffff, 0x3fffffff, 0x7fffffff
};

static const unsigned int xc_mod_q[][6] =
{
  { 0,  0,  0,  0,  0,  0}, {16,  8,  4,  2,  1,  1}, {16,  8,  4,  2,  2,  2},
  {15,  6,  3,  3,  3,  3}, {16,  8,  4,  4,  4,  4}, {15,  5,  5,  5,  5,  5},
  {12,  6,  6,  6 , 6,  6}, {14,  7,  7,  7,  7,  7}, {16,  8,  8,  8,  8,  8},
  { 9,  9,  9,  9,  9,  9}, {10, 10, 10, 10, 10, 10}, {11, 11, 11, 11, 11, 11},
  {12, 12, 12, 12, 12, 12}, {13, 13, 13, 13, 13, 13}, {14, 14, 14, 14, 14, 14},
  {15, 15, 15, 15, 15, 15}, {16, 16, 16, 16, 16, 16}, {17, 17, 17, 17, 17, 17},
  {18, 18, 18, 18, 18, 18}, {19, 19, 19, 19, 19, 19}, {20, 20, 20, 20, 20, 20},
  {21, 21, 21, 21, 21, 21}, {22, 22, 22, 22, 22, 22}, {23, 23, 23, 23, 23, 23},
  {24, 24, 24, 24, 24, 24}, {25, 25, 25, 25, 25, 25}, {26, 26, 26, 26, 26, 26},
  {27, 27, 27, 27, 27, 27}, {28, 28, 28, 28, 28, 28}, {29, 29, 29, 29, 29, 29},
  {30, 30, 30, 30, 30, 30}, {31, 31, 31, 31, 31, 31}
};

static const unsigned int xc_mod_r[][6] =
{
  {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
  {0x0000ffff, 0x000000ff, 0x0000000f, 0x00000003, 0x00000001, 0x00000001},
  {0x0000ffff, 0x000000ff, 0x0000000f, 0x00000003, 0x00000003, 0x00000003},
  {0x00007fff, 0x0000003f, 0x00000007, 0x00000007, 0x00000007, 0x00000007},
  {0x0000ffff, 0x000000ff, 0x0000000f, 0x0000000f, 0x0000000f, 0x0000000f},
  {0x00007fff, 0x0000001f, 0x0000001f, 0x0000001f, 0x0000001f, 0x0000001f},
  {0x00000fff, 0x0000003f, 0x0000003f, 0x0000003f, 0x0000003f, 0x0000003f},
  {0x00003fff, 0x0000007f, 0x0000007f, 0x0000007f, 0x0000007f, 0x0000007f},
  {0x0000ffff, 0x000000ff, 0x000000ff, 0x000000ff, 0x000000ff, 0x000000ff},
  {0x000001ff, 0x000001ff, 0x000001ff, 0x000001ff, 0x000001ff, 0x000001ff},
  {0x000003ff, 0x000003ff, 0x000003ff, 0x000003ff, 0x000003ff, 0x000003ff},
  {0x000007ff, 0x000007ff, 0x000007ff, 0x000007ff, 0x000007ff, 0x000007ff},
  {0x00000fff, 0x00000fff, 0x00000fff, 0x00000fff, 0x00000fff, 0x00000fff},
  {0x00001fff, 0x00001fff, 0x00001fff, 0x00001fff, 0x00001fff, 0x00001fff},
  {0x00003fff, 0x00003fff, 0x00003fff, 0x00003fff, 0x00003fff, 0x00003fff},
  {0x00007fff, 0x00007fff, 0x00007fff, 0x00007fff, 0x00007fff, 0x00007fff},
  {0x0000ffff, 0x0000ffff, 0x0000ffff, 0x0000ffff, 0x0000ffff, 0x0000ffff},
  {0x0001ffff, 0x0001ffff, 0x0001ffff, 0x0001ffff, 0x0001ffff, 0x0001ffff},
  {0x0003ffff, 0x0003ffff, 0x0003ffff, 0x0003ffff, 0x0003ffff, 0x0003ffff},
  {0x0007ffff, 0x0007ffff, 0x0007ffff, 0x0007ffff, 0x0007ffff, 0x0007ffff},
  {0x000fffff, 0x000fffff, 0x000fffff, 0x000fffff, 0x000fffff, 0x000fffff},
  {0x001fffff, 0x001fffff, 0x001fffff, 0x001fffff, 0x001fffff, 0x001fffff},
  {0x003fffff, 0x003fffff, 0x003fffff, 0x003fffff, 0x003fffff, 0x003fffff},
  {0x007fffff, 0x007fffff, 0x007fffff, 0x007fffff, 0x007fffff, 0x007fffff},
  {0x00ffffff, 0x00ffffff, 0x00ffffff, 0x00ffffff, 0x00ffffff, 0x00ffffff},
  {0x01ffffff, 0x01ffffff, 0x01ffffff, 0x01ffffff, 0x01ffffff, 0x01ffffff},
  {0x03ffffff, 0x03ffffff, 0x03ffffff, 0x03ffffff, 0x03ffffff, 0x03ffffff},
  {0x07ffffff, 0x07ffffff, 0x07ffffff, 0x07ffffff, 0x07ffffff, 0x07ffffff},
  {0x0fffffff, 0x0fffffff, 0x0fffffff, 0x0fffffff, 0x0fffffff, 0x0fffffff},
  {0x1fffffff, 0x1fffffff, 0x1fffffff, 0x1fffffff, 0x1fffffff, 0x1fffffff},
  {0x3fffffff, 0x3fffffff, 0x3fffffff, 0x3fffffff, 0x3fffffff, 0x3fffffff},
  {0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff}
};

/*
 * GetAnyDataNode
 * Pick any data node from given set, but try a preferred node
 */
int
GetAnyDataNode(Bitmapset *nodes)
{
	Bitmapset  *preferred = NULL;
	int			i, nodeid;
	int			nmembers = 0;
	int			members[NumDataNodes];

	for (i = 0; i < num_preferred_data_nodes; i++)
	{
		char ntype = PGXC_NODE_DATANODE;
		nodeid = PGXCNodeGetNodeId(preferred_data_node[i], &ntype);

		/* OK, found one */
		if (bms_is_member(nodeid, nodes))
			preferred = bms_add_member(preferred, nodeid);
	}

	/*
	 * If no preferred data nodes or they are not in the desired set, pick up
	 * from the original set.
	 */
	if (bms_is_empty(preferred))
		preferred = bms_copy(nodes);

	/*
	 * Load balance.
	 * We can not get item from the set, convert it to array
	 */
	while ((nodeid = bms_first_member(preferred)) >= 0)
		members[nmembers++] = nodeid;
	bms_free(preferred);

	/* If there is a single member nothing to balance */
	if (nmembers == 1)
		return members[0];

	/*
	 * In general, the set may contain any number of nodes, and if we save
	 * previous returned index for load balancing the distribution won't be
	 * flat, because small set will probably reset saved value, and lower
	 * indexes will be picked up more often.
	 * So we just get a random value from 0..nmembers-1.
	 */
	return members[((unsigned int) random()) % nmembers];
}

/*
 * compute_modulo
 * This function performs modulo in an optimized way
 * It optimizes modulo of any positive number by
 * 1,2,3,4,7,8,15,16,31,32,63,64 and so on
 * for the rest of the denominators it uses % operator
 * The optimized algos have been taken from
 * http://www-graphics.stanford.edu/~seander/bithacks.html
 */
static int
compute_modulo(unsigned int numerator, unsigned int denominator)
{
	unsigned int d;
	unsigned int m;
	unsigned int s;
	unsigned int mask;
	int k;
	unsigned int q, r;

	if (numerator == 0)
		return 0;

	/* Check if denominator is a power of 2 */
	if ((denominator & (denominator - 1)) == 0)
		return numerator & (denominator - 1);

	/* Check if (denominator+1) is a power of 2 */
	d = denominator + 1;
	if ((d & (d - 1)) == 0)
	{
		/* Which power of 2 is this number */
		s = 0;
		mask = 0x01;
		for (k = 0; k < 32; k++)
		{
			if ((d & mask) == mask)
				break;
			s++;
			mask = mask << 1;
		}

		m = (numerator & xc_mod_m[s]) + ((numerator >> s) & xc_mod_m[s]);

		for (q = 0, r = 0; m > denominator; q++, r++)
			m = (m >> xc_mod_q[s][q]) + (m & xc_mod_r[s][r]);

		m = m == denominator ? 0 : m;

		return m;
	}
	return numerator % denominator;
}

/*
 * GetRelationDistColumn - Returns the name of the hash or modulo distribution column
 * First hash distribution is checked
 * Retuens NULL if the table is neither hash nor modulo distributed
 */
char *
GetRelationDistColumn(RelationLocInfo * rel_loc_info)
{
char *pColName;

	pColName = NULL;

	pColName = GetRelationHashColumn(rel_loc_info);
	if (pColName == NULL)
		pColName = GetRelationModuloColumn(rel_loc_info);

	return pColName;
}

/*
 * Returns whether or not the data type is hash distributable with PG-XC
 * PGXCTODO - expand support for other data types!
 */
bool
IsTypeHashDistributable(Oid col_type)
{
	return (hash_func_ptr(col_type) != NULL);
}

/*
 * GetRelationHashColumn - return hash column for relation.
 *
 * Returns NULL if the relation is not hash partitioned.
 */
char *
GetRelationHashColumn(RelationLocInfo * rel_loc_info)
{
	char	   *column_str = NULL;

	if (rel_loc_info == NULL)
		column_str = NULL;
	else if (rel_loc_info->locatorType != LOCATOR_TYPE_HASH)
		column_str = NULL;
	else
	{
		int			len = strlen(rel_loc_info->partAttrName);

		column_str = (char *) palloc(len + 1);
		strncpy(column_str, rel_loc_info->partAttrName, len + 1);
	}

	return column_str;
}

/*
 * IsHashColumn - return whether or not column for relation is hashed.
 *
 */
bool
IsHashColumn(RelationLocInfo *rel_loc_info, char *part_col_name)
{
	bool		ret_value = false;

	if (!rel_loc_info || !part_col_name)
		ret_value = false;
	else if (rel_loc_info->locatorType != LOCATOR_TYPE_HASH)
		ret_value = false;
	else
		ret_value = !strcmp(part_col_name, rel_loc_info->partAttrName);

	return ret_value;
}


/*
 * IsHashColumnForRelId - return whether or not column for relation is hashed.
 *
 */
bool
IsHashColumnForRelId(Oid relid, char *part_col_name)
{
	RelationLocInfo *rel_loc_info = GetRelationLocInfo(relid);

	return IsHashColumn(rel_loc_info, part_col_name);
}

/*
 * IsDistColumnForRelId - return whether or not column for relation is used for hash or modulo distribution
 *
 */
bool
IsDistColumnForRelId(Oid relid, char *part_col_name)
{
	bool bRet;
	RelationLocInfo *rel_loc_info;

	rel_loc_info = GetRelationLocInfo(relid);
	bRet = false;

	bRet = IsHashColumn(rel_loc_info, part_col_name);
	if (bRet == false)
		IsModuloColumn(rel_loc_info, part_col_name);
	return bRet;
}


/*
 * Returns whether or not the data type is modulo distributable with PG-XC
 * PGXCTODO - expand support for other data types!
 */
bool
IsTypeModuloDistributable(Oid col_type)
{
	return (modulo_value_len(col_type) != -1);
}

/*
 * GetRelationModuloColumn - return modulo column for relation.
 *
 * Returns NULL if the relation is not modulo partitioned.
 */
char *
GetRelationModuloColumn(RelationLocInfo * rel_loc_info)
{
	char	   *column_str = NULL;

	if (rel_loc_info == NULL)
		column_str = NULL;
	else if (rel_loc_info->locatorType != LOCATOR_TYPE_MODULO)
		column_str = NULL;
	else
	{
		int	len = strlen(rel_loc_info->partAttrName);

		column_str = (char *) palloc(len + 1);
		strncpy(column_str, rel_loc_info->partAttrName, len + 1);
	}

	return column_str;
}

/*
 * IsModuloColumn - return whether or not column for relation is used for modulo distribution.
 *
 */
bool
IsModuloColumn(RelationLocInfo *rel_loc_info, char *part_col_name)
{
	bool		ret_value = false;

	if (!rel_loc_info || !part_col_name)
		ret_value = false;
	else if (rel_loc_info->locatorType != LOCATOR_TYPE_MODULO)
		ret_value = false;
	else
		ret_value = !strcmp(part_col_name, rel_loc_info->partAttrName);

	return ret_value;
}


/*
 * IsModuloColumnForRelId - return whether or not column for relation is used for modulo distribution.
 */
bool
IsModuloColumnForRelId(Oid relid, char *part_col_name)
{
	RelationLocInfo *rel_loc_info = GetRelationLocInfo(relid);

	return IsModuloColumn(rel_loc_info, part_col_name);
}

/*
 * Update the round robin node for the relation
 *
 * PGXCTODO - may not want to bother with locking here, we could track
 * these in the session memory context instead...
 */
int
GetRoundRobinNode(Oid relid)
{
	int			ret_node;
	Relation	rel = relation_open(relid, AccessShareLock);

    Assert (IsLocatorReplicated(rel->rd_locator_info->locatorType) ||
			rel->rd_locator_info->locatorType == LOCATOR_TYPE_RROBIN);

	ret_node = lfirst_int(rel->rd_locator_info->roundRobinNode);

	/* Move round robin indicator to next node */
	if (rel->rd_locator_info->roundRobinNode->next != NULL)
		rel->rd_locator_info->roundRobinNode = rel->rd_locator_info->roundRobinNode->next;
	else
		/* reset to first one */
		rel->rd_locator_info->roundRobinNode = rel->rd_locator_info->nodeList->head;

	relation_close(rel, AccessShareLock);

	return ret_node;
}

/*
 * IsTableDistOnPrimary
 *
 * Does the table distribution list include the primary node?
 */
bool
IsTableDistOnPrimary(RelationLocInfo *rel_loc_info)
{
	ListCell *item;

	if (!OidIsValid(primary_data_node) ||
		rel_loc_info == NULL ||
		list_length(rel_loc_info->nodeList = 0))
		return false;

	foreach(item, rel_loc_info->nodeList)
	{
		char ntype = PGXC_NODE_DATANODE;
		if (PGXCNodeGetNodeId(primary_data_node, &ntype) == lfirst_int(item))
			return true;
	}
	return false;
}


/*
 * IsLocatorInfoEqual
 * Check equality of given locator information
 */
bool
IsLocatorInfoEqual(RelationLocInfo *rel_loc_info1, RelationLocInfo *rel_loc_info2)
{
	List *nodeList1, *nodeList2;
	Assert(rel_loc_info1 && rel_loc_info2);

	nodeList1 = rel_loc_info1->nodeList;
	nodeList2 = rel_loc_info2->nodeList;

	/* Same relation? */
	if (rel_loc_info1->relid != rel_loc_info2->relid)
		return false;

	/* Same locator type? */
	if (rel_loc_info1->locatorType != rel_loc_info2->locatorType)
		return false;

	/* Same attribute number? */
	if (rel_loc_info1->partAttrNum != rel_loc_info2->partAttrNum)
		return false;

	/* Same node list? */
	if (list_difference_int(nodeList1, nodeList2) != NIL ||
		list_difference_int(nodeList2, nodeList1) != NIL)
		return false;

	/* Everything is equal */
	return true;
}

/*
 * ConvertToLocatorType
 *		get locator distribution type
 * We really should just have pgxc_class use disttype instead...
 */
char
ConvertToLocatorType(int disttype)
{
	char		loctype = LOCATOR_TYPE_NONE;

	switch (disttype)
	{
		case DISTTYPE_HASH:
			loctype = LOCATOR_TYPE_HASH;
			break;
		case DISTTYPE_ROUNDROBIN:
			loctype = LOCATOR_TYPE_RROBIN;
			break;
		case DISTTYPE_REPLICATION:
			loctype = LOCATOR_TYPE_REPLICATED;
			break;
		case DISTTYPE_MODULO:
			loctype = LOCATOR_TYPE_MODULO;
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("Invalid distribution type")));
			break;
	}

	return loctype;
}


/*
 * GetLocatorType - Returns the locator type of the table
 *
 */
char
GetLocatorType(Oid relid)
{
	char		ret = '\0';

	RelationLocInfo *ret_loc_info = GetRelationLocInfo(relid);

	if (ret_loc_info != NULL)
		ret = ret_loc_info->locatorType;

	return ret;
}


/*
 * Return a list of all Datanodes.
 * We assume all tables use all nodes in the prototype, so just return a list
 * from first one.
 */
List *
GetAllDataNodes(void)
{
	int			i;
	List	   *nodeList = NIL;

	for (i = 0; i < NumDataNodes; i++)
		nodeList = lappend_int(nodeList, i);

	return nodeList;
}

/*
 * Return a list of all Coordinators
 * This is used to send DDL to all nodes and to clean up pooler connections.
 * Do not put in the list the local Coordinator where this function is launched.
 */
List *
GetAllCoordNodes(void)
{
	int			i;
	List	   *nodeList = NIL;

	for (i = 0; i < NumCoords; i++)
	{
		/*
		 * Do not put in list the Coordinator we are on,
		 * it doesn't make sense to connect to the local Coordinator.
		 */

		if (i != PGXCNodeId - 1)
			nodeList = lappend_int(nodeList, i);
	}

	return nodeList;
}


/*
 * Build locator information associated with the specified relation.
 */
void
RelationBuildLocator(Relation rel)
{
	Relation	pcrel;
	ScanKeyData	skey;
	SysScanDesc	pcscan;
	HeapTuple	htup;
	MemoryContext	oldContext;
	RelationLocInfo	*relationLocInfo;
	int		j;
	Form_pgxc_class	pgxc_class;

	ScanKeyInit(&skey,
				Anum_pgxc_class_pcrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));

	pcrel = heap_open(PgxcClassRelationId, AccessShareLock);
	pcscan = systable_beginscan(pcrel, PgxcClassPgxcRelIdIndexId, true,
								SnapshotSelf, 1, &skey);
	htup = systable_getnext(pcscan);

	if (!HeapTupleIsValid(htup))
	{
		/* Assume local relation only */
		rel->rd_locator_info = NULL;
		systable_endscan(pcscan);
		heap_close(pcrel, AccessShareLock);
		return;
	}

	pgxc_class = (Form_pgxc_class) GETSTRUCT(htup);

	oldContext = MemoryContextSwitchTo(CacheMemoryContext);

	relationLocInfo = (RelationLocInfo *) palloc(sizeof(RelationLocInfo));
	rel->rd_locator_info = relationLocInfo;

	relationLocInfo->relid = RelationGetRelid(rel);
	relationLocInfo->locatorType = pgxc_class->pclocatortype;

	relationLocInfo->partAttrNum = pgxc_class->pcattnum;

	relationLocInfo->partAttrName = get_attname(relationLocInfo->relid, pgxc_class->pcattnum);

	relationLocInfo->nodeList = NIL;

	for (j = 0; j < pgxc_class->nodeoids.dim1; j++)
	{
		char ntype = PGXC_NODE_DATANODE;
		int nid = PGXCNodeGetNodeId(pgxc_class->nodeoids.values[j], &ntype);
		relationLocInfo->nodeList = lappend_int(relationLocInfo->nodeList, nid);
	}

	/*
	 * If the locator type is round robin, we set a node to
	 * use next time. In addition, if it is replicated,
	 * we choose a node to use for balancing reads.
	 */
	if (relationLocInfo->locatorType == LOCATOR_TYPE_RROBIN
		|| IsLocatorReplicated(relationLocInfo->locatorType))
	{
		int offset;
		/*
		 * pick a random one to start with,
		 * since each process will do this independently
		 */
		offset = compute_modulo(abs(rand()), list_length(relationLocInfo->nodeList));

		srand(time(NULL));
		relationLocInfo->roundRobinNode = relationLocInfo->nodeList->head; /* initialize */
		for (j = 0; j < offset && relationLocInfo->roundRobinNode->next != NULL; j++)
			relationLocInfo->roundRobinNode = relationLocInfo->roundRobinNode->next;
	}

	systable_endscan(pcscan);
	heap_close(pcrel, AccessShareLock);

	MemoryContextSwitchTo(oldContext);
}

/*
 * GetLocatorRelationInfo - Returns the locator information for relation,
 * in a copy of the RelationLocatorInfo struct in relcache
 */
RelationLocInfo *
GetRelationLocInfo(Oid relid)
{
	RelationLocInfo *ret_loc_info = NULL;
	Relation	rel = relation_open(relid, AccessShareLock);

	/* Relation needs to be valid */
	Assert(rel->rd_isvalid);

	if (rel->rd_locator_info)
		ret_loc_info = CopyRelationLocInfo(rel->rd_locator_info);

	relation_close(rel, AccessShareLock);

	return ret_loc_info;
}

/*
 * Get the distribution type of relation.
 */
char
GetRelationLocType(Oid relid)
{
	RelationLocInfo *locinfo = GetRelationLocInfo(relid);
	if (!locinfo)
		return LOCATOR_TYPE_NONE;

	return locinfo->locatorType;
}

/*
 * Copy the RelationLocInfo struct
 */
RelationLocInfo *
CopyRelationLocInfo(RelationLocInfo * src_info)
{
	RelationLocInfo *dest_info;

	Assert(src_info);

	dest_info = (RelationLocInfo *) palloc0(sizeof(RelationLocInfo));

	dest_info->relid = src_info->relid;
	dest_info->locatorType = src_info->locatorType;
	dest_info->partAttrNum = src_info->partAttrNum;
	if (src_info->partAttrName)
		dest_info->partAttrName = pstrdup(src_info->partAttrName);

	if (src_info->nodeList)
		dest_info->nodeList = list_copy(src_info->nodeList);
	/* Note, for round robin, we use the relcache entry */

	return dest_info;
}


/*
 * Free RelationLocInfo struct
 */
void
FreeRelationLocInfo(RelationLocInfo *relationLocInfo)
{
	if (relationLocInfo)
	{
		if (relationLocInfo->partAttrName)
			pfree(relationLocInfo->partAttrName);
		pfree(relationLocInfo);
	}
}


/*
 * Free the contents of the ExecNodes expression */
void
FreeExecNodes(ExecNodes **exec_nodes)
{
	ExecNodes *tmp_en = *exec_nodes;

	/* Nothing to do */
	if (!tmp_en)
		return;
	list_free(tmp_en->primarynodelist);
	list_free(tmp_en->nodeList);
	pfree(tmp_en);
	*exec_nodes = NULL;
}


#ifdef XCP
/*
 * Determine value length in bytes for specified type for a module locator.
 * Return -1 if module locator is not supported for the type.
 */
static int
modulo_value_len(Oid dataType)
{
	switch (dataType)
	{
		case BOOLOID:
		case CHAROID:
			return 1;
		case INT2OID:
			return 2;
		case INT4OID:
		case ABSTIMEOID:
		case RELTIMEOID:
		case DATEOID:
			return 4;
		default:
			return -1;
	}
}


static LocatorHashFunc
hash_func_ptr(Oid dataType)
{
	switch (dataType)
	{
		case INT8OID:
		case CASHOID:
			return hashint8;
		case INT2OID:
			return hashint2;
		case OIDOID:
			return hashoid;
		case INT4OID:
		case ABSTIMEOID:
		case RELTIMEOID:
		case DATEOID:
			return hashint4;
		case BOOLOID:
		case CHAROID:
			return hashchar;
		case NAMEOID:
			return hashname;
		case INT2VECTOROID:
			return hashint2vector;
		case VARCHAROID:
		case TEXTOID:
			return hashtext;
		case OIDVECTOROID:
			return hashoidvector;
		case BPCHAROID:
			return hashbpchar;
		case BYTEAOID:
			return hashvarlena;
		case TIMEOID:
			return time_hash;
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return timestamp_hash;
		case INTERVALOID:
			return interval_hash;
		case TIMETZOID:
			return timetz_hash;
		case NUMERICOID:
			return hash_numeric;
		case UUIDOID:
			return uuid_hash;
		default:
			return NULL;
	}
}


Locator *
createLocator(char locatorType, RelationAccessType accessType,
			  Oid dataType, LocatorListType listType, int nodeCount,
			  void *nodeList, void **result, bool primary)
{
	Locator    *locator;
	ListCell   *lc;
	void 	   *nodeMap = NULL;
	int 		i;

	locator = (Locator *) palloc(sizeof(Locator));
	locator->dataType = dataType;
	locator->listType = listType;
	locator->nodeCount = nodeCount;
	/* Create node map */
	switch (listType)
	{
		case LOCATOR_LIST_NONE:
			/* No map, return indexes */
			break;
		case LOCATOR_LIST_INT:
			/* Copy integer array */
			nodeMap = palloc(nodeCount * sizeof(int));
			memcpy(nodeMap, nodeList, nodeCount * sizeof(int));
			break;
		case LOCATOR_LIST_OID:
			/* Copy array of Oids */
			nodeMap = palloc(nodeCount * sizeof(Oid));
			memcpy(nodeMap, nodeList, nodeCount * sizeof(Oid));
			break;
		case LOCATOR_LIST_POINTER:
			/* Copy array of Oids */
			nodeMap = palloc(nodeCount * sizeof(void *));
			memcpy(nodeMap, nodeList, nodeCount * sizeof(void *));
			break;
		case LOCATOR_LIST_LIST:
			/* Create map from list */
		{
			List *l = (List *) nodeList;
			locator->nodeCount = list_length(l);
			if (IsA(l, IntList))
			{
				int *intptr;
				nodeMap = palloc(locator->nodeCount * sizeof(int));
				intptr = (int *) nodeMap;
				foreach(lc, l)
					*intptr++ = lfirst_int(lc);
				locator->listType = LOCATOR_LIST_INT;
			}
			else if (IsA(l, OidList))
			{
				Oid *oidptr;
				nodeMap = palloc(locator->nodeCount * sizeof(Oid));
				oidptr = (Oid *) nodeMap;
				foreach(lc, l)
					*oidptr++ = lfirst_oid(lc);
				locator->listType = LOCATOR_LIST_OID;
			}
			else if (IsA(l, List))
			{
				void **voidptr;
				nodeMap = palloc(locator->nodeCount * sizeof(void *));
				voidptr = (void **) nodeMap;
				foreach(lc, l)
					*voidptr++ = lfirst(lc);
				locator->listType = LOCATOR_LIST_POINTER;
			}
			else
			{
				/* can not get here */
				Assert(false);
			}
			break;
		}
	}
	/*
	 * Determine locatefunc, allocate results, set up parameters
	 * specific to locator type
	 */
	switch (locatorType)
	{
		case LOCATOR_TYPE_REPLICATED:
			if (accessType == RELATION_ACCESS_INSERT ||
					accessType == RELATION_ACCESS_UPDATE)
			{
				locator->locatefunc = locate_static;
				if (nodeMap == NULL)
				{
					/* no map, prepare array with indexes */
					int *intptr;
					nodeMap = palloc(locator->nodeCount * sizeof(int));
					intptr = (int *) nodeMap;
					for (i = 0; i < locator->nodeCount; i++)
						*intptr++ = i;
				}
				locator->nodeMap = nodeMap;
				locator->results = nodeMap;
			}
			else
			{
				locator->locatefunc = locate_roundrobin;
				locator->nodeMap = nodeMap;
				switch (locator->listType)
				{
					case LOCATOR_LIST_NONE:
					case LOCATOR_LIST_INT:
						locator->results = palloc(sizeof(int));
						break;
					case LOCATOR_LIST_OID:
						locator->results = palloc(sizeof(Oid));
						break;
					case LOCATOR_LIST_POINTER:
						locator->results = palloc(sizeof(void *));
						break;
					case LOCATOR_LIST_LIST:
						/* Should never happen */
						Assert(false);
						break;
				}
				locator->roundRobinNode = -1;
			}
			break;
		case LOCATOR_TYPE_RROBIN:
			if (accessType == RELATION_ACCESS_INSERT)
			{
				locator->locatefunc = locate_roundrobin;
				locator->nodeMap = nodeMap;
				switch (locator->listType)
				{
					case LOCATOR_LIST_NONE:
					case LOCATOR_LIST_INT:
						locator->results = palloc(sizeof(int));
						break;
					case LOCATOR_LIST_OID:
						locator->results = palloc(sizeof(Oid));
						break;
					case LOCATOR_LIST_POINTER:
						locator->results = palloc(sizeof(void *));
						break;
					case LOCATOR_LIST_LIST:
						/* Should never happen */
						Assert(false);
						break;
				}
				locator->roundRobinNode = -1;
			}
			else
			{
				locator->locatefunc = locate_static;
				if (nodeMap == NULL)
				{
					/* no map, prepare array with indexes */
					int *intptr;
					nodeMap = palloc(locator->nodeCount * sizeof(int));
					intptr = (int *) nodeMap;
					for (i = 0; i < locator->nodeCount; i++)
						*intptr++ = i;
				}
				locator->nodeMap = nodeMap;
				locator->results = nodeMap;
			}
			break;
		case LOCATOR_TYPE_HASH:
			if (accessType == RELATION_ACCESS_INSERT)
			{
				locator->locatefunc = locate_hash_insert;
				locator->nodeMap = nodeMap;
				switch (locator->listType)
				{
					case LOCATOR_LIST_NONE:
					case LOCATOR_LIST_INT:
						locator->results = palloc(sizeof(int));
						break;
					case LOCATOR_LIST_OID:
						locator->results = palloc(sizeof(Oid));
						break;
					case LOCATOR_LIST_POINTER:
						locator->results = palloc(sizeof(void *));
						break;
					case LOCATOR_LIST_LIST:
						/* Should never happen */
						Assert(false);
						break;
				}
			}
			else
			{
				locator->locatefunc = locate_hash_select;
				locator->nodeMap = nodeMap;
				switch (locator->listType)
				{
					case LOCATOR_LIST_NONE:
					case LOCATOR_LIST_INT:
						locator->results = palloc(locator->nodeCount * sizeof(int));
						break;
					case LOCATOR_LIST_OID:
						locator->results = palloc(locator->nodeCount * sizeof(Oid));
						break;
					case LOCATOR_LIST_POINTER:
						locator->results = palloc(locator->nodeCount * sizeof(void *));
						break;
					case LOCATOR_LIST_LIST:
						/* Should never happen */
						Assert(false);
						break;
				}
			}

			locator->hashfunc = hash_func_ptr(dataType);
			if (locator->hashfunc == NULL)
				ereport(ERROR, (errmsg("Error: unsupported data type for HASH locator: %d\n",
								   dataType)));
			break;
		case LOCATOR_TYPE_MODULO:
			if (accessType == RELATION_ACCESS_INSERT)
			{
				locator->locatefunc = locate_modulo_insert;
				locator->nodeMap = nodeMap;
				switch (locator->listType)
				{
					case LOCATOR_LIST_NONE:
					case LOCATOR_LIST_INT:
						locator->results = palloc(sizeof(int));
						break;
					case LOCATOR_LIST_OID:
						locator->results = palloc(sizeof(Oid));
						break;
					case LOCATOR_LIST_POINTER:
						locator->results = palloc(sizeof(void *));
						break;
					case LOCATOR_LIST_LIST:
						/* Should never happen */
						Assert(false);
						break;
				}
			}
			else
			{
				locator->locatefunc = locate_modulo_select;
				locator->nodeMap = nodeMap;
				switch (locator->listType)
				{
					case LOCATOR_LIST_NONE:
					case LOCATOR_LIST_INT:
						locator->results = palloc(locator->nodeCount * sizeof(int));
						break;
					case LOCATOR_LIST_OID:
						locator->results = palloc(locator->nodeCount * sizeof(Oid));
						break;
					case LOCATOR_LIST_POINTER:
						locator->results = palloc(locator->nodeCount * sizeof(void *));
						break;
					case LOCATOR_LIST_LIST:
						/* Should never happen */
						Assert(false);
						break;
				}
			}

			locator->valuelen = modulo_value_len(dataType);
			if (locator->valuelen == -1)
				ereport(ERROR, (errmsg("Error: unsupported data type for MODULO locator: %d\n",
								   dataType)));
			break;
		default:
			ereport(ERROR, (errmsg("Error: no such supported locator type: %c\n",
								   locatorType)));
	}

	if (result)
		*result = locator->results;

	return locator;
}


void
freeLocator(Locator *locator)
{
	pfree(locator->nodeMap);
	/*
	 * locator->nodeMap and locator->results may point to the same memory,
	 * do not free it twice
	 */
	if (locator->results != locator->nodeMap)
		pfree(locator->results);
	pfree(locator);
}


/*
 * Each time return the same predefined results
 */
static int
locate_static(Locator *self, Datum value, bool isnull,
			  bool *hasprimary)
{
	/* TODO */
	if (hasprimary)
		*hasprimary = false;
	return self->nodeCount;
}


/*
 * Each time return one next node, in round robin manner
 */
static int
locate_roundrobin(Locator *self, Datum value, bool isnull,
				  bool *hasprimary)
{
	/* TODO */
	if (hasprimary)
		*hasprimary = false;
	if (++self->roundRobinNode >= self->nodeCount)
		self->roundRobinNode = 0;
	switch (self->listType)
	{
		case LOCATOR_LIST_NONE:
			((int *) self->results)[0] = self->roundRobinNode;
			break;
		case LOCATOR_LIST_INT:
			((int *) self->results)[0] =
					((int *) self->nodeMap)[self->roundRobinNode];
			break;
		case LOCATOR_LIST_OID:
			((Oid *) self->results)[0] =
					((Oid *) self->nodeMap)[self->roundRobinNode];
			break;
		case LOCATOR_LIST_POINTER:
			((void **) self->results)[0] =
					((void **) self->nodeMap)[self->roundRobinNode];
			break;
		case LOCATOR_LIST_LIST:
			/* Should never happen */
			Assert(false);
			break;
	}
	return 1;
}


/*
 * Calculate hash from supplied value and use modulo by nodeCount as an index
 */
static int
locate_hash_insert(Locator *self, Datum value, bool isnull,
				   bool *hasprimary)
{
	int index;
	if (hasprimary)
		*hasprimary = false;
	if (isnull)
		index = 0;
	else
	{
		unsigned int hash32;

		hash32 = (unsigned int) DatumGetInt32(DirectFunctionCall1(self->hashfunc, value));

		index = compute_modulo(hash32, self->nodeCount);
	}
	switch (self->listType)
	{
		case LOCATOR_LIST_NONE:
			((int *) self->results)[0] = index;
			break;
		case LOCATOR_LIST_INT:
			((int *) self->results)[0] = ((int *) self->nodeMap)[index];
			break;
		case LOCATOR_LIST_OID:
			((Oid *) self->results)[0] = ((Oid *) self->nodeMap)[index];
			break;
		case LOCATOR_LIST_POINTER:
			((void **) self->results)[0] = ((void **) self->nodeMap)[index];
			break;
		case LOCATOR_LIST_LIST:
			/* Should never happen */
			Assert(false);
			break;
	}
	return 1;
}


/*
 * Calculate hash from supplied value and use modulo by nodeCount as an index
 * if value is NULL assume no hint and return all the nodes.
 */
static int
locate_hash_select(Locator *self, Datum value, bool isnull,
				   bool *hasprimary)
{
	if (hasprimary)
		*hasprimary = false;
	if (isnull)
	{
		int i;
		switch (self->listType)
		{
			case LOCATOR_LIST_NONE:
				for (i = 0; i < self->nodeCount; i++)
					((int *) self->results)[i] = i;
				break;
			case LOCATOR_LIST_INT:
				memcpy(self->results, self->nodeMap,
					   self->nodeCount * sizeof(int));
				break;
			case LOCATOR_LIST_OID:
				memcpy(self->results, self->nodeMap,
					   self->nodeCount * sizeof(Oid));
				break;
			case LOCATOR_LIST_POINTER:
				memcpy(self->results, self->nodeMap,
					   self->nodeCount * sizeof(void *));
				break;
			case LOCATOR_LIST_LIST:
				/* Should never happen */
				Assert(false);
				break;
		}
		return self->nodeCount;
	}
	else
	{
		unsigned int hash32;
		int 		 index;

		hash32 = (unsigned int) DatumGetInt32(DirectFunctionCall1(self->hashfunc, value));

		index = compute_modulo(hash32, self->nodeCount);
		switch (self->listType)
		{
			case LOCATOR_LIST_NONE:
				((int *) self->results)[0] = index;
				break;
			case LOCATOR_LIST_INT:
				((int *) self->results)[0] = ((int *) self->nodeMap)[index];
				break;
			case LOCATOR_LIST_OID:
				((Oid *) self->results)[0] = ((Oid *) self->nodeMap)[index];
				break;
			case LOCATOR_LIST_POINTER:
				((void **) self->results)[0] = ((void **) self->nodeMap)[index];
				break;
			case LOCATOR_LIST_LIST:
				/* Should never happen */
				Assert(false);
				break;
		}
		return 1;
	}
}


/*
 * Use modulo of supplied value by nodeCount as an index
 */
static int
locate_modulo_insert(Locator *self, Datum value, bool isnull,
				   bool *hasprimary)
{
	int index;
	if (hasprimary)
		*hasprimary = false;
	if (isnull)
		index = 0;
	else
	{
		unsigned int mod32;

		if (self->valuelen == 4)
			mod32 = (unsigned int) (GET_4_BYTES(value));
		else if (self->valuelen == 2)
			mod32 = (unsigned int) (GET_2_BYTES(value));
		else if (self->valuelen == 1)
			mod32 = (unsigned int) (GET_1_BYTE(value));
		else
			mod32 = 0;

		index = compute_modulo(mod32, self->nodeCount);
	}
	switch (self->listType)
	{
		case LOCATOR_LIST_NONE:
			((int *) self->results)[0] = index;
			break;
		case LOCATOR_LIST_INT:
			((int *) self->results)[0] = ((int *) self->nodeMap)[index];
			break;
		case LOCATOR_LIST_OID:
			((Oid *) self->results)[0] = ((Oid *) self->nodeMap)[index];
			break;
		case LOCATOR_LIST_POINTER:
			((void **) self->results)[0] = ((void **) self->nodeMap)[index];
			break;
		case LOCATOR_LIST_LIST:
			/* Should never happen */
			Assert(false);
			break;
	}
	return 1;
}


/*
 * Use modulo of supplied value by nodeCount as an index
 * if value is NULL assume no hint and return all the nodes.
 */
static int
locate_modulo_select(Locator *self, Datum value, bool isnull,
				   bool *hasprimary)
{
	if (hasprimary)
		*hasprimary = false;
	if (isnull)
	{
		int i;
		switch (self->listType)
		{
			case LOCATOR_LIST_NONE:
				for (i = 0; i < self->nodeCount; i++)
					((int *) self->results)[i] = i;
				break;
			case LOCATOR_LIST_INT:
				memcpy(self->results, self->nodeMap,
					   self->nodeCount * sizeof(int));
				break;
			case LOCATOR_LIST_OID:
				memcpy(self->results, self->nodeMap,
					   self->nodeCount * sizeof(Oid));
				break;
			case LOCATOR_LIST_POINTER:
				memcpy(self->results, self->nodeMap,
					   self->nodeCount * sizeof(void *));
				break;
			case LOCATOR_LIST_LIST:
				/* Should never happen */
				Assert(false);
				break;
		}
		return self->nodeCount;
	}
	else
	{
		unsigned int mod32;
		int 		 index;

		if (self->valuelen == 4)
			mod32 = (unsigned int) (GET_4_BYTES(value));
		else if (self->valuelen == 2)
			mod32 = (unsigned int) (GET_2_BYTES(value));
		else if (self->valuelen == 1)
			mod32 = (unsigned int) (GET_1_BYTE(value));
		else
			mod32 = 0;

		index = compute_modulo(mod32, self->nodeCount);

		switch (self->listType)
		{
			case LOCATOR_LIST_NONE:
				((int *) self->results)[0] = index;
				break;
			case LOCATOR_LIST_INT:
				((int *) self->results)[0] = ((int *) self->nodeMap)[index];
				break;
			case LOCATOR_LIST_OID:
				((Oid *) self->results)[0] = ((Oid *) self->nodeMap)[index];
				break;
			case LOCATOR_LIST_POINTER:
				((void **) self->results)[0] = ((void **) self->nodeMap)[index];
				break;
			case LOCATOR_LIST_LIST:
				/* Should never happen */
				Assert(false);
				break;
		}
		return 1;
	}
}


int
GET_NODES(Locator *self, Datum value, bool isnull, bool *hasprimary)
{
	return (*self->locatefunc) (self, value, isnull, hasprimary);
}


void *
getLocatorResults(Locator *self)
{
	return self->results;
}


void *
getLocatorNodeMap(Locator *self)
{
	return self->nodeMap;
}


int
getLocatorNodeCount(Locator *self)
{
	return self->nodeCount;
}
#endif
