/*-------------------------------------------------------------------------
 *
 * pg_integrity_checksums.c
 *    Extension entry point and shared configuration helpers
 *
 * The checksum implementation is split across the checksum_*.c modules;
 * checksum_internal.h declares the interfaces they share.  This file owns
 * module loading, the extension GUCs and the common snapshot/worker helpers.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "pg_integrity_checksums.h"
#include "checksum_internal.h"

PG_MODULE_MAGIC;

/*-------------------------------------------------------------------------
 * Configuration and shared helpers
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
 * When true (the default), a relation that cannot be checksummed (invalid
 * index, unsupported relation kind, or a scan error) aborts the database
 * checksum.  When false, such a relation is skipped with a warning and the
 * result silently excludes it, so a corrupt table no longer shows up as a
 * changed checksum; that is only appropriate for ad-hoc use.
 */
bool pg_integrity_checksums_strict = true;

/* Upper bound for the GUC; mirrors the sanity limit of parallel workers. */
#define PG_INTEGRITY_CHECKSUMS_MAX_WORKERS 1024

/*
 * _PG_init - extension load-time initialization
 *
 * Registers the extension GUCs and the syscache invalidation callback used by
 * the canonical logical type cache.
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
                             "When enabled (the default), database checksum "
                             "functions abort if a relation cannot be "
                             "checksummed (invalid index, unsupported "
                             "relation kind, or error).  When disabled, such "
                             "relations are skipped with a warning.",
                             &pg_integrity_checksums_strict,
                             true,
                             PGC_USERSET, 0,
                             NULL, NULL, NULL);

    CacheRegisterSyscacheCallback(TYPEOID, logical_type_cache_invalidate, 0);
}

/*
 * pg_integrity_checksums_worker_count - determine how many parallel workers to use
 *
 * Returns 0 when the scan should run in a single backend.  This happens
 * when the GUC is zero (the default), when max_parallel_workers or
 * max_worker_processes is zero, or when parallel mode is already active
 * (a parallel context cannot be nested).  Otherwise the GUC value is
 * clamped to max_parallel_workers and max_worker_processes.  Callers further
 * clamp the result to the amount of work available.
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
