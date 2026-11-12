/*-------------------------------------------------------------------------
 *
 * checksum_parallel.c
 *    Dynamic shared-memory parallel checksum execution
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/heapam.h"
#include "access/brin.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "miscadmin.h"
#include "storage/dsm.h"
#include "storage/shm_toc.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "checksum_internal.h"

/*-------------------------------------------------------------------------
 * PARALLEL SCAN IMPLEMENTATION
 *-------------------------------------------------------------------------
 *
 * The serial block-range helpers live in checksum_scan.c.  This module
 * splits a scan into block or relation ranges and gives each range to the
 * leader or a worker through an atomic work counter.  Because the
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
 * ScanAccums - one accumulator of every kind, for the block-scan tasks
 *
 * A task only touches the accumulator that matches its type and algorithm;
 * keeping all four lets the leader and the workers share one code path.
 */
typedef struct ScanAccums
{
    ChecksumAccum physical32;
    ChecksumAccum64 physical64;
    ChecksumLogicalAccum logical32;
    ChecksumLogicalAccum64 logical64;
} ScanAccums;

/* Is this a table/index task whose result is a logical (sum, count) pair? */
static bool
task_is_logical(ChecksumTaskType task_type)
{
    return task_type == CHECKSUM_TASK_TABLE_LOGICAL ||
        task_type == CHECKSUM_TASK_INDEX_LOGICAL;
}

/*
 * run_block_task - claim block chunks and scan them
 *
 * Shared by the leader and the workers of a table or index scan.  Claims
 * chunks of blocks from the shared counter until none remain and
 * accumulates them into accs.
 */
static void
run_block_task(ChecksumParallelState *state, Snapshot snapshot,
               ScanAccums *accs)
{
    ChecksumAlgo algo = state->algo;
    uint32      start;
    uint32      end;

    switch (state->task_type)
    {
        case CHECKSUM_TASK_TABLE_PHYSICAL:
        case CHECKSUM_TASK_TABLE_LOGICAL:
            {
                Relation    rel = relation_open(state->reloid, AccessShareLock);
                BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

                while (pg_integrity_checksums_consume(state,
                                                      PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                      &start, &end))
                {
                    if (state->task_type == CHECKSUM_TASK_TABLE_PHYSICAL)
                    {
                        if (algo == CHECKSUM_ALGO_FNV64)
                            scan_table_blocks_physical64(rel, start, end,
                                                         state->include_header,
                                                         snapshot,
                                                         &accs->physical64,
                                                         bstrategy);
                        else
                            scan_table_blocks_physical(rel, start, end,
                                                       state->include_header,
                                                       snapshot,
                                                       &accs->physical32,
                                                       bstrategy);
                    }
                    else if (algo == CHECKSUM_ALGO_FNV64)
                        scan_table_blocks_logical64(rel, start, end, snapshot,
                                                    &accs->logical64,
                                                    bstrategy);
                    else
                        scan_table_blocks_logical(rel, start, end, snapshot,
                                                  &accs->logical32, bstrategy);
                }

                relation_close(rel, AccessShareLock);
                FreeAccessStrategy(bstrategy);
                break;
            }
        case CHECKSUM_TASK_INDEX_PHYSICAL:
            {
                Relation    idxRel = index_open(state->reloid, AccessShareLock);
                bool        is_brin = (idxRel->rd_rel->relam == BRIN_AM_OID);
                BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

                while (pg_integrity_checksums_consume(state,
                                                      PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                      &start, &end))
                {
                    if (algo == CHECKSUM_ALGO_FNV64)
                        scan_index_blocks_physical64(idxRel, start, end,
                                                     is_brin,
                                                     &accs->physical64,
                                                     bstrategy);
                    else
                        scan_index_blocks_physical(idxRel, start, end, is_brin,
                                                   &accs->physical32,
                                                   bstrategy);
                }

                index_close(idxRel, AccessShareLock);
                FreeAccessStrategy(bstrategy);
                break;
            }
        case CHECKSUM_TASK_INDEX_LOGICAL:
            {
                IndexLogicalScanState scanstate;

                /* scanstate owns its buffer strategy. */
                if (!pg_index_logical_scan_init(state->reloid, snapshot,
                                                &scanstate))
                    elog(ERROR, "could not initialize logical index scan for index %u",
                         state->reloid);

                while (pg_integrity_checksums_consume(state,
                                                      PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK,
                                                      &start, &end))
                {
                    if (algo == CHECKSUM_ALGO_FNV64)
                        pg_index_logical_scan_blocks64(&scanstate, start, end,
                                                       &accs->logical64);
                    else
                        pg_index_logical_scan_blocks(&scanstate, start, end,
                                                     &accs->logical32);
                }

                pg_index_logical_scan_end(&scanstate);
                break;
            }
        default:
            elog(ERROR, "unexpected checksum block task type %d",
                 (int) state->task_type);
    }
}

