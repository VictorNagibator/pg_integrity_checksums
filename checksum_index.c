/*-------------------------------------------------------------------------
 *
 * checksum_index.c
 *    Physical and logical index checksums
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/brin.h"
#include "access/relation.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_proc.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/typcache.h"

#include "checksum_internal.h"

static uint64 pg_index_physical_checksum_common(Oid indexoid,
                                                bool check_privileges,
                                                ChecksumAlgo algo,
                                                bool *is_null);
static uint64 pg_index_logical_checksum_common(Oid indexoid,
                                               bool check_privileges,
                                               ChecksumAlgo algo,
                                               bool *is_null);

/*
 * Accumulator helpers: fold into or finalize the accumulator that matches
 * "algo"; the other one is ignored.  32-bit results are zero-extended.
 */
static inline void
physical_accum_add(ChecksumAlgo algo, ChecksumAccum *acc32,
                   ChecksumAccum64 *acc64, uint64 h)
{
    if (checksum_algo_is32(algo))
        checksum_accum_add(acc32, (uint32) h);
    else
        checksum_accum64_add(acc64, h);
}

static inline uint64
physical_accum_finalize(ChecksumAlgo algo, ChecksumAccum *acc32,
                        ChecksumAccum64 *acc64)
{
    if (checksum_algo_is32(algo))
        return checksum_accum_finalize(acc32);

    return checksum_accum64_finalize(acc64);
}

static inline void
logical_accum_add_algo(ChecksumAlgo algo, ChecksumLogicalAccum *acc32,
                       ChecksumLogicalAccum64 *acc64, uint64 h)
{
    if (checksum_algo_is32(algo))
        logical_accum_add(acc32, (uint32) h);
    else
        logical_accum_add64(acc64, h);
}

/*-------------------------------------------------------------------------
 * INDEX LEVEL FUNCTIONS (Physical and Logical)
 *-------------------------------------------------------------------------
 */

/*
 * Index Support Functions
 */

/*
 * index_supports_checksum - Check if index access method supports checksums
 *
 * Returns true for supported index types: B-tree, Hash, GiST, GIN,
 * SP-GiST, and BRIN. Other index types (like Bloom, Rum) are not
 * supported due to their specialized structures.
 */
static bool
index_supports_checksum(Oid amoid)
{
    return (amoid == BTREE_AM_OID ||
            amoid == HASH_AM_OID ||
            amoid == GIST_AM_OID ||
            amoid == GIN_AM_OID ||
            amoid == SPGIST_AM_OID ||
            amoid == BRIN_AM_OID);
}

/*-------------------------------------------------------------------------
 * Physical Index Checksum Functions
 *-------------------------------------------------------------------------
 */

/*
 * pg_index_physical_checksum - SQL function for physical index checksums
 *
 * Public interface for computing physical index checksums.
 * Supports all major index types. Returns NULL for unsupported index types.
 * Partitioned indexes are aggregated over their leaf indexes.  Empty indexes are still distinguished by
 * their relation and storage identity.
 */
PG_FUNCTION_INFO_V1(pg_index_physical_checksum);

Datum
pg_index_physical_checksum(PG_FUNCTION_ARGS)
{
    Oid         indexoid;
    bool        is_null = false;
    uint32      index_checksum;

    indexoid = PG_GETARG_OID(0);

    index_checksum = (uint32) pg_index_physical_checksum_calc(indexoid, true,
                                                              CHECKSUM_ALGO_FNV32,
                                                              &is_null);

    if (is_null)
        PG_RETURN_NULL();

    PG_RETURN_INT32((int32) index_checksum);
}

/*
 * pg_index_physical_checksum_algo - generic physical index checksum
 */
PG_FUNCTION_INFO_V1(pg_index_physical_checksum_algo);

Datum
pg_index_physical_checksum_algo(PG_FUNCTION_ARGS)
{
    Oid         indexoid;
    ChecksumAlgo algorithm;
    bool        is_null = false;
    uint64      result;

    indexoid = PG_GETARG_OID(0);
    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(1));

    result = pg_index_physical_checksum_common(indexoid, true, algorithm,
                                               &is_null);
    if (is_null)
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(result,
                                     checksum_algo_len(algorithm)));
}

/*
 * pg_index_physical_partitioned_checksum - aggregate leaf index checksums
 */
