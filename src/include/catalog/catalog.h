/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  prototypes for functions in backend/catalog/catalog.c
 *
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/catalog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_H
#define CATALOG_H

/*
 *	'pgrminclude ignore' needed here because CppAsString2() does not throw
 *	an error if the symbol is not defined.
 */
#include "catalog/catversion.h" /* pgrminclude ignore */
#include "catalog/pg_class.h"
#include "utils/relcache.h"

#define OIDCHARS		10		/* max chars printed by %u */
#define TABLESPACE_VERSION_DIRECTORY	"PG_" PG_MAJORVERSION "_" \
									CppAsString2(CATALOG_VERSION_NO)


extern bool IsSystemRelation(Relation relation);
extern bool IsToastRelation(Relation relation);
extern bool IsCatalogRelation(Relation relation);

extern bool IsSystemClass(Oid relid, Form_pg_class reltuple);
extern bool IsToastClass(Form_pg_class reltuple);
extern bool IsCatalogClass(Oid relid, Form_pg_class reltuple);

extern bool IsSystemNamespace(Oid namespaceId);
extern bool IsToastNamespace(Oid namespaceId);

extern bool IsReservedName(const char *name);

extern bool IsSharedRelation(Oid relationId);

extern Oid	GetNewOid(Relation relation);
extern Oid GetNewOidWithIndex(Relation relation, Oid indexId,
				   AttrNumber oidcolumn);
extern Oid GetNewRelFileNode(Oid reltablespace, Relation pg_class,
				  char relpersistence);

#endif   /* CATALOG_H */
