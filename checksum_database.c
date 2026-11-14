/*-------------------------------------------------------------------------
 *
 * checksum_database.c
 *    Whole-database checksum enumeration and SQL entry points
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/parallel.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "checksum_internal.h"

/*-------------------------------------------------------------------------
 * Database enumeration and aggregation
 *-------------------------------------------------------------------------
 */

/*
 * compute_database_item - compute the checksum of one relation
 *
 * Sets *skip when the relation has no usable checksum (for example an
 * invalid index, an unsupported relation kind, or a TOAST relation in logical
 * mode).
 */
static void
compute_database_item(Oid relid, char relkind, bool physical, ChecksumAlgo algo,
                      uint64 *rel_checksum, bool *skip)
{
    *rel_checksum = 0;
    *skip = false;

    if (relkind == RELKIND_INDEX)
    {
        bool        is_null = false;
        uint32      value = 0;

        if (physical && algo == CHECKSUM_ALGO_FNV64)
        {
            uint64      value64 = pg_index_physical_checksum_calc64(relid,
                                                                    false,
                                                                    &is_null);

            if (!is_null)
                *rel_checksum = value64;
        }
        else if (physical)
            value = pg_index_physical_checksum_calc(relid, false, &is_null);
        else if (algo == CHECKSUM_ALGO_FNV64)
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
        else if (algo == CHECKSUM_ALGO_FNV32)
            *rel_checksum = (uint64) value;
    }
    else
    {
        if (physical && algo == CHECKSUM_ALGO_FNV64)
            *rel_checksum = pg_table_physical_checksum_calc64(relid, false,
                                                              false);
        else if (physical)
            *rel_checksum = pg_table_physical_checksum_calc(relid, false,
                                                            false);
        else
        {
            bool        is_null = false;

            if (algo == CHECKSUM_ALGO_FNV64)
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
 * skipped_relation_reason - why a relation has no usable checksum
 *
 * Only called for relations that compute_database_item() reported as skipped,
 * which is either an invalid index or a relation kind without a checksum for
 * the requested mode.
 */
static const char *
skipped_relation_reason(Oid relid)
{
    if (get_rel_relkind(relid) == RELKIND_INDEX)
    {
        HeapTuple   tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(relid));

        if (HeapTupleIsValid(tuple))
        {
            bool        valid = ((Form_pg_index) GETSTRUCT(tuple))->indisvalid;

            ReleaseSysCache(tuple);

            if (!valid)
                return "The index is invalid and has no content to checksum";
        }
    }

    return "The relation kind or access method has no checksum in this mode";
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
             errdetail("%s.", skipped_relation_reason(relid)),
             errhint("Set pg_integrity_checksums.strict = false to skip relations without a usable checksum.")));
}

/*
 * handle_skipped_relation - a relation has no usable checksum
 *
 * An error in strict mode; otherwise the relation is left out of the result
 * and counted, so the caller can report how many were skipped.
 */
static void
handle_skipped_relation(Oid relid, uint32 *skipped)
{
    pg_integrity_checksums_report_skipped_relation(relid);

    if (skipped != NULL)
        (*skipped)++;
}

/*
 * add_relation_result - fold one relation's checksum into the aggregate
 *
 * The accumulator is chosen by which pointer is set: logical_acc64 for
 * 64-bit logical scans, logical_acc for 32-bit logical scans, otherwise the
 * physical accumulator (which also mixes in the relation OID).
 */
static void
add_relation_result(Oid relid, uint64 rel_checksum, ChecksumAccum64 *acc,
                    ChecksumLogicalAccum *logical_acc,
                    ChecksumLogicalAccum64 *logical_acc64)
{
    if (logical_acc64 != NULL)
        logical_accum_add64(logical_acc64, rel_checksum);
    else if (logical_acc != NULL)
        logical_accum_add(logical_acc, (uint32) rel_checksum);
    else
        checksum_accum64_add(acc, combine_checksums_64(rel_checksum,
                                                       (uint64) relid));
}

/*
 * is_fatal_scan_error - errors that are never caught and skipped
 *
 * Out-of-memory and shutdown errors are re-thrown immediately: the backend is
 * going away or cannot be trusted.
 */
static bool
is_fatal_scan_error(int sqlerrcode)
{
    return sqlerrcode == ERRCODE_OUT_OF_MEMORY ||
        sqlerrcode == ERRCODE_ADMIN_SHUTDOWN ||
        sqlerrcode == ERRCODE_CRASH_SHUTDOWN ||
        sqlerrcode == ERRCODE_CANNOT_CONNECT_NOW;
}

