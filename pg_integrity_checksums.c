/*-------------------------------------------------------------------------
 *
 * pg_integrity_checksums.c
 *    Implementation of multi-level checksum functionality for PostgreSQL
 *
 * This module provides checksum computation at six distinct levels,
 * each with both physical and logical variants (except cell level):
 *
 * 1. PAGE LEVEL (pg_page_checksum):
 *    - Physical only (wraps PostgreSQL's built-in checksums)
 *    - Uses PostgreSQL's page checksum algorithm
 *    - Detects physical page corruption
 *
 * 2. CELL LEVEL (pg_cell_checksum):
 *    - Logical only (depends only on cell value and attribute number)
 *    - Default 32-bit calls encode NULL as CHECKSUM_NULL (0xFFFFFFFF);
 *      generic cell calls return a bytea digest and use the 64-bit sentinel
 *      for the fnv64 path
 *    - Handles all PostgreSQL data types
 *    - Same value in same column -> same checksum
 *
 * 3. TUPLE LEVEL:
 *    A. Physical (pg_tuple_physical_checksum):
 *       - Depends on physical location (block, offset)
 *       - Includes tuple header (optional)
 *       - Changes after VACUUM, CLUSTER, physical moves
 *       - Detects physical tuple corruption
 *
 *    B. Logical (pg_tuple_logical_checksum):
 *       - Hashes canonical column values, without relation OIDs or TIDs
 *       - No primary key required
 *       - Stable across VACUUM, CLUSTER, REINDEX
 *
 * 4. TABLE LEVEL:
 *    A. Physical (pg_table_physical_checksum):
 *       - Aggregates physical tuple checksums
 *       - Order-independent aggregation
 *       - Includes relation OID for uniqueness
 *       - Changes after physical reorganization
 *
 *    B. Logical (pg_table_logical_checksum):
 *       - Aggregates canonical logical row checksums
 *       - No primary key and no relation OID
 *       - Sum-plus-count preserves duplicate rows
 *       - Stable across VACUUM, CLUSTER, REINDEX
 *
 * 5. INDEX LEVEL:
 *    A. Physical (pg_index_physical_checksum):
 *       - Depends on physical index structure
 *       - Supports all index types (B-tree, Hash, GiST, GIN, SP-GiST, BRIN)
 *       - Changes after REINDEX, physical reorganization
 *       - Detects index corruption
 *
 *    B. Logical (pg_index_logical_checksum):
 *       - Derived from visible table rows and the index definition
 *       - Works for every access method
 *       - No heap TIDs or dead entries
 *       - Stable across VACUUM, CLUSTER, REINDEX
 *
 * 6. DATABASE LEVEL:
 *    A. Physical (pg_database_physical_checksum):
 *       - Aggregates physical relation checksums
 *       - Superuser-only for security
 *       - Optional filtering of system catalogs
 *       - Changes after any physical reorganization
 *
 *    B. Logical (pg_database_logical_checksum):
 *       - Aggregates canonical logical relation checksums
 *       - Superuser-only for security
 *       - No primary key requirement
 *       - Optional logical index checksums
 *       - Stable across physical reorganization of entire database
 *
 * Algorithm Details:
 * - Page checksums wrap PostgreSQL's built-in 16-bit page checksum.
 * - Default logical/physical SQL functions use FNV-1a 32-bit, preserving the
 *   original 2.x values.
 * - Generic overloads accept "fnv32" or "fnv64" and return big-endian raw
 *   digests: 4 bytes for fnv32 and 8 bytes for fnv64.  Table/database physical
 *   checksums always return 8 bytes because relation identity is part of the
 *   value.
 * - FNV-1a formula: hash = (hash ^ byte) * prime, with the 32-bit or 64-bit
 *   basis/prime selected by the algorithm.
 * - Different seeds for different contexts ensure uniqueness.
 * - Order-independent aggregation ensures stability across scans; logical
 *   aggregation mixes 64-bit partial sums and counts for the fnv64 path.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include <math.h>
#include "fmgr.h"
#include "funcapi.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/genam.h"
#include "access/nbtree.h"
#include "access/brin.h"
#include "access/parallel.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_type.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h" 
#include "catalog/pg_inherits.h"
#include "catalog/pg_am.h"          /* *_AM_OID (all supported versions) */
#include "catalog/index.h"          /* IndexGetRelation, BuildIndexInfo, FormIndexDatum */
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"

/*
 * The OID of the pg_index_indrelid_index index is exposed differently
 * depending on the release: PostgreSQL 13 and older declare it in
 * catalog/indexing.h, while PostgreSQL 14 and newer generate it into
 * catalog/pg_index_d.h.
 */
#if PG_VERSION_NUM >= 140000
#include "catalog/pg_index_d.h"     /* IndexIndrelidIndexId */
#else
#include "catalog/indexing.h"       /* IndexIndrelidIndexId */
#endif
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/dsm.h"
#include "storage/shm_toc.h"
#include "storage/smgr.h"
#if PG_VERSION_NUM >= 140000
#include "utils/multirangetypes.h"
#endif
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/errcodes.h"
#include "utils/array.h"
#include "utils/arrayaccess.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rangetypes.h"
#include "utils/rel.h"
#include "utils/typcache.h"
#include "utils/resowner.h"
#include "utils/rls.h"
#include "utils/syscache.h"
#include "utils/snapmgr.h"
#include "utils/guc.h"
#include "port/atomics.h"
#include "miscadmin.h"              
#include "storage/checksum.h" 