static uint64
pg_index_physical_partitioned_checksum(Oid indexoid, ChecksumAlgo algo,
                                       bool *is_null)
{
    List       *parts = pg_integrity_checksums_leaf_indexes(indexoid);
    ChecksumAccum acc32 = {0, 0};
    ChecksumAccum64 acc64 = {0, 0};
    ListCell   *lc;

    *is_null = false;

    foreach(lc, parts)
    {
        Oid         partoid = lfirst_oid(lc);
        bool        part_null = false;
        uint64      part_checksum;

        part_checksum = pg_index_physical_checksum_common(partoid, false, algo,
                                                          &part_null);
        if (part_null)
            *is_null = true;
        else
            physical_accum_add(algo, &acc32, &acc64,
                               combine_hash(part_checksum, (uint64) partoid,
                                            algo));
    }

    list_free(parts);

    if (*is_null)
        return 0;

    return combine_hash(physical_accum_finalize(algo, &acc32, &acc64),
                        (uint64) indexoid, algo);
}

/*
 * pg_index_physical_checksum_common - physical index checksum for one algo
 *
 * Sets *is_null for unsupported access methods.
 */
static uint64
pg_index_physical_checksum_common(Oid indexoid, bool check_privileges,
                                  ChecksumAlgo algo, bool *is_null)
{
    Relation    idxRel;
    ChecksumAccum acc32 = {0, 0};
    ChecksumAccum64 acc64 = {0, 0};
    uint64      index_checksum;
    uint64      index_type_hash;
    bool        is_brin;
    BlockNumber nblocks;
    int         workers;

    *is_null = false;

    /* Open index */
    idxRel = index_open(indexoid, AccessShareLock);

    /* Indexes derive their ACL from the underlying heap relation. */
    if (check_privileges)
        pg_integrity_checksums_require_select(indexoid);

    /*
     * A partitioned index has no storage of its own; aggregate the checksums
     * of its leaf indexes, as the table functions do for partitions.
     */
    if (idxRel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
    {
        bool        supported = index_supports_checksum(idxRel->rd_rel->relam);

        index_close(idxRel, AccessShareLock);

        if (!supported)
        {
            *is_null = true;
            return 0;
        }

        return pg_index_physical_partitioned_checksum(indexoid, algo, is_null);
    }

    /* Check index type support */
    if (!index_supports_checksum(idxRel->rd_rel->relam))
    {
        index_close(idxRel, AccessShareLock);
        *is_null = true;
        return 0;
    }

    is_brin = (idxRel->rd_rel->relam == BRIN_AM_OID);
    nblocks = RelationGetNumberOfBlocks(idxRel);
    workers = pg_integrity_checksums_worker_count_for(idxRel);
    if (nblocks < (BlockNumber) workers)
        workers = (int) nblocks;

    if (workers < 1)
    {
        BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

        scan_index_blocks_physical(idxRel, 0, nblocks, is_brin, algo,
                                   &acc32, &acc64, bstrategy);
        FreeAccessStrategy(bstrategy);
    }
    else
        pg_integrity_checksums_parallel_scan_blocks(CHECKSUM_TASK_INDEX_PHYSICAL,
                                                    indexoid, false, nblocks,
                                                    workers, algo, &acc32,
                                                    NULL, &acc64, NULL);

    index_checksum = physical_accum_finalize(algo, &acc32, &acc64);

    /* Generic indexes also fold in their access-method metadata. */
    if (!is_brin)
    {
        index_type_hash = hash_bytes_algo(0, &idxRel->rd_rel->relam,
                                          sizeof(Oid), algo);
        index_type_hash = combine_hash(index_type_hash,
                                       (uint64) idxRel->rd_rel->relnatts,
                                       algo);
        index_checksum = combine_hash(index_checksum, index_type_hash, algo);
    }

    /*
     * Index rewrites (REINDEX, VACUUM FULL, CLUSTER) assign a new
     * relfilenode.  Include it so a rebuilt index gets a different physical
     * checksum even when its pages happen to be byte-identical.
     */
    index_checksum = combine_hash(index_checksum,
                                  (uint64) pg_integrity_checksums_relfilenumber(idxRel),
                                  algo);
    index_checksum = combine_hash(index_checksum,
                                  (uint64) idxRel->rd_rel->reltablespace,
                                  algo);

    /* Close index */
    index_close(idxRel, AccessShareLock);

    return index_checksum;
}

/*
 * pg_index_physical_checksum_calc - physical index checksum for one algo
 */
uint64
pg_index_physical_checksum_calc(Oid indexoid, bool check_privileges,
                                ChecksumAlgo algo, bool *is_null)
{
    return pg_index_physical_checksum_common(indexoid, check_privileges,
                                             algo, is_null);
}

/*-------------------------------------------------------------------------
 * Logical Index Checksum Functions
 *-------------------------------------------------------------------------
 */

/*
 * pg_index_logical_checksum - SQL function for logical index checksums
 *
 * Public interface for computing logical index checksums.
 * Returns NULL for an invalid or unusable index.
 */
PG_FUNCTION_INFO_V1(pg_index_logical_checksum);

Datum
pg_index_logical_checksum(PG_FUNCTION_ARGS)
{
    Oid         indexoid;
    bool        is_null = false;
    uint32      index_checksum;

    indexoid = PG_GETARG_OID(0);

    index_checksum = (uint32) pg_index_logical_checksum_calc(indexoid, true,
                                                             CHECKSUM_ALGO_FNV32,
                                                             &is_null);

    if (is_null)
        PG_RETURN_NULL();

    PG_RETURN_INT32((int32) index_checksum);
}

/*
 * pg_index_logical_checksum_algo - generic logical index checksum
 */
PG_FUNCTION_INFO_V1(pg_index_logical_checksum_algo);

Datum
pg_index_logical_checksum_algo(PG_FUNCTION_ARGS)
{
    Oid         indexoid;
    ChecksumAlgo algorithm;
    bool        is_null = false;
    uint64      result;

    indexoid = PG_GETARG_OID(0);
    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(1));

    result = pg_index_logical_checksum_common(indexoid, true, algorithm,
                                              &is_null);
    if (is_null)
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(result,
                                     checksum_algo_len(algorithm)));
}

