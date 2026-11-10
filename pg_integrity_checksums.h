/*-------------------------------------------------------------------------
 *
 * pg_integrity_checksums.h
 *    Header file for checksum extension for PostgreSQL
 *
 * This extension provides a comprehensive checksum framework for PostgreSQL
 * at various granularity levels: page, cell, tuple, table, index, and
 * database.  Logical checksums are canonical, primary-key-free and exclude
 * relation OIDs, heap TIDs and type OIDs.
 *
 * The default SQL functions use FNV-1a 32-bit hashes, matching the original
 * 2.x baselines.  Generic algorithm overloads expose FNV-1a 32-bit and
 * 64-bit digests.  Page-level checksums remain PostgreSQL's built-in 32-bit
 * page checksum (16-bit) and are not affected by the generic hash API.
 *
 * CHECKSUM TYPES:
 * 1. Physical Checksums: Depend on physical storage layout
 *    - Change after VACUUM FULL, REINDEX, or physical reorganization
 *    - Useful for detecting physical corruption
 *    - Include: physical location, page headers, etc.
 *
 * 2. Logical Checksums: Depend only on logical data content
 *    - Stable across VACUUM, CLUSTER, REINDEX and physical reorganization
 *    - Useful for logical consistency checks, replication, migration
 *    - Based on canonical column values; no PK/OID/TID
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_INTEGRITY_CHECKSUMS_H
#define PG_INTEGRITY_CHECKSUMS_H

/*
 * The extension relies on the table-AM abstraction (table_open(),
 * table_beginscan(), ...) that became mandatory in PostgreSQL 12, as well as
 * on the generated catalog OID headers introduced around the same time.
 * PostgreSQL 11 and older are therefore not supported.
 */
#if PG_VERSION_NUM < 120000
#error "pg_integrity_checksums requires PostgreSQL 12 or later"
#endif

#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_operator.h"
#include "nodes/pg_list.h"

/*
 * PG_GETARG_ITEMPOINTER was introduced in PostgreSQL 16. Provide a
 * compatibility macro so the extension builds against older releases.
 */
#if PG_VERSION_NUM < 160000
#define PG_GETARG_ITEMPOINTER(n) ((ItemPointer) DatumGetPointer(PG_GETARG_DATUM(n)))
#endif

/* FNV-1a 32-bit hash algorithm constants */
#define FNV_PRIME_32 16777619U
#define FNV_BASIS_32 2166136261U

/* FNV-1a 64-bit hash algorithm constants */
#define FNV_PRIME_64 UINT64CONST(1099511628211)
#define FNV_BASIS_64 UINT64CONST(14695981039346656037)

/* Special checksum value for NULL cells (0xFFFFFFFF = -1 in signed int32) */
#define CHECKSUM_NULL 0xFFFFFFFF

/* 64-bit counterpart used by the generic FNV-1a 64-bit path. */
#define CHECKSUM_NULL_64 UINT64CONST(18446744073709551615)

/* Function declarations - Public SQL-callable functions */
extern Datum pg_page_checksum(PG_FUNCTION_ARGS);
extern Datum pg_page_checksum_status(PG_FUNCTION_ARGS);
extern Datum pg_cell_checksum(PG_FUNCTION_ARGS);
extern Datum pg_tuple_physical_checksum(PG_FUNCTION_ARGS);
extern Datum pg_tuple_logical_checksum(PG_FUNCTION_ARGS);
extern Datum pg_table_physical_checksum(PG_FUNCTION_ARGS);
extern Datum pg_table_logical_checksum(PG_FUNCTION_ARGS);
extern Datum pg_index_physical_checksum(PG_FUNCTION_ARGS);
extern Datum pg_index_logical_checksum(PG_FUNCTION_ARGS);
extern Datum pg_database_physical_checksum(PG_FUNCTION_ARGS);
extern Datum pg_database_logical_checksum(PG_FUNCTION_ARGS);
extern Datum pg_data_checksum(PG_FUNCTION_ARGS);
extern Datum pg_text_checksum(PG_FUNCTION_ARGS);

/* Generic algorithm overloads.  Each accepts an algorithm name as its last
 * text argument: "fnv32" or "fnv64". */
extern Datum pg_data_checksum_algo(PG_FUNCTION_ARGS);
extern Datum pg_text_checksum_algo(PG_FUNCTION_ARGS);
extern Datum pg_cell_checksum_algo(PG_FUNCTION_ARGS);
extern Datum pg_tuple_physical_checksum_algo(PG_FUNCTION_ARGS);
extern Datum pg_tuple_logical_checksum_algo(PG_FUNCTION_ARGS);
extern Datum pg_table_physical_checksum_algo(PG_FUNCTION_ARGS);
extern Datum pg_table_logical_checksum_algo(PG_FUNCTION_ARGS);
extern Datum pg_index_physical_checksum_algo(PG_FUNCTION_ARGS);
extern Datum pg_index_logical_checksum_algo(PG_FUNCTION_ARGS);
extern Datum pg_database_physical_checksum_algo(PG_FUNCTION_ARGS);
extern Datum pg_database_logical_checksum_algo(PG_FUNCTION_ARGS);
extern Datum pg_checksum_bigint(PG_FUNCTION_ARGS);

#endif /* PG_INTEGRITY_CHECKSUMS_H */