/*
 * accums_to_result - reduce the accumulator a task used to (partial, count)
 *
 * This is the fixed-size form written to shared memory by workers: the XOR
 * accumulator for physical tasks, the sum for logical tasks.
 */
static void
accums_to_result(ChecksumTaskType task_type, ChecksumAlgo algo,
                 const ScanAccums *accs, uint64 *partial, uint64 *count)
{
    if (task_is_logical(task_type))
    {
        if (algo == CHECKSUM_ALGO_FNV64)
        {
            *partial = accs->logical64.sum;
            *count = accs->logical64.count;
        }
        else
        {
            *partial = accs->logical32.sum;
            *count = accs->logical32.count;
        }
    }
    else if (algo == CHECKSUM_ALGO_FNV64)
    {
        *partial = accs->physical64.partial;
        *count = accs->physical64.count;
    }
    else
    {
        *partial = accs->physical32.partial;
        *count = accs->physical32.count;
    }
}

/*
 * fold_result - merge one participant's (partial, count) into the caller's
 * accumulators
 */
static void
fold_result(ChecksumTaskType task_type, ChecksumAlgo algo, uint64 partial,
            uint64 count, ChecksumAccum *acc, ChecksumLogicalAccum *logical_acc,
            ChecksumAccum64 *acc64, ChecksumLogicalAccum64 *logical_acc64)
{
    if (task_is_logical(task_type))
    {
        if (algo == CHECKSUM_ALGO_FNV64)
        {
            logical_acc64->sum += partial;
            logical_acc64->count += count;
        }
        else
        {
            logical_acc->sum += partial;
            logical_acc->count += count;
        }
    }
    else if (algo == CHECKSUM_ALGO_FNV64)
        checksum_accum64_merge(acc64, partial, count);
    else
        checksum_accum_merge(acc, partial, count);
}

/*
 * pg_integrity_checksums_parallel_worker_main - entry point for parallel workers
 *
 * Restores the shared scan state and snapshot, then consumes items from the
 * shared atomic counter until none remain, writing a fixed-size result back
 * to shared memory before exiting.
 */