/*
 * compute_database_relation - checksum one relation of a database scan
 */
static void
compute_database_relation(Oid relid, char relkind, bool physical,
                          ChecksumAlgo algo, ChecksumAccum64 *acc,
                          ChecksumLogicalAccum *logical_acc,
                          ChecksumLogicalAccum64 *logical_acc64,
                          uint32 *skipped)
{
    uint64      rel_checksum = 0;
    bool        skip = false;
    MemoryContext oldcontext;
    ResourceOwner oldowner;

#if PG_VERSION_NUM < 170000
    /*
     * PostgreSQL 12-16 prohibit internal subtransactions while in parallel
     * mode.  The leader of a parallel database scan is in parallel mode too,
     * so neither the leader nor a worker can use the subtransaction-based
     * error recovery below.  Run the relation checksum directly; an error
     * then aborts the parallel scan.  There is no safe way to catch and
     * continue without a subtransaction.
     */
    if (IsInParallelMode())
    {
        compute_database_item(relid, relkind, physical, algo, &rel_checksum,
                              &skip);
        if (skip)
            handle_skipped_relation(relid, skipped);
        else
            add_relation_result(relid, rel_checksum, acc, logical_acc,
                                logical_acc64);
        return;
    }
#endif

    /*
     * Each relation is checksummed in an internal subtransaction.  If a
     * relation cannot be processed (dropped concurrently, corrupt,
     * unsupported, ...), the subtransaction rollback releases its locks and
     * buffer pins, and the scan can safely continue.  Catching an error
     * without a subtransaction would leave resources behind.
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

        if (!skip)
            add_relation_result(relid, rel_checksum, acc, logical_acc,
                                logical_acc64);

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
         * For every error but the fatal ones the internal subtransaction is
         * rolled back first, because a caller (for example a PL/pgSQL
         * exception block) may catch the error and continue, and an open
         * child transaction would keep IsInParallelMode() true.  After the
         * rollback, strict mode and query cancellation re-throw the error;
         * any other error is reported as a warning and the relation is
         * skipped.
         */
        if (is_fatal_scan_error(sqlerrcode))
            PG_RE_THROW();

        MemoryContextSwitchTo(oldcontext);
        edata = CopyErrorData();
        FlushErrorState();

        /* Abort the inner transaction */
        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcontext);
        CurrentResourceOwner = oldowner;

        if (skipped != NULL)
            (*skipped)++;

        if (pg_integrity_checksums_strict || sqlerrcode == ERRCODE_QUERY_CANCELED)
        {
            /*
             * Name the relation: many errors (a missing file, a corrupt
             * page) do not say which relation the scan was working on.
             */
            if (sqlerrcode != ERRCODE_QUERY_CANCELED)
            {
                char       *relname = get_rel_name(relid);

                edata->context = psprintf("while computing the database checksum of relation \"%s\" (OID %u)%s%s",
                                          relname ? relname : "?", relid,
                                          edata->context ? "\n" : "",
                                          edata->context ? edata->context : "");
            }

            ReThrowError(edata);
        }

        /*
         * Parallel workers log to the server log; the leader emits a summary
         * warning when it folds the workers' results.
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

    /*
     * A relation without a usable checksum is an error in strict mode.  Raise
     * it here, after the subtransaction has been released, so it is an
     * ordinary error and not one caught by the handler above.
     */
    if (skip)
        handle_skipped_relation(relid, skipped);
}

/*-------------------------------------------------------------------------
 * Logical table groups: one heap pass for a table and its indexes
 *-------------------------------------------------------------------------
 */

/*
 * LogicalGroupState - visitor state of a grouped logical heap pass
 *
 * accs[0] belongs to the table, accs[1..nindexes] to its indexes.  The
 * 64-bit accumulator layout is used for both algorithms: the 32-bit logical
 * accumulator has the same (sum, count) fields and is filled the same way.
 */
typedef struct LogicalGroupState
{
    ChecksumAlgo algo;
    LogicalRowHasher hasher;
    int         nindexes;
    IndexLogicalScanState *indexes;
    bool       *index_valid;
    ChecksumLogicalAccum64 *accs;
} LogicalGroupState;

static inline void
group_accum_add(ChecksumLogicalAccum64 *acc, uint64 h, ChecksumAlgo algo)
{
    if (algo == CHECKSUM_ALGO_FNV32)
        acc->sum += hash_mix64((uint64) (uint32) h);
    else
        acc->sum += hash_mix64(h);
    acc->count++;
}

