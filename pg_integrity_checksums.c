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
static bool pg_integrity_checksums_strict = false;

/* Upper bound for the GUC; mirrors the sanity limit of parallel workers. */
#define PG_INTEGRITY_CHECKSUMS_MAX_WORKERS 1024

/* Number of blocks claimed per atomic fetch during a parallel scan. */
#define PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK 64

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

/*
 * ChecksumWorkerResult - fixed-size result returned by each worker
 *
 * Each worker writes a single entry of this type into shared memory, so
 * no streaming or variable-sized shared memory is required. 32-bit scans
 * zero-extend their aggregate; 64-bit scans store it directly.  had_error is
 * set by database scans when one or more relations had to be skipped.
 */
typedef struct ChecksumWorkerResult
{
    uint64      partial;
    uint64      count;
    bool        had_error;
} ChecksumWorkerResult;

/*
 * ChecksumParallelState - shared state for a parallel checksum scan
 *
 * This structure lives in dynamic shared memory and is used both by the
 * leader and by the parallel workers. nitems holds the number of items
 * (blocks for table/index scans, relations for database scans) and
 * next_item is the atomic counter used to claim work in a dynamic,
 * self-balancing fashion.
 */
typedef struct ChecksumParallelState
{
    ChecksumTaskType task_type;
    Oid         reloid;
    bool        include_header;
    int         algo;
    uint32      nitems;
    uint32      snapshot_size;
    pg_atomic_uint32 next_item;
} ChecksumParallelState;

/* Keys for the shm_toc used by the parallel scan. */
#define KEY_STATE       0
#define KEY_SNAPSHOT    1
#define KEY_RESULTS     2
#define KEY_ITEMS       3

/*-------------------------------------------------------------------------
 * Helper function declarations
 *-------------------------------------------------------------------------
 */
static uint64 compute_database_checksum_internal(bool physical,
                                                 bool include_system,
                                                 bool include_toast,
                                                 bool include_indexes,
                                                 int algo);

/* Privilege, relkind and storage validation helpers */

/* Table checksum implementations (shared by the SQL wrappers and partitions) */

static void compute_database_item(Oid relid, char relkind, bool physical,
                                  int algo, uint64 *rel_checksum, bool *skip);
static void pg_integrity_checksums_report_skipped_relation(Oid relid);
static void compute_database_items(ChecksumRelationItem *items, uint32 start,
                                   uint32 end, bool physical, int algo,
                                   ChecksumAccum64 *acc,
                                   ChecksumLogicalAccum *logical_acc,
                                   ChecksumLogicalAccum64 *logical_acc64,
                                   bool *had_error);


/* Parallel worker entry point (must be a globally visible symbol). */
PGDLLEXPORT void pg_integrity_checksums_parallel_worker_main(dsm_segment *seg, shm_toc *toc);


/*-------------------------------------------------------------------------
 * PARALLEL SCAN IMPLEMENTATION
 *-------------------------------------------------------------------------
 *
 * The serial and parallel code paths share the block-range (or item-range)
 * scan functions below. In the serial case the whole range is processed by
 * a single backend; in the parallel case the range is split dynamically
 * among the leader and its workers using an atomic counter. Because the
 * aggregation is order- and partition-independent, both paths produce
 * identical results.
 */

/*
 * pg_integrity_checksums_consume - claim the next range of items
 *
 * Atomically advances the shared counter and returns the resulting range
 * [start, end). Returns false when all items have been claimed. This
 * self-balancing scheme keeps all participants busy regardless of how many
 * workers actually started.
 */
static bool
pg_integrity_checksums_consume(ChecksumParallelState *state, uint32 chunk,
                     uint32 *start, uint32 *end)
{
    uint32      first = pg_atomic_fetch_add_u32(&state->next_item, chunk);

    if (first >= state->nitems)
        return false;

    *start = first;

    /*
     * Compute the end without first + chunk, which could wrap around for a
     * relation with close to 2^32 blocks.
     */
    if (chunk >= state->nitems - first)
        *end = state->nitems;
    else
        *end = first + chunk;

    return true;
}

/*
 * compute_database_item - compute the checksum of one relation
 *
 * Sets *skip when the relation has no usable checksum (for example an
 * invalid index, an unsupported relation kind, or a TOAST relation in logical
 * mode).
 */