/*
 * Parallel safety of an index definition
 *
 * A logical index checksum evaluates the index expressions and the partial
 * index predicate.  In a parallel scan they run in the workers, and in the
 * leader while it is in parallel mode, so every function they call must be
 * PARALLEL SAFE, as for a parallel CREATE INDEX.  A user-defined function is
 * PARALLEL UNSAFE unless declared otherwise, and running one in parallel mode
 * fails (SET, sequence access, writes) or misbehaves.
 */
static bool
parallel_unsafe_function(Oid func_id, void *context)
{
    (void) context;
    return func_parallel(func_id) != PROPARALLEL_SAFE;
}

static bool
parallel_unsafe_walker(Node *node, void *context)
{
    if (node == NULL)
        return false;

    if (check_functions_in_node(node, parallel_unsafe_function, context))
        return true;

    return expression_tree_walker(node, parallel_unsafe_walker, context);
}

/*
 * pg_index_definition_parallel_safe - can a parallel scan evaluate this index?
 */
bool
pg_index_definition_parallel_safe(Relation idxRel)
{
    List       *exprs = RelationGetIndexExpressions(idxRel);
    List       *pred = RelationGetIndexPredicate(idxRel);
    bool        unsafe;

    unsafe = parallel_unsafe_walker((Node *) exprs, NULL) ||
        parallel_unsafe_walker((Node *) pred, NULL);

    list_free_deep(exprs);
    list_free_deep(pred);

    return !unsafe;
}

/*
 * pg_index_oid_definition_parallel_safe - the same, by OID
 *
 * A relation that is not an index, or that no longer exists, counts as safe:
 * its checksum does not evaluate an index definition, or fails on its own.
 */
bool
pg_index_oid_definition_parallel_safe(Oid indexoid)
{
    Relation    rel = try_relation_open(indexoid, AccessShareLock);
    bool        safe = true;

    if (rel == NULL)
        return true;

    if (rel->rd_rel->relkind == RELKIND_INDEX)
        safe = pg_index_definition_parallel_safe(rel);

    relation_close(rel, AccessShareLock);
    return safe;
}

/*
 * pg_index_logical_scan_init - prepare an executor-like scan of an index
 *
 * The index definition is evaluated on heap tuples.  The caller owns the
 * returned state and must call pg_index_logical_scan_end().
 */