static uint64
group_accum_finalize(ChecksumLogicalAccum64 *acc, ChecksumAlgo algo)
{
    if (algo == CHECKSUM_ALGO_FNV32)
    {
        ChecksumLogicalAccum acc32 = {acc->sum, acc->count};

        return (uint64) logical_accum_finalize(&acc32);
    }

    return logical_accum_finalize64(acc);
}

static void
logical_group_visitor(Relation rel, Page page, BlockNumber blkno,
                      OffsetNumber offnum, HeapTuple tuple, void *arg)
{
    LogicalGroupState *state = (LogicalGroupState *) arg;
    int         i;

    group_accum_add(&state->accs[0],
                    logical_row_hash(&state->hasher, tuple, state->algo),
                    state->algo);

    for (i = 0; i < state->nindexes; i++)
    {
        uint64      row_hash;

        if (state->index_valid[i] &&
            pg_index_logical_row_hash(&state->indexes[i], tuple, state->algo,
                                      &row_hash))
            group_accum_add(&state->accs[i + 1], row_hash, state->algo);
    }
}

/*
 * compute_logical_group - logical checksums of a table and its indexes
 *
 * items[0] is the table and items[1..n-1] are indexes on it.  The visible
 * heap tuples are read once; every tuple is hashed as a table row and, for
 * each index, as that index's entry.  The per-relation results are exactly
 * those of pg_table_logical_checksum_calc() and
 * pg_index_logical_checksum_calc() with the same snapshot: the same tuples
 * are visited, and the order in which they are accumulated does not matter.
 *
 * results[i] receives the checksum of items[i]; skip[i] is set for an index
 * that has no logical checksum (invalid, not ready, ...).
 */
static void
compute_logical_group(ChecksumRelationItem *items, int n, ChecksumAlgo algo,
                      uint64 *results, bool *skip)
{
    LogicalGroupState state;
    Relation    rel;
    Snapshot    snapshot = pg_integrity_checksums_get_snapshot();
    BufferAccessStrategy bstrategy;
    BlockNumber nblocks;
    int         i;

    rel = relation_open(items[0].reloid, AccessShareLock);
    pg_integrity_checksums_check_relation_kind(rel, true, true);

    state.algo = algo;
    state.nindexes = n - 1;
    state.indexes = palloc0(sizeof(IndexLogicalScanState) * state.nindexes);
    state.index_valid = palloc0(sizeof(bool) * state.nindexes);
    state.accs = palloc0(sizeof(ChecksumLogicalAccum64) * n);
    logical_row_hasher_init(&state.hasher, RelationGetDescr(rel));

    skip[0] = false;
    for (i = 0; i < state.nindexes; i++)
    {
        state.index_valid[i] = pg_index_logical_scan_init(items[i + 1].reloid,
                                                          snapshot,
                                                          &state.indexes[i]);
        skip[i + 1] = !state.index_valid[i];
    }

    nblocks = RelationGetNumberOfBlocks(rel);
    bstrategy = GetAccessStrategy(BAS_BULKREAD);
    scan_visible_tuples(rel, 0, nblocks, snapshot, bstrategy, false,
                        logical_group_visitor, &state);
    FreeAccessStrategy(bstrategy);

    for (i = 0; i < n; i++)
        results[i] = skip[i] ? 0 : group_accum_finalize(&state.accs[i], algo);

    for (i = 0; i < state.nindexes; i++)
    {
        if (state.index_valid[i])
            pg_index_logical_scan_end(&state.indexes[i]);
    }
    logical_row_hasher_free(&state.hasher);
    pfree(state.indexes);
    pfree(state.index_valid);
    pfree(state.accs);

    relation_close(rel, AccessShareLock);
}

/*
 * add_group_results - fold the results of compute_logical_group()
 */
static void
add_group_results(ChecksumRelationItem *items, int n, uint64 *results,
                  bool *skip, ChecksumLogicalAccum *logical_acc,
                  ChecksumLogicalAccum64 *logical_acc64, uint32 *skipped)
{
    int         i;

    for (i = 0; i < n; i++)
    {
        if (skip[i])
            handle_skipped_relation(items[i].reloid, skipped);
        else
            add_relation_result(items[i].reloid, results[i], NULL,
                                logical_acc, logical_acc64);
    }
}

