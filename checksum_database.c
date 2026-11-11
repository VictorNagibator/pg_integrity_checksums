/*-------------------------------------------------------------------------
 *
 * checksum_database.c
 *    Whole-database checksum enumeration and SQL entry points
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/heapam.h"
#include "access/parallel.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "checksum_internal.h"

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
void
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