static void
compute_database_item(Oid relid, char relkind, bool physical, int algo,
                      uint64 *rel_checksum, bool *skip)
{
    *rel_checksum = 0;
    *skip = false;

    if (relkind == RELKIND_INDEX)
    {
        bool        is_null = false;
        uint32      value = 0;

        if (physical && algo == 1)
        {
            uint64      value64 = pg_index_physical_checksum_calc64(relid,
                                                                    false,
                                                                    &is_null);

            if (!is_null)
                *rel_checksum = value64;
        }
        else if (physical)
            value = pg_index_physical_checksum_calc(relid, false, &is_null);
        else if (algo == 1)
        {
            uint64      value64 = pg_index_logical_checksum_calc64(relid,
                                                                   false,
                                                                   &is_null);

            if (!is_null)
                *rel_checksum = value64;
        }
        else
            value = pg_index_logical_checksum_calc(relid, false, &is_null);

        if (is_null)
            *skip = true;
        else if (algo == 0)
            *rel_checksum = (uint64) value;
    }
    else
    {
        if (physical && algo == 1)
            *rel_checksum = pg_table_physical_checksum_calc64(relid, false,
                                                              false);
        else if (physical)
            *rel_checksum = pg_table_physical_checksum_calc(relid, false,
                                                            false);
        else
        {
            bool        is_null = false;

            if (algo == 1)
                *rel_checksum = pg_table_logical_checksum_calc64(relid, false,
                                                                 &is_null);
            else
            {
                int64       value = pg_table_logical_checksum_calc(relid,
                                                                   false,
                                                                   &is_null);

                *rel_checksum = (uint64) value;
            }
            if (is_null)
                *skip = true;
        }
    }
}

/*
 * pg_integrity_checksums_report_skipped_relation - strict-mode error for skipped relation
 */
static void
pg_integrity_checksums_report_skipped_relation(Oid relid)
{
    char       *relname;

    if (!pg_integrity_checksums_strict)
        return;

    relname = get_rel_name(relid);

    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("relation \"%s\" cannot be checksummed",
                    relname ? relname : "?"),
             errdetail("The relation has no usable checksum (invalid index, unsupported relation kind, or a scan error)."),
             errhint("Set pg_integrity_checksums.strict = false to skip relations without a usable checksum.")));
}

/*
 * compute_database_items - database checksum over a range of relations
 */
static void
compute_database_items(ChecksumRelationItem *items, uint32 start, uint32 end,
                       bool physical, int algo, ChecksumAccum64 *acc,
                       ChecksumLogicalAccum *logical_acc,
                       ChecksumLogicalAccum64 *logical_acc64, bool *had_error)
{
    uint32      i;

    for (i = start; i < end; i++)
    {
        Oid         relid = items[i].reloid;
        char        relkind = items[i].relkind;
        uint64      rel_checksum = 0;
        bool        skip = false;
        MemoryContext oldcontext;
        ResourceOwner oldowner;

#if PG_VERSION_NUM < 170000
        /*
         * PostgreSQL 12-16 prohibit internal subtransactions while in
         * parallel mode.  The leader of a parallel database scan is in
         * parallel mode too, so neither the leader nor a worker can use the
         * subtransaction-based error recovery below.  Run the relation
         * checksum directly; an error then aborts the parallel scan.  There
         * is no safe way to catch and continue without a subtransaction.
         */
        if (IsInParallelMode())
        {
            compute_database_item(relid, relkind, physical, algo, &rel_checksum,
                                  &skip);
            if (skip)
                pg_integrity_checksums_report_skipped_relation(relid);
            else if (logical_acc64 != NULL)
                logical_accum_add64(logical_acc64, rel_checksum);
            else if (logical_acc != NULL)
                logical_accum_add(logical_acc, (uint32) rel_checksum);
            else
                checksum_accum64_add(acc,
                                     combine_checksums_64(rel_checksum,
                                                          (uint64) relid));
            continue;
        }
#endif

        /*
         * Each relation is checksummed in an internal subtransaction.  If a
         * relation cannot be processed (dropped concurrently, corrupt,
         * unsupported, ...), the subtransaction rollback releases its locks
         * and buffer pins, and the scan can safely continue.  Catching an
         * error without a subtransaction would leave resources behind.
         */
        oldcontext = CurrentMemoryContext;
        oldowner = CurrentResourceOwner;

        BeginInternalSubTransaction(NULL);
        /* Run statements inside the caller's memory context */
        MemoryContextSwitchTo(oldcontext);

        PG_TRY();
        {
            compute_database_item(relid, relkind, physical, algo, &rel_checksum,
                                  &skip);

            if (skip)
                pg_integrity_checksums_report_skipped_relation(relid);
            else if (logical_acc64 != NULL)
                logical_accum_add64(logical_acc64, rel_checksum);
            else if (logical_acc != NULL)
                logical_accum_add(logical_acc, (uint32) rel_checksum);
            else
                checksum_accum64_add(acc,
                                     combine_checksums_64(rel_checksum,
                                                          (uint64) relid));

            /* Commit the inner transaction, return to outer xact context */
            ReleaseCurrentSubTransaction();
            MemoryContextSwitchTo(oldcontext);
            CurrentResourceOwner = oldowner;
        }
        PG_CATCH();
        {
            int         sqlerrcode = geterrcode();
            ErrorData  *edata;

            /*
             * Cancellation, shutdown and out-of-memory errors mean that the
             * result cannot be trusted; propagate them instead of silently
             * returning a partial database checksum.
             */
            /*
             * OOM and shutdown errors are propagated immediately.  Other
             * errors, including cancellation and strict-mode failures, first
             * release the internal subtransaction: a caller (for example a
             * PL/pgSQL exception block) may catch the error and continue, and
             * an open parallel child transaction would keep
             * IsInParallelMode() true.
             */
            if (sqlerrcode == ERRCODE_OUT_OF_MEMORY ||
                sqlerrcode == ERRCODE_ADMIN_SHUTDOWN ||
                sqlerrcode == ERRCODE_CRASH_SHUTDOWN ||
                sqlerrcode == ERRCODE_CANNOT_CONNECT_NOW)
                PG_RE_THROW();

            MemoryContextSwitchTo(oldcontext);
            edata = CopyErrorData();
            FlushErrorState();

            /* Abort the inner transaction */
            RollbackAndReleaseCurrentSubTransaction();
            MemoryContextSwitchTo(oldcontext);
            CurrentResourceOwner = oldowner;

            if (had_error != NULL)
                *had_error = true;

            if (pg_integrity_checksums_strict || sqlerrcode == ERRCODE_QUERY_CANCELED)
                ReThrowError(edata);

            /*
             * Parallel workers log to the server log; the leader emits a
             * summary warning when it folds the workers' results.
             */
            if (IsParallelWorker())
                elog(LOG,
                     "skipping relation %u while computing database checksum: %s",
                     relid, edata->message);
            else
                ereport(WARNING,
                        (errmsg("skipping relation %u while computing database checksum",
                                relid),
                         errdetail("%s", edata->message)));

            FreeErrorData(edata);
        }
        PG_END_TRY();
    }
}