/*
 * compute_database_group - logical database checksum of one table group
 *
 * The group runs in one internal subtransaction.  If it fails for any reason
 * other than a fatal error or a cancellation, the subtransaction is rolled
 * back and the relations are computed again one by one, so the outcome
 * (warnings, strict-mode errors naming the relation, skipped count) is that
 * of the per-relation scan.  Errors are rare, so the second pass costs
 * nothing in the normal case.
 */
static void
compute_database_group(ChecksumRelationItem *items, int n, ChecksumAlgo algo,
                       ChecksumLogicalAccum *logical_acc,
                       ChecksumLogicalAccum64 *logical_acc64,
                       uint32 *skipped)
{
    uint64     *results = palloc(sizeof(uint64) * n);
    bool       *skip = palloc(sizeof(bool) * n);
    MemoryContext oldcontext;
    ResourceOwner oldowner;
    bool        failed = false;
    int         i;

#if PG_VERSION_NUM < 170000
    /* No subtransactions in parallel mode; see compute_database_relation(). */
    if (IsInParallelMode())
    {
        compute_logical_group(items, n, algo, results, skip);
        add_group_results(items, n, results, skip, logical_acc,
                          logical_acc64, skipped);
        pfree(results);
        pfree(skip);
        return;
    }
#endif

    oldcontext = CurrentMemoryContext;
    oldowner = CurrentResourceOwner;

    BeginInternalSubTransaction(NULL);
    MemoryContextSwitchTo(oldcontext);

    PG_TRY();
    {
        compute_logical_group(items, n, algo, results, skip);

        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcontext);
        CurrentResourceOwner = oldowner;
    }
    PG_CATCH();
    {
        int         sqlerrcode = geterrcode();
        ErrorData  *edata;

        if (is_fatal_scan_error(sqlerrcode))
            PG_RE_THROW();

        MemoryContextSwitchTo(oldcontext);
        edata = CopyErrorData();
        FlushErrorState();

        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcontext);
        CurrentResourceOwner = oldowner;

        /* A cancellation is not retried. */
        if (sqlerrcode == ERRCODE_QUERY_CANCELED)
            ReThrowError(edata);

        FreeErrorData(edata);
        failed = true;
    }
    PG_END_TRY();

    if (failed)
    {
        for (i = 0; i < n; i++)
            compute_database_relation(items[i].reloid, items[i].relkind,
                                      false, algo, NULL, logical_acc,
                                      logical_acc64, skipped);
    }
    else
        add_group_results(items, n, results, skip, logical_acc,
                          logical_acc64, skipped);

    pfree(results);
    pfree(skip);
}

/*
 * compute_database_items - database checksum over a range of relations
 *
 * Items with group_size 0 are covered by the head of their group, which may
 * lie outside [start, end): a parallel participant that claims such an item
 * has nothing to do.
 */
void
compute_database_items(ChecksumRelationItem *items, uint32 start, uint32 end,
                       bool physical, ChecksumAlgo algo, ChecksumAccum64 *acc,
                       ChecksumLogicalAccum *logical_acc,
                       ChecksumLogicalAccum64 *logical_acc64, uint32 *skipped)
{
    uint32      i;

    for (i = start; i < end; i++)
    {
        if (items[i].group_size == 0)
            continue;

        if (items[i].group_size > 1)
        {
            Assert(!physical);
            compute_database_group(&items[i], (int) items[i].group_size, algo,
                                   logical_acc, logical_acc64, skipped);
        }
        else
            compute_database_relation(items[i].reloid, items[i].relkind,
                                      physical, algo, acc, logical_acc,
                                      logical_acc64, skipped);

        CHECK_FOR_INTERRUPTS();
    }
}

/*
 * GroupSortItem - sort key that puts every index right after its table
 */
typedef struct GroupSortItem
{
    ChecksumRelationItem item;
    Oid         group_oid;      /* the table: its own OID, or the index's table */
    bool        is_index;
} GroupSortItem;

static int
group_sort_cmp(const void *a, const void *b)
{
    const GroupSortItem *ga = (const GroupSortItem *) a;
    const GroupSortItem *gb = (const GroupSortItem *) b;

    if (ga->group_oid != gb->group_oid)
        return ga->group_oid < gb->group_oid ? -1 : 1;
    if (ga->is_index != gb->is_index)
        return ga->is_index ? 1 : -1;
    if (ga->item.reloid != gb->item.reloid)
        return ga->item.reloid < gb->item.reloid ? -1 : 1;
    return 0;
}