bool
pg_index_logical_scan_init(Oid indexoid, Snapshot snapshot,
                           IndexLogicalScanState *scanstate)
{
    Oid         heapoid;
    MemoryContext oldcontext;

    MemSet(scanstate, 0, sizeof(*scanstate));
    scanstate->snapshot = snapshot;

    scanstate->idxRel = index_open(indexoid, AccessShareLock);

    if (scanstate->idxRel->rd_rel->relkind != RELKIND_INDEX ||
        !scanstate->idxRel->rd_index->indisvalid ||
        !scanstate->idxRel->rd_index->indisready ||
        !scanstate->idxRel->rd_index->indislive)
    {
        index_close(scanstate->idxRel, AccessShareLock);
        MemSet(scanstate, 0, sizeof(*scanstate));
        return false;
    }

    heapoid = IndexGetRelation(indexoid, false);
    scanstate->heapRel = relation_open(heapoid, AccessShareLock);

    if (scanstate->heapRel->rd_rel->relkind != RELKIND_RELATION &&
        scanstate->heapRel->rd_rel->relkind != RELKIND_MATVIEW &&
        scanstate->heapRel->rd_rel->relkind != RELKIND_TOASTVALUE)
    {
        relation_close(scanstate->heapRel, AccessShareLock);
        index_close(scanstate->idxRel, AccessShareLock);
        MemSet(scanstate, 0, sizeof(*scanstate));
        return false;
    }

    scanstate->context = AllocSetContextCreate(CurrentMemoryContext,
                                               "pg_integrity_checksums index logical",
                                               ALLOCSET_SMALL_SIZES);
    scanstate->bstrategy = GetAccessStrategy(BAS_BULKREAD);
    oldcontext = MemoryContextSwitchTo(scanstate->context);

    scanstate->indexInfo = BuildIndexInfo(scanstate->idxRel);
    scanstate->estate = CreateExecutorState();
    scanstate->econtext = GetPerTupleExprContext(scanstate->estate);
    scanstate->predicate = ExecPrepareQual(scanstate->indexInfo->ii_Predicate,
                                           scanstate->estate);
    scanstate->slot = MakeSingleTupleTableSlot(RelationGetDescr(scanstate->heapRel),
                                               &TTSOpsHeapTuple);
    scanstate->nattrs = scanstate->indexInfo->ii_NumIndexAttrs;
    scanstate->values = palloc(sizeof(Datum) * scanstate->nattrs);
    scanstate->nulls = palloc(sizeof(bool) * scanstate->nattrs);
    scanstate->inputtypid = palloc(sizeof(Oid) * scanstate->nattrs);
    scanstate->inputtypmod = palloc(sizeof(int32) * scanstate->nattrs);
    scanstate->hints = palloc0(sizeof(LogicalColumnHint) * scanstate->nattrs);

    {
        ListCell   *expr_item = list_head(scanstate->indexInfo->ii_Expressions);
        int         i;

        for (i = 0; i < scanstate->nattrs; i++)
        {
            int         keycol = scanstate->indexInfo->ii_IndexAttrNumbers[i];
            Form_pg_attribute hatt;
            Oid         typid;
            int32       typmod;

            if (keycol > 0)
            {
                hatt = TupleDescAttr(scanstate->heapRel->rd_att, keycol - 1);
                typid = hatt->atttypid;
                typmod = hatt->atttypmod;
            }
            else if (keycol == 0)
            {
                Node       *expr;

                if (expr_item == NULL)
                    elog(ERROR, "wrong number of index expressions");

                expr = (Node *) lfirst(expr_item);
                typid = exprType(expr);
                typmod = exprTypmod(expr);
                expr_item = lnext_compat(scanstate->indexInfo->ii_Expressions,
                                         expr_item);
            }
            else
            {
                Oid         sysrelid = RelationGetRelid(scanstate->heapRel);

                typid = get_atttype(sysrelid, keycol);
                typmod = -1;
                if (!OidIsValid(typid))
                {
                    hatt = TupleDescAttr(RelationGetDescr(scanstate->idxRel), i);
                    typid = hatt->atttypid;
                    typmod = hatt->atttypmod;
                }
            }

            scanstate->inputtypid[i] = typid;
            scanstate->inputtypmod[i] = typmod;
        }

        if (expr_item != NULL)
            elog(ERROR, "wrong number of index expressions");
    }

    MemoryContextSwitchTo(oldcontext);

    return true;
}

/*
 * IndexLogicalVisitorState - accumulator state for a logical index scan
 *
 * Only the accumulator that matches "algo" is used.
 */