/*
 * pg_integrity_checksums_parallel_worker_main - entry point for parallel workers
 *
 * Restores the shared scan state and snapshot, then consumes items from the
 * shared atomic counter until none remain, writing a fixed-size result back
 * to shared memory before exiting.
 */
void
pg_integrity_checksums_parallel_worker_main(dsm_segment *seg, shm_toc *toc)
{
    ChecksumParallelState *state = shm_toc_lookup(toc, KEY_STATE, false);
    ChecksumWorkerResult *results = shm_toc_lookup(toc, KEY_RESULTS, false);
    int         worker_number = ParallelWorkerNumber;
    ChecksumWorkerResult *myresult = &results[worker_number];
    Snapshot    snapshot = NULL;
    uint32      start;
    uint32      end;

    (void) seg;

    myresult->partial = 0;
    myresult->count = 0;
    myresult->had_error = false;

    if (state->snapshot_size > 0)
    {
        char       *snap = shm_toc_lookup(toc, KEY_SNAPSHOT, false);

        snapshot = RestoreSnapshot(snap);
    }

    switch (state->task_type)
    {
        case CHECKSUM_TASK_TABLE_PHYSICAL:
            {
                Relation    rel = relation_open(state->reloid, AccessShareLock);
                ChecksumAccum acc = {0, 0};
                BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

                if (state->algo == 1)
                {
                    ChecksumAccum64 acc64 = {0, 0};

                    while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                &start, &end))
                        scan_table_blocks_physical64(rel, start, end,
                                                     state->include_header,
                                                     snapshot, &acc64, bstrategy);
                    relation_close(rel, AccessShareLock);
                    FreeAccessStrategy(bstrategy);
                    myresult->partial = acc64.partial;
                    myresult->count = acc64.count;
                    break;
                }

                while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                            &start, &end))
                    scan_table_blocks_physical(rel, start, end,
                                               state->include_header, snapshot,
                                               &acc, bstrategy);

                relation_close(rel, AccessShareLock);
                FreeAccessStrategy(bstrategy);
                myresult->partial = acc.partial;
                myresult->count = acc.count;
                break;
            }
        case CHECKSUM_TASK_TABLE_LOGICAL:
            {
                Relation    rel = relation_open(state->reloid, AccessShareLock);
                ChecksumLogicalAccum acc = {0, 0};
                BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

                if (state->algo == 1)
                {
                    ChecksumLogicalAccum64 acc64 = {0, 0};

                    while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                &start, &end))
                        scan_table_blocks_logical64(rel, start, end, snapshot,
                                                    &acc64, bstrategy);
                    relation_close(rel, AccessShareLock);
                    FreeAccessStrategy(bstrategy);
                    myresult->partial = acc64.sum;
                    myresult->count = acc64.count;
                    break;
                }

                while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                            &start, &end))
                    scan_table_blocks_logical(rel, start, end, snapshot, &acc,
                                              bstrategy);

                relation_close(rel, AccessShareLock);
                FreeAccessStrategy(bstrategy);
                myresult->partial = acc.sum;
                myresult->count = acc.count;
                break;
            }
        case CHECKSUM_TASK_INDEX_PHYSICAL:
            {
                Relation    idxRel = index_open(state->reloid, AccessShareLock);
                bool        is_brin = (idxRel->rd_rel->relam == BRIN_AM_OID);
                ChecksumAccum acc = {0, 0};
                BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

                if (state->algo == 1)
                {
                    ChecksumAccum64 acc64 = {0, 0};

                    while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                &start, &end))
                        scan_index_blocks_physical64(idxRel, start, end, is_brin,
                                                     &acc64, bstrategy);
                    index_close(idxRel, AccessShareLock);
                    FreeAccessStrategy(bstrategy);
                    myresult->partial = acc64.partial;
                    myresult->count = acc64.count;
                    break;
                }

                while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                            &start, &end))
                    scan_index_blocks_physical(idxRel, start, end, is_brin, &acc,
                                               bstrategy);

                index_close(idxRel, AccessShareLock);
                FreeAccessStrategy(bstrategy);
                myresult->partial = acc.partial;
                myresult->count = acc.count;
                break;
            }
        case CHECKSUM_TASK_INDEX_LOGICAL:
            {
                IndexLogicalScanState scanstate;
                ChecksumLogicalAccum acc = {0, 0};

                /* scanstate owns its buffer strategy. */
                if (!pg_index_logical_scan_init(state->reloid, snapshot,
                                                &scanstate))
                    elog(ERROR, "could not initialize logical index scan for index %u",
                         state->reloid);

                if (state->algo == 1)
                {
                    ChecksumLogicalAccum64 acc64 = {0, 0};

                    while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                &start, &end))
                        pg_index_logical_scan_blocks64(&scanstate, start, end,
                                                       &acc64);
                    pg_index_logical_scan_end(&scanstate);
                    myresult->partial = acc64.sum;
                    myresult->count = acc64.count;
                    break;
                }

                while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                            &start, &end))
                    pg_index_logical_scan_blocks(&scanstate, start, end, &acc);

                pg_index_logical_scan_end(&scanstate);
                myresult->partial = acc.sum;
                myresult->count = acc.count;
                break;
            }
        case CHECKSUM_TASK_DATABASE_PHYSICAL:
        case CHECKSUM_TASK_DATABASE_LOGICAL:
            {
                ChecksumRelationItem *items = shm_toc_lookup(toc, KEY_ITEMS, false);
                bool        physical = (state->task_type == CHECKSUM_TASK_DATABASE_PHYSICAL);
                ChecksumAccum64 acc64 = {0, 0};
                ChecksumLogicalAccum lacc = {0, 0};
                ChecksumLogicalAccum64 lacc64 = {0, 0};
                bool        pushed = false;

                /*
                 * The relation checksum functions rely on GetActiveSnapshot(),
                 * so push the leader's snapshot to keep the whole database
                 * consistent.
                 */
                if (snapshot != NULL)
                {
                    PushActiveSnapshot(snapshot);
                    pushed = true;
                }

                while (pg_integrity_checksums_consume(state, 1, &start, &end))
                    compute_database_items(items, start, end, physical,
                                           state->algo, &acc64,
                                           physical ? NULL : &lacc,
                                           (!physical && state->algo == 1) ? &lacc64 : NULL,
                                           &myresult->had_error);

                if (pushed)
                    PopActiveSnapshot();

                if (physical)
                {
                    myresult->partial = acc64.partial;
                    myresult->count = acc64.count;
                }
                else if (state->algo == 1)
                {
                    myresult->partial = lacc64.sum;
                    myresult->count = lacc64.count;
                }
                else
                {
                    myresult->partial = lacc.sum;
                    myresult->count = lacc.count;
                }
                break;
            }
        default:
            elog(ERROR, "unknown checksum parallel task type %d",
                 (int) state->task_type);
    }
}