/*
 * group_database_items - order items so tables are followed by their indexes
 *
 * Sets group_size: the number of indexes plus one for a table, 0 for an
 * index that follows its table, and 1 for an index whose table is not part
 * of the scan.  The order of the items does not change the database
 * checksum, which aggregates per-relation results order-independently.
 */
static void
group_database_items(ChecksumRelationItem *items, uint32 nitems)
{
    GroupSortItem *sorted = palloc(sizeof(GroupSortItem) * Max(nitems, 1));
    uint32      i;

    for (i = 0; i < nitems; i++)
    {
        sorted[i].item = items[i];
        sorted[i].is_index = (items[i].relkind == RELKIND_INDEX);
        sorted[i].group_oid = sorted[i].is_index ?
            IndexGetRelation(items[i].reloid, true) : items[i].reloid;

        /* A concurrently dropped index: leave it to the per-relation path. */
        if (!OidIsValid(sorted[i].group_oid))
            sorted[i].group_oid = items[i].reloid;
    }

    qsort(sorted, nitems, sizeof(GroupSortItem), group_sort_cmp);

    i = 0;
    while (i < nitems)
    {
        uint32      j = i + 1;

        if (!sorted[i].is_index)
        {
            while (j < nitems && sorted[j].is_index &&
                   sorted[j].group_oid == sorted[i].item.reloid)
                j++;
        }

        items[i] = sorted[i].item;
        items[i].group_size = j - i;
        for (uint32 k = i + 1; k < j; k++)
        {
            items[k] = sorted[k].item;
            items[k].group_size = 0;
        }
        i = j;
    }

    pfree(sorted);
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
                                  ChecksumAlgo algo)
{
    Relation    pg_class_rel;
    SysScanDesc scan;
    HeapTuple   classTuple;
    Snapshot    snapshot;
    ChecksumRelationItem *items;
    uint32      nitems = 0;
    uint32      capacity = 64;
    ChecksumAccum64 acc = {0, 0};
    ChecksumLogicalAccum lacc = {0, 0};
    ChecksumLogicalAccum64 lacc64 = {0, 0};
    uint32      skipped = 0;
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

    /*
     * A sequential catalog scan with the statement snapshot.  The
     * systable_beginscan() signature is the same on every supported release,
     * unlike table_beginscan(), which gained an argument in PostgreSQL 19.
     */
    scan = systable_beginscan(pg_class_rel, InvalidOid, false, snapshot,
                              0, NULL);

    while ((classTuple = systable_getnext(scan)) != NULL)
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
        items[nitems].group_size = 1;
        nitems++;

        CHECK_FOR_INTERRUPTS();
    }

    systable_endscan(scan);
    table_close(pg_class_rel, AccessShareLock);

    workers = pg_integrity_checksums_worker_count();
    if (nitems < (uint32) workers)
        workers = (int) nitems;

    /*
     * A logical index checksum is computed from the heap, so a serial scan
     * reads each table once for itself and all of its indexes instead of
     * once per relation.  A parallel scan hands out whole relations, and
     * grouping would give one participant a table together with all of its
     * indexes; there the per-relation split balances the work better.
     */
    if (!physical && include_indexes && workers < 1)
        group_database_items(items, nitems);

    if (workers < 1)
        compute_database_items(items, 0, nitems, physical, algo, &acc,
                               physical ? NULL : &lacc,
                               (!physical && algo == CHECKSUM_ALGO_FNV64) ? &lacc64 : NULL,
                               &skipped);
    else
        pg_integrity_checksums_parallel_scan_database(physical, items, nitems,
                                            workers, algo, &acc,
                                            physical ? NULL : &lacc,
                                            (!physical && algo == CHECKSUM_ALGO_FNV64) ? &lacc64 : NULL,
                                            &skipped);

    pfree(items);

    if (skipped > 0)
        ereport(WARNING,
                (errmsg("database checksum skipped %u relation(s)", skipped),
                 errhint("Set pg_integrity_checksums.strict = on to fail with the reason for the first skipped relation, or inspect the server log.")));

    if (physical)
        return checksum_accum64_finalize(&acc);

    if (algo == CHECKSUM_ALGO_FNV64)
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
                                                     include_toast, true,
                                                     CHECKSUM_ALGO_FNV32);

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
                                                     false, include_indexes,
                                                     CHECKSUM_ALGO_FNV32);

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
    ChecksumAlgo         algorithm;
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

    PG_RETURN_BYTEA_P(checksum_bytea(db_checksum, algorithm == CHECKSUM_ALGO_FNV32 ? 4 : 8));
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
    ChecksumAlgo         algorithm;
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