PGDLLEXPORT void
pg_integrity_checksums_parallel_worker_main(dsm_segment *seg, shm_toc *toc)
{
    ChecksumParallelState *state = shm_toc_lookup(toc, CHECKSUM_KEY_STATE, false);
    ChecksumWorkerResult *results = shm_toc_lookup(toc, CHECKSUM_KEY_RESULTS, false);
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
        char       *snap = shm_toc_lookup(toc, CHECKSUM_KEY_SNAPSHOT, false);

        snapshot = RestoreSnapshot(snap);
    }

    switch (state->task_type)
    {
        case CHECKSUM_TASK_TABLE_PHYSICAL:
        case CHECKSUM_TASK_TABLE_LOGICAL:
        case CHECKSUM_TASK_INDEX_PHYSICAL:
        case CHECKSUM_TASK_INDEX_LOGICAL:
            {
                ScanAccums  accs = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};

                run_block_task(state, snapshot, &accs);
                accums_to_result(state->task_type, state->algo, &accs,
                                 &myresult->partial, &myresult->count);
                break;
            }
        case CHECKSUM_TASK_DATABASE_PHYSICAL:
        case CHECKSUM_TASK_DATABASE_LOGICAL:
            {
                ChecksumRelationItem *items = shm_toc_lookup(toc, CHECKSUM_KEY_ITEMS, false);
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
                                           (!physical && state->algo == CHECKSUM_ALGO_FNV64) ? &lacc64 : NULL,
                                           &myresult->had_error);

                if (pushed)
                    PopActiveSnapshot();

                if (physical)
                {
                    myresult->partial = acc64.partial;
                    myresult->count = acc64.count;
                }
                else if (state->algo == CHECKSUM_ALGO_FNV64)
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
                                  int workers, ChecksumAlgo algo, ChecksumAccum *acc,
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
        shm_toc_insert(pcxt->toc, CHECKSUM_KEY_STATE, state);
        state->task_type = task_type;
        state->reloid = reloid;
        state->include_header = include_header;
        state->algo = algo;
        state->nitems = nblocks;
        state->snapshot_size = snapshot_size;
        pg_atomic_init_u32(&state->next_item, 0);

        results = shm_toc_allocate(pcxt->toc, sizeof(ChecksumWorkerResult) * workers);
        shm_toc_insert(pcxt->toc, CHECKSUM_KEY_RESULTS, results);
        memset(results, 0, sizeof(ChecksumWorkerResult) * workers);

        if (snapshot_size > 0)
        {
            char       *snap = shm_toc_allocate(pcxt->toc, snapshot_size);

            SerializeSnapshot(snapshot, snap);
            shm_toc_insert(pcxt->toc, CHECKSUM_KEY_SNAPSHOT, snap);
        }

        LaunchParallelWorkers(pcxt);

        if (pcxt->nworkers_launched > 0)
            WaitForParallelWorkersToAttach(pcxt);

        /*
         * The leader processes its own share of the work, then folds its
         * result in exactly like a worker's.
         */
        {
            ScanAccums  accs = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
            uint64      partial;
            uint64      count;

            run_block_task(state, snapshot, &accs);
            accums_to_result(task_type, algo, &accs, &partial, &count);
            fold_result(task_type, algo, partial, count, acc, logical_acc,
                        acc64, logical_acc64);
        }

        if (pcxt->nworkers_launched > 0)
            WaitForParallelWorkersToFinish(pcxt);

        /* Fold in the workers' results. */
        for (i = 0; i < workers; i++)
        {
            if (pcxt->worker[i].bgwhandle != NULL)
                fold_result(task_type, algo, results[i].partial,
                            results[i].count, acc, logical_acc, acc64,
                            logical_acc64);
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
                                    int workers, ChecksumAlgo algo,
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
        shm_toc_insert(pcxt->toc, CHECKSUM_KEY_STATE, state);
        state->task_type = physical ? CHECKSUM_TASK_DATABASE_PHYSICAL
                                    : CHECKSUM_TASK_DATABASE_LOGICAL;
        state->reloid = InvalidOid;
        state->include_header = false;
        state->algo = algo;
        state->nitems = nitems;
        state->snapshot_size = snapshot_size;
        pg_atomic_init_u32(&state->next_item, 0);

        results = shm_toc_allocate(pcxt->toc, sizeof(ChecksumWorkerResult) * workers);
        shm_toc_insert(pcxt->toc, CHECKSUM_KEY_RESULTS, results);
        memset(results, 0, sizeof(ChecksumWorkerResult) * workers);

        {
            char       *snap = shm_toc_allocate(pcxt->toc, snapshot_size);

            SerializeSnapshot(snapshot, snap);
            shm_toc_insert(pcxt->toc, CHECKSUM_KEY_SNAPSHOT, snap);
        }

        {
            ChecksumRelationItem *ditems = shm_toc_allocate(pcxt->toc,
                                                            sizeof(ChecksumRelationItem) * nitems);

            memcpy(ditems, items, sizeof(ChecksumRelationItem) * nitems);
            shm_toc_insert(pcxt->toc, CHECKSUM_KEY_ITEMS, ditems);
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
                else if (algo == CHECKSUM_ALGO_FNV64)
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