/*
 * pg_integrity_checksums_parallel_scan_blocks - run a block-based scan in parallel
 *
 * Sets up the shared memory segment, launches the requested workers, lets the
 * leader participate in the scan, and folds the workers' results into the
 * caller's accumulator.  Physical scans use acc; table and index logical
 * scans use logical_acc.
 */
void
pg_integrity_checksums_parallel_scan_blocks(ChecksumTaskType task_type, Oid reloid,
                                  bool include_header, uint32 nblocks,
                                  int workers, int algo, ChecksumAccum *acc,
                                  ChecksumLogicalAccum *logical_acc,
                                  ChecksumAccum64 *acc64,
                                  ChecksumLogicalAccum64 *logical_acc64)
{
    ParallelContext *pcxt;
    ParallelContext *volatile pcxt_cleanup = NULL;
    shm_toc_estimator e;
    ChecksumParallelState *state;
    ChecksumWorkerResult *results;
    Snapshot    snapshot = NULL;
    uint32      snapshot_size = 0;
    bool        needs_snapshot = (task_type == CHECKSUM_TASK_TABLE_PHYSICAL ||
                                  task_type == CHECKSUM_TASK_TABLE_LOGICAL ||
                                  task_type == CHECKSUM_TASK_INDEX_LOGICAL);
    uint32      start;
    uint32      end;
    int         i;

    /*
     * Do not launch more workers than there are block chunks.  This avoids
     * paying worker startup cost for small relations.
     */
    if (nblocks == 0)
        workers = 0;
    else
    {
        uint32      chunks = nblocks / PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK +
            (nblocks % PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK != 0);

        if ((uint32) workers > chunks)
            workers = (int) chunks;
    }

    if (workers <= 0)
        return;

    if (needs_snapshot)
    {
        snapshot = pg_integrity_checksums_get_snapshot();
        if (snapshot == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("no active snapshot while starting a parallel checksum scan")));
        snapshot_size = (uint32) EstimateSnapshotSpace(snapshot);
    }

    /*
     * CreateParallelContext() requires parallel mode to be active.  It also
     * makes IsInParallelMode() true in the leader, which prevents the leader
     * from starting a nested parallel scan when it processes its own share
     * of a database scan.
     */
    EnterParallelMode();

    PG_TRY();
    {
        pcxt = CreateParallelContext("pg_integrity_checksums",
                                     "pg_integrity_checksums_parallel_worker_main",
                                     workers);
        pcxt_cleanup = pcxt;

        shm_toc_initialize_estimator(&e);
        shm_toc_estimate_chunk(&e, sizeof(ChecksumParallelState));
        shm_toc_estimate_chunk(&e, sizeof(ChecksumWorkerResult) * workers);
        if (snapshot_size > 0)
            shm_toc_estimate_chunk(&e, snapshot_size);
        shm_toc_estimate_keys(&e, 3);
        pcxt->estimator = e;

        InitializeParallelDSM(pcxt);

        state = shm_toc_allocate(pcxt->toc, sizeof(ChecksumParallelState));
        shm_toc_insert(pcxt->toc, KEY_STATE, state);
        state->task_type = task_type;
        state->reloid = reloid;
        state->include_header = include_header;
        state->algo = algo;
        state->nitems = nblocks;
        state->snapshot_size = snapshot_size;
        pg_atomic_init_u32(&state->next_item, 0);

        results = shm_toc_allocate(pcxt->toc, sizeof(ChecksumWorkerResult) * workers);
        shm_toc_insert(pcxt->toc, KEY_RESULTS, results);
        memset(results, 0, sizeof(ChecksumWorkerResult) * workers);

        if (snapshot_size > 0)
        {
            char       *snap = shm_toc_allocate(pcxt->toc, snapshot_size);

            SerializeSnapshot(snapshot, snap);
            shm_toc_insert(pcxt->toc, KEY_SNAPSHOT, snap);
        }

        LaunchParallelWorkers(pcxt);

        if (pcxt->nworkers_launched > 0)
            WaitForParallelWorkersToAttach(pcxt);

        /* The leader processes its own share of the work. */
        switch (task_type)
        {
            case CHECKSUM_TASK_TABLE_PHYSICAL:
                {
                    Relation    rel = relation_open(reloid, AccessShareLock);
                    BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

                    if (algo == 1)
                    {
                        while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                    &start, &end))
                            scan_table_blocks_physical64(rel, start, end,
                                                         include_header, snapshot,
                                                         acc64, bstrategy);
                    }
                    else
                    {
                        while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                    &start, &end))
                            scan_table_blocks_physical(rel, start, end, include_header,
                                                       snapshot, acc, bstrategy);
                    }

                    relation_close(rel, AccessShareLock);
                    FreeAccessStrategy(bstrategy);
                    break;
                }
            case CHECKSUM_TASK_TABLE_LOGICAL:
                {
                    Relation    rel = relation_open(reloid, AccessShareLock);
                    BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

                    if (algo == 1)
                    {
                        while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                    &start, &end))
                            scan_table_blocks_logical64(rel, start, end, snapshot,
                                                        logical_acc64, bstrategy);
                    }
                    else
                    {
                        while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                    &start, &end))
                            scan_table_blocks_logical(rel, start, end, snapshot,
                                                      logical_acc, bstrategy);
                    }

                    relation_close(rel, AccessShareLock);
                    FreeAccessStrategy(bstrategy);
                    break;
                }
            case CHECKSUM_TASK_INDEX_PHYSICAL:
                {
                    Relation    idxRel = index_open(reloid, AccessShareLock);
                    bool        is_brin = (idxRel->rd_rel->relam == BRIN_AM_OID);
                    BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

                    if (algo == 1)
                    {
                        while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                    &start, &end))
                            scan_index_blocks_physical64(idxRel, start, end,
                                                         is_brin, acc64,
                                                         bstrategy);
                    }
                    else
                    {
                        while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                    &start, &end))
                            scan_index_blocks_physical(idxRel, start, end, is_brin,
                                                       acc, bstrategy);
                    }

                    index_close(idxRel, AccessShareLock);
                    FreeAccessStrategy(bstrategy);
                    break;
                }
            case CHECKSUM_TASK_INDEX_LOGICAL:
                {
                    IndexLogicalScanState scanstate;

                    if (!pg_index_logical_scan_init(reloid, snapshot,
                                                    &scanstate))
                        elog(ERROR, "could not initialize logical index scan for index %u",
                             reloid);

                    if (algo == 1)
                    {
                        while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                    &start, &end))
                            pg_index_logical_scan_blocks64(&scanstate, start, end,
                                                           logical_acc64);
                    }
                    else
                    {
                        while (pg_integrity_checksums_consume(state, PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                    &start, &end))
                            pg_index_logical_scan_blocks(&scanstate, start, end,
                                                         logical_acc);
                    }

                    pg_index_logical_scan_end(&scanstate);
                    break;
                }
            default:
                break;
        }

        if (pcxt->nworkers_launched > 0)
            WaitForParallelWorkersToFinish(pcxt);

        /* Fold in the workers' results. */
        for (i = 0; i < workers; i++)
        {
            if (pcxt->worker[i].bgwhandle != NULL)
            {
                if (algo == 1)
                {
                    if (task_type == CHECKSUM_TASK_TABLE_LOGICAL ||
                        task_type == CHECKSUM_TASK_INDEX_LOGICAL)
                    {
                        logical_acc64->sum += results[i].partial;
                        logical_acc64->count += results[i].count;
                    }
                    else
                        checksum_accum64_merge(acc64, results[i].partial,
                                               results[i].count);
                }
                else if (task_type == CHECKSUM_TASK_TABLE_LOGICAL ||
                         task_type == CHECKSUM_TASK_INDEX_LOGICAL)
                {
                    logical_acc->sum += results[i].partial;
                    logical_acc->count += results[i].count;
                }
                else
                    checksum_accum_merge(acc, results[i].partial,
                                         results[i].count);
            }
        }
    }
    PG_CATCH();
    {
        if (pcxt_cleanup != NULL)
            DestroyParallelContext(pcxt_cleanup);
        ExitParallelMode();
        PG_RE_THROW();
    }
    PG_END_TRY();

    DestroyParallelContext(pcxt);
    ExitParallelMode();
}