typedef struct IndexLogicalVisitorState
{
    IndexLogicalScanState *scanstate;
    ChecksumAlgo algo;
    ChecksumLogicalAccum *acc32;
    ChecksumLogicalAccum64 *acc64;
} IndexLogicalVisitorState;

/*
 * pg_index_logical_row_hash - hash the index entry of one visible heap tuple
 *
 * Evaluates the index expressions and predicate on the heap tuple and hashes
 * the resulting index columns, so that the checksum covers the index
 * definition's view of the data rather than its physical layout.  Returns
 * false (and no hash) when a partial index predicate excludes the tuple.
 */
bool
pg_index_logical_row_hash(IndexLogicalScanState *scanstate, HeapTuple tuple,
                          ChecksumAlgo algo, uint64 *row_hash)
{
    MemoryContext oldcontext;
    uint64      h;
    int         i;

    ResetExprContext(scanstate->econtext);
    ExecStoreHeapTuple(tuple, scanstate->slot, false);
    scanstate->econtext->ecxt_scantuple = scanstate->slot;

    if (scanstate->predicate != NULL &&
        !ExecQual(scanstate->predicate, scanstate->econtext))
        return false;

    FormIndexDatum(scanstate->indexInfo, scanstate->slot, scanstate->estate,
                   scanstate->values, scanstate->nulls);

    h = checksum_algo_is32(algo) ? FNV_BASIS_32 : FNV_BASIS_64;

    /*
     * Hash in the per-tuple memory context, which the next row resets, so
     * detoasted values and send function results do not accumulate.
     */
    oldcontext = MemoryContextSwitchTo(scanstate->econtext->ecxt_per_tuple_memory);

    for (i = 0; i < scanstate->nattrs; i++)
    {
        uint64      col_hash;

        col_hash = pg_cell_checksum_canonical_hinted(scanstate->values[i],
                                                     scanstate->nulls[i],
                                                     scanstate->inputtypid[i],
                                                     scanstate->inputtypmod[i],
                                                     i + 1, algo,
                                                     &scanstate->hints[i]);
        h = combine_hash(h, col_hash, algo);
    }

    MemoryContextSwitchTo(oldcontext);

    *row_hash = h;
    return true;
}

/* index_logical_tuple_visitor - fold one heap tuple's index entry */
static void
index_logical_tuple_visitor(Relation rel, Page page, BlockNumber blkno,
                            OffsetNumber offnum, HeapTuple tuple, void *arg)
{
    IndexLogicalVisitorState *state = (IndexLogicalVisitorState *) arg;
    uint64      row_hash;

    if (!pg_index_logical_row_hash(state->scanstate, tuple, state->algo,
                                   &row_hash))
        return;

    logical_accum_add_algo(state->algo, state->acc32, state->acc64, row_hash);
}

/*
 * pg_index_logical_scan_blocks - hash index definitions over a heap
 * block range
 *
 * Only the accumulator that matches "algo" is used.
 */
void
pg_index_logical_scan_blocks(IndexLogicalScanState *scanstate,
                                  BlockNumber start, BlockNumber end,
                                  ChecksumAlgo algo,
                                  ChecksumLogicalAccum *acc32,
                                  ChecksumLogicalAccum64 *acc64)
{
    IndexLogicalVisitorState state = {scanstate, algo, acc32, acc64};

    scan_visible_tuples(scanstate->heapRel, start, end, scanstate->snapshot,
                        scanstate->bstrategy, false,
                        index_logical_tuple_visitor,
                        &state);
}

/*
 * pg_index_logical_scan_end - release an index logical scan state
 */
void
pg_index_logical_scan_end(IndexLogicalScanState *scanstate)
{
    if (scanstate->slot != NULL)
        ExecDropSingleTupleTableSlot(scanstate->slot);
    if (scanstate->estate != NULL)
        FreeExecutorState(scanstate->estate);
    if (scanstate->heapRel != NULL)
        relation_close(scanstate->heapRel, AccessShareLock);
    if (scanstate->idxRel != NULL)
        index_close(scanstate->idxRel, AccessShareLock);
    if (scanstate->bstrategy != NULL)
        FreeAccessStrategy(scanstate->bstrategy);
    if (scanstate->context != NULL)
        MemoryContextDelete(scanstate->context);

    MemSet(scanstate, 0, sizeof(*scanstate));
}