#include "pg_integrity_checksums.h"
#include "checksum_internal.h"

PG_MODULE_MAGIC;

/*-------------------------------------------------------------------------
 * Parallelism configuration
 *-------------------------------------------------------------------------
 */

/*
 * pg_integrity_checksums_workers - GUC controlling the number of parallel workers
 *
 * A value of 0 (the default) disables parallelism, preserving the original
 * single-backend behavior. A positive value requests a specific number of
 * workers, clamped by max_parallel_workers and by the number of blocks (or
 * relations) being scanned. Parallelism is also skipped when we are already
 * running inside a parallel worker, since workers cannot be nested.
 */
static int pg_integrity_checksums_workers = 0;

/*
 * pg_integrity_checksums_strict - GUC controlling database-scan skip behavior
 *
 * When true, a relation that cannot be checksummed (invalid index,
 * unsupported relation kind, or a scan error) aborts the database checksum
 * instead of being skipped with a warning or silently omitted.
 */
bool pg_integrity_checksums_strict = false;

/* Upper bound for the GUC; mirrors the sanity limit of parallel workers. */
#define PG_INTEGRITY_CHECKSUMS_MAX_WORKERS 1024


/*
 * _PG_init - extension load-time initialization
 *
 * Registers the GUC variable that controls the degree of parallelism.
 */
void
_PG_init(void)
{
    DefineCustomIntVariable("pg_integrity_checksums.workers",
                            "Number of parallel workers for checksum computation.",
                            "Zero disables parallelism, a positive value "
                            "requests a specific number of workers.",
                            &pg_integrity_checksums_workers,
                            0, 0, PG_INTEGRITY_CHECKSUMS_MAX_WORKERS,
                            PGC_USERSET, 0,
                            NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_integrity_checksums.strict",
                             "Fail instead of skipping relations.",
                             "When enabled, database checksum functions abort "
                             "if a relation cannot be checksummed (invalid "
                             "index, unsupported relation kind, or error).",
                             &pg_integrity_checksums_strict,
                             false,
                             PGC_USERSET, 0,
                             NULL, NULL, NULL);

    CacheRegisterSyscacheCallback(TYPEOID, logical_type_cache_invalidate, 0);
}

/*
 * pg_integrity_checksums_worker_count - determine how many parallel workers to use
 *
 * Returns 0 when the scan should run in a single backend. This happens
 * when the GUC is non-positive and max_parallel_workers is zero, when we
 * are already inside a parallel worker (nesting is not allowed), or when
 * there is nothing to parallelize.
 */
int
pg_integrity_checksums_worker_count(void)
{
    int         workers = pg_integrity_checksums_workers;

    if (workers <= 0)
        return 0;

    if (workers > max_parallel_workers)
        workers = max_parallel_workers;

    /*
     * Custom parallel contexts consume slots from max_worker_processes, not
     * from the per-query max_parallel_workers allowance; clamp to both.
     */
    if (workers > max_worker_processes)
        workers = max_worker_processes;

    /* Parallel workers cannot launch nested parallel workers. */
    if (IsInParallelMode())
        return 0;

    return workers;
}

/*
 * pg_integrity_checksums_get_snapshot - return the snapshot used for heap scans
 *
 * Uses the active snapshot when one is available and falls back to the
 * transaction snapshot otherwise. Both the serial and parallel code
 * paths use this so that they observe identical tuple sets.
 */
Snapshot
pg_integrity_checksums_get_snapshot(void)
{
    Snapshot    snapshot = GetActiveSnapshot();

    if (snapshot == NULL)
        snapshot = GetTransactionSnapshot();

    return snapshot;
}

/*-------------------------------------------------------------------------
 * Internal type definitions
 *-------------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 * Order-independent aggregation state
 *-------------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 * UTILITY FUNCTIONS
 *-------------------------------------------------------------------------
 */

/*
 * pg_data_checksum - Generic data checksum utility function
 *
 * Computes a checksum for arbitrary binary data. Useful for application
 * developers who want to checksum their own data using the same algorithm.
 *
 * Parameters:
 *   data: Binary data to checksum (bytea)
 *   seed: Initial hash value (can be used for different contexts)
 */
PG_FUNCTION_INFO_V1(pg_data_checksum);

Datum
pg_data_checksum(PG_FUNCTION_ARGS)
{
    bytea *data;
    uint32 seed;
    uint32 checksum;
    
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();
    
    data = PG_GETARG_BYTEA_PP(0);
    seed = PG_GETARG_INT32(1);
    
    checksum = pg_checksum_data_custom(VARDATA_ANY(data), 
                                       VARSIZE_ANY_EXHDR(data), 
                                       seed);
    
    PG_RETURN_INT32((int32)checksum);
}

/*
 * pg_text_checksum - Text data checksum utility function
 *
 * Convenience wrapper for checksumming text data. Useful for string
 * comparison and text content verification.
 */
PG_FUNCTION_INFO_V1(pg_text_checksum);

Datum
pg_text_checksum(PG_FUNCTION_ARGS)
{
    text *input_text;
    uint32 seed;
    uint32 checksum;
    
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();
    
    input_text = PG_GETARG_TEXT_PP(0);
    seed = PG_GETARG_INT32(1);
    
    checksum = pg_checksum_data_custom(VARDATA_ANY(input_text), 
                                       VARSIZE_ANY_EXHDR(input_text), 
                                       seed);
    
    PG_RETURN_INT32((int32)checksum);
}