/*
 * pg_integrity_checksums_parallel_scan_database - run a database scan in parallel
 */
void
pg_integrity_checksums_parallel_scan_database(bool physical,
                                    ChecksumRelationItem *items, uint32 nitems,
                                    int workers, int algo,
                                    ChecksumAccum64 *acc,
                                    ChecksumLogicalAccum *logical_acc,
                                    ChecksumLogicalAccum64 *logical_acc64,
                                    bool *had_error)
{
    ParallelContext *pcxt;
    ParallelContext *volatile pcxt_cleanup = NULL;
    shm_toc_estimator e;
    ChecksumParallelState *state;
    ChecksumWorkerResult *results;
    Snapshot    snapshot;
    uint32      snapshot_size;
    uint32      start;
    uint32      end;
    bool        local_had_error = false;
    int         i;

    /*
     * Propagate a consistent snapshot to the workers so that every relation
     * is checksummed as of the same point in time, exactly like the serial
     * path.
     */
    snapshot = pg_integrity_checksums_get_snapshot();
    if (snapshot == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("no active snapshot while starting a parallel database checksum")));
    snapshot_size = (uint32) EstimateSnapshotSpace(snapshot);

    EnterParallelMode();

    PG_TRY();
    {
        pcxt = CreateParallelContext("pg_integrity_checksums",
                                     "pg_integrity_checksums_parallel_worker_main",
                                     workers);
        pcxt_cleanup = pcxt;

        shm_toc_initialize_estimator(&e);
        shm_toc_estimate_chunk(&e, sizeof(ChecksumParallelState));
        shm_toc_estimate_chunk(&e, sizeof(ChecksumWorkerResult) * workers);
        shm_toc_estimate_chunk(&e, sizeof(ChecksumRelationItem) * nitems);
        shm_toc_estimate_chunk(&e, snapshot_size);
        shm_toc_estimate_keys(&e, 4);
        pcxt->estimator = e;

        InitializeParallelDSM(pcxt);

        state = shm_toc_allocate(pcxt->toc, sizeof(ChecksumParallelState));
        shm_toc_insert(pcxt->toc, KEY_STATE, state);
        state->task_type = physical ? CHECKSUM_TASK_DATABASE_PHYSICAL
                                    : CHECKSUM_TASK_DATABASE_LOGICAL;
        state->reloid = InvalidOid;
        state->include_header = false;
        state->algo = algo;
        state->nitems = nitems;
        state->snapshot_size = snapshot_size;
        pg_atomic_init_u32(&state->next_item, 0);

        results = shm_toc_allocate(pcxt->toc, sizeof(ChecksumWorkerResult) * workers);
        shm_toc_insert(pcxt->toc, KEY_RESULTS, results);
        memset(results, 0, sizeof(ChecksumWorkerResult) * workers);

        {
            char       *snap = shm_toc_allocate(pcxt->toc, snapshot_size);

            SerializeSnapshot(snapshot, snap);
            shm_toc_insert(pcxt->toc, KEY_SNAPSHOT, snap);
        }

        {
            ChecksumRelationItem *ditems = shm_toc_allocate(pcxt->toc,
                                                            sizeof(ChecksumRelationItem) * nitems);

            memcpy(ditems, items, sizeof(ChecksumRelationItem) * nitems);
            shm_toc_insert(pcxt->toc, KEY_ITEMS, ditems);
        }

        LaunchParallelWorkers(pcxt);

        if (pcxt->nworkers_launched > 0)
            WaitForParallelWorkersToAttach(pcxt);

        /* The leader processes its own share of the relations. */
        while (pg_integrity_checksums_consume(state, 1, &start, &end))
            compute_database_items(items, start, end, physical, algo, acc,
                                   logical_acc, logical_acc64, &local_had_error);

        if (pcxt->nworkers_launched > 0)
            WaitForParallelWorkersToFinish(pcxt);

        for (i = 0; i < workers; i++)
        {
            if (pcxt->worker[i].bgwhandle != NULL)
            {
                if (physical)
                    checksum_accum64_merge(acc, results[i].partial,
                                           results[i].count);
                else if (algo == 1)
                {
                    logical_acc64->sum += results[i].partial;
                    logical_acc64->count += results[i].count;
                }
                else
                {
                    logical_acc->sum += results[i].partial;
                    logical_acc->count += results[i].count;
                }

                if (results[i].had_error)
                    local_had_error = true;
            }
        }

    }
    PG_CATCH();
    {
        if (pcxt_cleanup != NULL)
            DestroyParallelContext(pcxt_cleanup);
        ExitParallelMode();
        PG_RE_THROW();
    }
    PG_END_TRY();

    DestroyParallelContext(pcxt);
    ExitParallelMode();

    if (had_error != NULL)
        *had_error = local_had_error;
}