/*
 * pg_index_logical_checksum_accum - heap-derived logical index content
 *
 * A logical index checksum describes the index entries that the table data
 * implies, not the physical index pages.  We evaluate the index definition
 * (plain columns, expressions, partial predicate and INCLUDE columns) on each
 * visible heap row.  This is independent of the access method, so it works
 * for B-tree, Hash, GiST, GIN, SP-GiST, BRIN and custom AMs, and it does not
 * contain heap TIDs, relation OIDs or type OIDs.
 *
 * A physical index checksum remains the tool for detecting missing/corrupt
 * index entries; this logical checksum is a fingerprint of the expected
 * logical index content.
 *
 * Only the accumulator that matches "algo" is used.
 */
static void
pg_index_logical_checksum_accum(Oid indexoid, bool check_privileges,
                                ChecksumAlgo algo,
                                ChecksumLogicalAccum *acc32,
                                ChecksumLogicalAccum64 *acc64,
                                bool *is_null)
{
    Relation    idxRel;
    Snapshot    snapshot;
    IndexLogicalScanState scanstate;
    BlockNumber nblocks;
    int         workers;
    ChecksumOwnerContext owner_ctx;

    *is_null = false;

    idxRel = index_open(indexoid, AccessShareLock);

    if (check_privileges)
        pg_integrity_checksums_require_select(indexoid);

    if (idxRel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
    {
        List       *parts = pg_integrity_checksums_leaf_indexes(indexoid);
        ListCell   *lc;
        bool        any_null = false;

        foreach(lc, parts)
        {
            Oid         partoid = lfirst_oid(lc);
            bool        part_null = false;
            uint64      part_checksum;

            part_checksum = pg_index_logical_checksum_common(partoid, false,
                                                             algo, &part_null);
            if (part_null)
                any_null = true;
            else
                logical_accum_add_algo(algo, acc32, acc64, part_checksum);
        }

        list_free(parts);
        index_close(idxRel, AccessShareLock);

        if (any_null)
            *is_null = true;
        return;
    }

    index_close(idxRel, AccessShareLock);

    snapshot = pg_integrity_checksums_get_snapshot();

    /*
     * The index definition is user code: evaluate it as the table owner.
     * Parallel workers inherit the switched identity.
     */
    pg_integrity_checksums_switch_to_owner(IndexGetRelation(indexoid, false),
                                           &owner_ctx);

    if (!pg_index_logical_scan_init(indexoid, snapshot, &scanstate))
    {
        pg_integrity_checksums_restore_user(&owner_ctx);
        *is_null = true;
        return;
    }

    nblocks = RelationGetNumberOfBlocks(scanstate.heapRel);
    workers = pg_integrity_checksums_worker_count_for(scanstate.heapRel);
    if (nblocks < (BlockNumber) workers)
        workers = (int) nblocks;
    if (workers > 0 && !pg_index_definition_parallel_safe(scanstate.idxRel))
        workers = 0;

    if (workers < 1)
    {
        pg_index_logical_scan_blocks(&scanstate, 0, nblocks, algo,
                                          acc32, acc64);
        pg_index_logical_scan_end(&scanstate);
    }
    else
    {
        pg_index_logical_scan_end(&scanstate);
        pg_integrity_checksums_parallel_scan_blocks(CHECKSUM_TASK_INDEX_LOGICAL,
                                                    indexoid, false, nblocks,
                                                    workers, algo, NULL, acc32,
                                                    NULL, acc64);
    }

    pg_integrity_checksums_restore_user(&owner_ctx);
}

/*
 * pg_index_logical_checksum_common - logical index checksum for one algo
 */
static uint64
pg_index_logical_checksum_common(Oid indexoid, bool check_privileges,
                                 ChecksumAlgo algo, bool *is_null)
{
    ChecksumLogicalAccum acc32 = {0, 0};
    ChecksumLogicalAccum64 acc64 = {0, 0};

    pg_index_logical_checksum_accum(indexoid, check_privileges, algo,
                                    &acc32, &acc64, is_null);

    if (*is_null)
        return 0;

    if (checksum_algo_is32(algo))
        return logical_accum_finalize(&acc32);

    return logical_accum_finalize64(&acc64);
}

/*
 * pg_index_logical_checksum_calc - logical index checksum for one algo
 */
uint64
pg_index_logical_checksum_calc(Oid indexoid, bool check_privileges,
                               ChecksumAlgo algo, bool *is_null)
{
    return pg_index_logical_checksum_common(indexoid, check_privileges, algo,
                                            is_null);
}