/*-------------------------------------------------------------------------
 * DATABASE LEVEL FUNCTIONS (Physical and Logical)
 *-------------------------------------------------------------------------
 */

/*
 * compute_database_checksum_internal - Core database checksum calculation
 *
 * Computes a checksum for the entire database by aggregating checksums
 * of all relations. This is a resource-intensive operation that should
 * be run by superusers only.
 *
 * Features:
 * 1. Uses a consistent snapshot for data consistency
 * 2. Can filter out system catalogs and TOAST tables
 * 3. Skips relations that cannot be processed (with error handling)
 * 4. Handles both physical and logical checksum modes
 *
 * Returns a 64-bit FNV-1a hash.
 */
static uint64
compute_database_checksum_internal(bool physical, bool include_system,
                                  bool include_toast, bool include_indexes,
                                  int algo)
{
    Relation    pg_class_rel;
    TableScanDesc scan;
    HeapTuple   classTuple;
    Snapshot    snapshot;
    ChecksumRelationItem *items;
    uint32      nitems = 0;
    uint32      capacity = 64;
    ChecksumAccum64 acc = {0, 0};
    ChecksumLogicalAccum lacc = {0, 0};
    ChecksumLogicalAccum64 lacc64 = {0, 0};
    bool        had_error = false;
    int         workers;

    /* Use a consistent snapshot */
    snapshot = pg_integrity_checksums_get_snapshot();
    if (snapshot == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("no active snapshot while computing database checksum")));

    /* Collect the matching relations first. */
    items = (ChecksumRelationItem *) palloc(capacity * sizeof(ChecksumRelationItem));

    pg_class_rel = table_open(RelationRelationId, AccessShareLock);
#if PG_VERSION_NUM >= 190000
    scan = table_beginscan(pg_class_rel, snapshot, 0, NULL, 0);
#else
    scan = table_beginscan(pg_class_rel, snapshot, 0, NULL);
#endif

    while ((classTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        Form_pg_class classForm = (Form_pg_class) GETSTRUCT(classTuple);
        Oid         relid = classForm->oid;
        Oid         relnamespace = classForm->relnamespace;
        char        relkind = classForm->relkind;

        /*
         * Temporary relations are session-local: they are not part of the
         * persistent database, other sessions must not read them, and a
         * parallel worker is not allowed to touch them at all. Skipping them
         * keeps the database checksum stable and independent of the calling
         * session's temporary objects.
         */
        if (isAnyTempNamespace(relnamespace))
            continue;

        /*
         * Shared catalogs belong to the cluster rather than to this
         * database.  They are deliberately not part of a per-database
         * checksum, even when include_system is true.
         */
        if (classForm->relisshared)
            continue;

        /*
         * TOAST relations use a tuple format of their own.  A physical
         * checksum can hash their raw pages; a logical checksum already
         * covers the toast content when the parent table detoasts it.
         */
        if (relnamespace == PG_TOAST_NAMESPACE)
        {
            if (!physical || !include_toast)
                continue;
        }
        else if (!include_system && relnamespace == PG_CATALOG_NAMESPACE)
            continue;

        /* Skip non-table/index relations */
        if (relkind != RELKIND_RELATION &&
            relkind != RELKIND_INDEX &&
            relkind != RELKIND_MATVIEW &&
            relkind != RELKIND_TOASTVALUE)
            continue;

        if (!include_indexes && relkind == RELKIND_INDEX)
            continue;

        if (nitems >= capacity)
        {
            capacity *= 2;
            items = (ChecksumRelationItem *)
                repalloc(items, capacity * sizeof(ChecksumRelationItem));
        }

        items[nitems].reloid = relid;
        items[nitems].relkind = relkind;
        nitems++;

        CHECK_FOR_INTERRUPTS();
    }

    table_endscan(scan);
    table_close(pg_class_rel, AccessShareLock);

    workers = pg_integrity_checksums_worker_count();
    if (nitems < (uint32) workers)
        workers = (int) nitems;

    if (workers < 1)
        compute_database_items(items, 0, nitems, physical, algo, &acc,
                               physical ? NULL : &lacc,
                               (!physical && algo == 1) ? &lacc64 : NULL,
                               &had_error);
    else
        pg_integrity_checksums_parallel_scan_database(physical, items, nitems,
                                            workers, algo, &acc,
                                            physical ? NULL : &lacc,
                                            (!physical && algo == 1) ? &lacc64 : NULL,
                                            &had_error);

    pfree(items);

    if (had_error)
        ereport(WARNING,
                (errmsg("database checksum skipped one or more relations"),
                 errhint("Inspect the server log for the affected relations.")));

    if (physical)
        return checksum_accum64_finalize(&acc);

    if (algo == 1)
        return logical_accum_finalize64(&lacc64);

    return (uint64) logical_accum_finalize(&lacc);
}

/*
 * pg_database_physical_checksum - SQL function for physical database checksums
 *
 * Public interface for computing physical database checksums.
 * Requires superuser privileges for security.
 *
 * Parameters:
 *   include_system: Include system catalogs in checksum
 *   include_toast: Include TOAST tables in checksum
 */
PG_FUNCTION_INFO_V1(pg_database_physical_checksum);

Datum
pg_database_physical_checksum(PG_FUNCTION_ARGS)
{
    bool        include_system = false;
    bool        include_toast = false;
    uint64      db_checksum;
    
    /* Parse optional parameters */
    if (PG_NARGS() >= 1)
        include_system = PG_GETARG_BOOL(0);
    if (PG_NARGS() >= 2)
        include_toast = PG_GETARG_BOOL(1);

    /* Security check: only superusers can checksum the entire database */
    if (!superuser())
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("must be superuser to compute database checksum")));

    /* Compute physical database checksum */
    db_checksum = compute_database_checksum_internal(true, include_system,
                                                     include_toast, true, 0);

    PG_RETURN_INT64((int64)db_checksum);
}

/*
 * pg_database_logical_checksum - SQL function for logical database checksums
 *
 * Public interface for computing logical database checksums.
 * Requires superuser privileges.  There is no primary-key requirement.
 */
PG_FUNCTION_INFO_V1(pg_database_logical_checksum);

Datum
pg_database_logical_checksum(PG_FUNCTION_ARGS)
{
    bool        include_system = false;
    bool        include_indexes = true;
    uint64      db_checksum;

    /* Parse optional parameters */
    if (PG_NARGS() >= 1)
        include_system = PG_GETARG_BOOL(0);
    if (PG_NARGS() >= 2)
        include_indexes = PG_GETARG_BOOL(1);

    /* Security check: only superusers can checksum the entire database */
    if (!superuser())
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("must be superuser to compute database checksum")));

    /* Compute logical database checksum */
    db_checksum = compute_database_checksum_internal(false, include_system,
                                                     false, include_indexes, 0);

    PG_RETURN_INT64((int64)db_checksum);
}

/*
 * pg_database_logical_checksum_algo - generic logical database checksum
 */
PG_FUNCTION_INFO_V1(pg_database_logical_checksum_algo);

Datum
pg_database_logical_checksum_algo(PG_FUNCTION_ARGS)
{
    bool        include_system = false;
    bool        include_indexes = true;
    int         algorithm;
    uint64      db_checksum;

    if (PG_NARGS() >= 1)
        include_system = PG_GETARG_BOOL(0);
    if (PG_NARGS() >= 2)
        include_indexes = PG_GETARG_BOOL(1);
    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(2));

    if (!superuser())
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("must be superuser to compute database checksum")));

    db_checksum = compute_database_checksum_internal(false, include_system,
                                                     false, include_indexes,
                                                     algorithm);

    PG_RETURN_BYTEA_P(checksum_bytea(db_checksum, algorithm == 0 ? 4 : 8));
}

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

/*
 * pg_index_physical_checksum_algo - generic physical index checksum
 */
PG_FUNCTION_INFO_V1(pg_index_physical_checksum_algo);

Datum
pg_index_physical_checksum_algo(PG_FUNCTION_ARGS)
{
    Oid         indexoid;
    int         algorithm;
    bool        is_null = false;
    uint64      result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    indexoid = PG_GETARG_OID(0);
    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(1));

    if (algorithm == 0)
    {
        uint32      legacy = pg_index_physical_checksum_calc(indexoid, true,
                                                             &is_null);

        if (is_null)
            PG_RETURN_NULL();
        PG_RETURN_BYTEA_P(checksum_bytea(legacy, 4));
    }

    result = pg_index_physical_checksum_calc64(indexoid, true, &is_null);
    if (is_null)
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(result, 8));
}

/*
 * pg_database_physical_checksum_algo - generic physical database checksum
 */
PG_FUNCTION_INFO_V1(pg_database_physical_checksum_algo);

Datum
pg_database_physical_checksum_algo(PG_FUNCTION_ARGS)
{
    bool        include_system = false;
    bool        include_toast = false;
    int         algorithm;
    uint64      db_checksum;

    if (PG_NARGS() >= 1)
        include_system = PG_GETARG_BOOL(0);
    if (PG_NARGS() >= 2)
        include_toast = PG_GETARG_BOOL(1);
    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(2));

    if (!superuser())
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("must be superuser to compute database checksum")));

    db_checksum = compute_database_checksum_internal(true, include_system,
                                                     include_toast, true,
                                                     algorithm);

    PG_RETURN_BYTEA_P(checksum_bytea(db_checksum, 8));
}
