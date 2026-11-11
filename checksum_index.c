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
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
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
#include "utils/typcache.h"

#include "checksum_internal.h"

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
 * Supports all major index types. Returns NULL for unsupported index types
 * and for partitioned indexes.  Empty indexes are still distinguished by
 * their relation and storage identity.
 */
PG_FUNCTION_INFO_V1(pg_index_physical_checksum);

Datum
pg_index_physical_checksum(PG_FUNCTION_ARGS)
{
    Oid         indexoid;
    bool        is_null = false;
    uint32      index_checksum;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    indexoid = PG_GETARG_OID(0);

    index_checksum = pg_index_physical_checksum_calc(indexoid, true, &is_null);

    if (is_null)
        PG_RETURN_NULL();

    PG_RETURN_INT32((int32) index_checksum);
}

/*
 * pg_index_physical_partitioned_checksum - aggregate leaf index checksums
 */
static uint32
pg_index_physical_partitioned_checksum(Oid indexoid, bool *is_null)
{
    List       *parts = pg_integrity_checksums_leaf_indexes(indexoid);
    ChecksumAccum acc = {0, 0};
    ListCell   *lc;

    *is_null = false;

    foreach(lc, parts)
    {
        Oid         partoid = lfirst_oid(lc);
        bool        part_null = false;
        uint32      part_checksum;

        part_checksum = pg_index_physical_checksum_calc(partoid, false,
                                                        &part_null);
        if (part_null)
            *is_null = true;
        else
            checksum_accum_add(&acc,
                               combine_checksums(part_checksum,
                                                 (uint32) partoid));
    }

    list_free(parts);

    if (*is_null)
        return 0;

    return combine_checksums(checksum_accum_finalize(&acc),
                             (uint32) indexoid);
}

/*
 * pg_index_physical_checksum_calc - implementation shared with database scan
 *
 * Sets *is_null for unsupported access methods.
 */
uint32
pg_index_physical_checksum_calc(Oid indexoid, bool check_privileges,
                                bool *is_null)
{
    Relation    idxRel;
    ChecksumAccum acc = {0, 0};
    uint32      index_checksum = 0;
    uint32      index_type_hash;
    bool        is_brin;
    uint32      nblocks;
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

        return pg_index_physical_partitioned_checksum(indexoid, is_null);
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
    workers = pg_integrity_checksums_worker_count();
    if (nblocks < (uint32) workers)
        workers = (int) nblocks;

    if (workers < 1)
    {
        BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

        scan_index_blocks_physical(idxRel, 0, nblocks, is_brin, &acc,
                                   bstrategy);
        FreeAccessStrategy(bstrategy);
    }
    else
        pg_integrity_checksums_parallel_scan_blocks(CHECKSUM_TASK_INDEX_PHYSICAL,
                                          indexoid, false, nblocks, workers,
                                          0, &acc, NULL, NULL, NULL);

    index_checksum = checksum_accum_finalize(&acc);

    /* Generic indexes also fold in their access-method metadata. */
    if (!is_brin)
    {
        index_type_hash = fnv1a_32_hash(&idxRel->rd_rel->relam, sizeof(Oid), FNV_BASIS_32);
        index_type_hash = combine_checksums(index_type_hash,
                                            (uint32) idxRel->rd_rel->relnatts);
        index_checksum = combine_checksums(index_checksum, index_type_hash);
    }

    /*
     * Index rewrites (REINDEX, VACUUM FULL, CLUSTER) assign a new
     * relfilenode.  Include it so a rebuilt index gets a different physical
     * checksum even when its pages happen to be byte-identical.
     */
    index_checksum = combine_checksums(index_checksum,
                                       (uint32) idxRel->rd_rel->relfilenode);
    index_checksum = combine_checksums(index_checksum,
                                       (uint32) idxRel->rd_rel->reltablespace);

    /* Close index */
    index_close(idxRel, AccessShareLock);

    return index_checksum;
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

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    indexoid = PG_GETARG_OID(0);

    index_checksum = pg_index_logical_checksum_calc(indexoid, true, &is_null);

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
    int         algorithm;
    bool        is_null = false;
    uint64      result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    indexoid = PG_GETARG_OID(0);
    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(1));

    if (algorithm == 0)
    {
        uint32      legacy = pg_index_logical_checksum_calc(indexoid, true,
                                                            &is_null);

        if (is_null)
            PG_RETURN_NULL();
        PG_RETURN_BYTEA_P(checksum_bytea(legacy, 4));
    }

    result = pg_index_logical_checksum_calc64(indexoid, true, &is_null);
    if (is_null)
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(result, 8));
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
 * pg_index_logical_scan_blocks - hash index definitions over a heap block range
 */
void
pg_index_logical_scan_blocks(IndexLogicalScanState *scanstate,
                             BlockNumber start, BlockNumber end,
                             ChecksumLogicalAccum *acc)
{
    BlockNumber blkno;

    for (blkno = start; blkno < end; blkno++)
    {
        Buffer      buffer;
        Page        page;
        OffsetNumber maxoff;
        OffsetNumber offnum;

        buffer = ReadBufferExtended(scanstate->heapRel, MAIN_FORKNUM, blkno,
                                    RBM_NORMAL, scanstate->bstrategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);

        if (PageIsNew(page))
        {
            UnlockReleaseBuffer(buffer);
            if ((blkno & 63) == 0)
                CHECK_FOR_INTERRUPTS();
            continue;
        }

        pg_integrity_checksums_check_page(scanstate->heapRel, blkno, page);

        maxoff = PageGetMaxOffsetNumber(page);
        for (offnum = FirstOffsetNumber; offnum <= maxoff;
             offnum = OffsetNumberNext(offnum))
        {
            ItemId      lp = PageGetItemId(page, offnum);
            HeapTupleHeader htup;
            HeapTupleData tuple;
            Size        tuple_len;

            if (!ItemIdIsNormal(lp))
                continue;

            if (!pg_integrity_checksums_item_is_sane(page, lp))
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("corrupt line pointer in relation \"%s\" at block %u, offset %u",
                                RelationGetRelationName(scanstate->heapRel),
                                blkno, offnum)));

            htup = (HeapTupleHeader) PageGetItem(page, lp);
            tuple_len = ItemIdGetLength(lp);

            pg_integrity_checksums_check_heap_tuple(scanstate->heapRel, blkno, offnum,
                                          htup, tuple_len);

            tuple.t_len = tuple_len;
            tuple.t_data = htup;
            tuple.t_tableOid = RelationGetRelid(scanstate->heapRel);
            ItemPointerSet(&tuple.t_self, blkno, offnum);

            if (!HeapTupleSatisfiesVisibility(&tuple, scanstate->snapshot,
                                              buffer))
                continue;

            ResetExprContext(scanstate->econtext);
            ExecStoreHeapTuple(&tuple, scanstate->slot, false);
            scanstate->econtext->ecxt_scantuple = scanstate->slot;

            if (scanstate->predicate != NULL &&
                !ExecQual(scanstate->predicate, scanstate->econtext))
                continue;

            FormIndexDatum(scanstate->indexInfo, scanstate->slot,
                           scanstate->estate, scanstate->values,
                           scanstate->nulls);

            {
                uint32      row_hash = FNV_BASIS_32;
                int         i;

                for (i = 0; i < scanstate->nattrs; i++)
                {
                    uint32      col_hash;

                    col_hash = pg_cell_checksum_canonical(scanstate->values[i],
                                                          scanstate->nulls[i],
                                                          scanstate->inputtypid[i],
                                                          scanstate->inputtypmod[i],
                                                          i + 1);
                    row_hash = combine_checksums(row_hash, col_hash);
                }

                logical_accum_add(acc, row_hash);
            }
        }

        UnlockReleaseBuffer(buffer);

        if ((blkno & 63) == 0)
            CHECK_FOR_INTERRUPTS();
    }

}

void
pg_index_logical_scan_blocks64(IndexLogicalScanState *scanstate,
                             BlockNumber start, BlockNumber end,
                             ChecksumLogicalAccum64 *acc)
{
    BlockNumber blkno;

    for (blkno = start; blkno < end; blkno++)
    {
        Buffer      buffer;
        Page        page;
        OffsetNumber maxoff;
        OffsetNumber offnum;

        buffer = ReadBufferExtended(scanstate->heapRel, MAIN_FORKNUM, blkno,
                                    RBM_NORMAL, scanstate->bstrategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);

        if (PageIsNew(page))
        {
            UnlockReleaseBuffer(buffer);
            if ((blkno & 63) == 0)
                CHECK_FOR_INTERRUPTS();
            continue;
        }

        pg_integrity_checksums_check_page(scanstate->heapRel, blkno, page);

        maxoff = PageGetMaxOffsetNumber(page);
        for (offnum = FirstOffsetNumber; offnum <= maxoff;
             offnum = OffsetNumberNext(offnum))
        {
            ItemId      lp = PageGetItemId(page, offnum);
            HeapTupleHeader htup;
            HeapTupleData tuple;
            Size        tuple_len;

            if (!ItemIdIsNormal(lp))
                continue;

            if (!pg_integrity_checksums_item_is_sane(page, lp))
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("corrupt line pointer in relation \"%s\" at block %u, offset %u",
                                RelationGetRelationName(scanstate->heapRel),
                                blkno, offnum)));

            htup = (HeapTupleHeader) PageGetItem(page, lp);
            tuple_len = ItemIdGetLength(lp);

            pg_integrity_checksums_check_heap_tuple(scanstate->heapRel, blkno, offnum,
                                          htup, tuple_len);

            tuple.t_len = tuple_len;
            tuple.t_data = htup;
            tuple.t_tableOid = RelationGetRelid(scanstate->heapRel);
            ItemPointerSet(&tuple.t_self, blkno, offnum);

            if (!HeapTupleSatisfiesVisibility(&tuple, scanstate->snapshot,
                                              buffer))
                continue;

            ResetExprContext(scanstate->econtext);
            ExecStoreHeapTuple(&tuple, scanstate->slot, false);
            scanstate->econtext->ecxt_scantuple = scanstate->slot;

            if (scanstate->predicate != NULL &&
                !ExecQual(scanstate->predicate, scanstate->econtext))
                continue;

            FormIndexDatum(scanstate->indexInfo, scanstate->slot,
                           scanstate->estate, scanstate->values,
                           scanstate->nulls);

            {
                uint64      row_hash = FNV_BASIS_64;
                int         i;

                for (i = 0; i < scanstate->nattrs; i++)
                {
                    uint64      col_hash;

                    col_hash = pg_cell_checksum_canonical_algo(scanstate->values[i],
                                                          scanstate->nulls[i],
                                                          scanstate->inputtypid[i],
                                                          scanstate->inputtypmod[i],
                                                          i + 1, 1);
                    row_hash = combine_logical(row_hash, col_hash, 1);
                }

                logical_accum_add64(acc, row_hash);
            }
        }

        UnlockReleaseBuffer(buffer);

        if ((blkno & 63) == 0)
            CHECK_FOR_INTERRUPTS();
    }

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
 */
static void
pg_index_logical_checksum_accum(Oid indexoid, bool check_privileges,
                                ChecksumLogicalAccum *acc, bool *is_null)
{
    Relation    idxRel;
    Snapshot    snapshot;
    IndexLogicalScanState scanstate;
    BlockNumber nblocks;
    int         workers;

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
            uint32      part_checksum;

            part_checksum = pg_index_logical_checksum_calc(partoid, false,
                                                           &part_null);
            if (part_null)
                any_null = true;
            else
                logical_accum_add(acc, part_checksum);
        }

        list_free(parts);
        index_close(idxRel, AccessShareLock);

        if (any_null)
            *is_null = true;
        return;
    }

    index_close(idxRel, AccessShareLock);

    snapshot = pg_integrity_checksums_get_snapshot();

    if (!pg_index_logical_scan_init(indexoid, snapshot, &scanstate))
    {
        *is_null = true;
        return;
    }

    nblocks = RelationGetNumberOfBlocks(scanstate.heapRel);
    workers = pg_integrity_checksums_worker_count();
    if (nblocks < (BlockNumber) workers)
        workers = (int) nblocks;

    if (workers < 1)
    {
        pg_index_logical_scan_blocks(&scanstate, 0, nblocks, acc);
        pg_index_logical_scan_end(&scanstate);
    }
    else
    {
        pg_index_logical_scan_end(&scanstate);
        pg_integrity_checksums_parallel_scan_blocks(CHECKSUM_TASK_INDEX_LOGICAL,
                                          indexoid, false, nblocks, workers,
                                          0, NULL, acc, NULL, NULL);
    }
}

/*
 * pg_index_logical_checksum_calc - implementation shared with database scan
 */
uint32
pg_index_logical_checksum_calc(Oid indexoid, bool check_privileges,
                               bool *is_null)
{
    ChecksumLogicalAccum acc = {0, 0};

    pg_index_logical_checksum_accum(indexoid, check_privileges, &acc,
                                    is_null);

    if (*is_null)
        return 0;

    return logical_accum_finalize(&acc);
}

/*
 * pg_index_logical_checksum_accum64 - 64-bit heap-derived index content
 */
static void
pg_index_logical_checksum_accum64(Oid indexoid, bool check_privileges,
                                  ChecksumLogicalAccum64 *acc, bool *is_null)
{
    Relation    idxRel;
    Snapshot    snapshot;
    IndexLogicalScanState scanstate;
    BlockNumber nblocks;
    int         workers;

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

            part_checksum = pg_index_logical_checksum_calc64(partoid, false,
                                                             &part_null);
            if (part_null)
                any_null = true;
            else
                logical_accum_add64(acc, part_checksum);
        }

        list_free(parts);
        index_close(idxRel, AccessShareLock);

        if (any_null)
            *is_null = true;
        return;
    }

    index_close(idxRel, AccessShareLock);

    snapshot = pg_integrity_checksums_get_snapshot();

    if (!pg_index_logical_scan_init(indexoid, snapshot, &scanstate))
    {
        *is_null = true;
        return;
    }

    nblocks = RelationGetNumberOfBlocks(scanstate.heapRel);
    workers = pg_integrity_checksums_worker_count();
    if (IsInParallelMode())
        workers = 0;
    if (workers < 1)
    {
        pg_index_logical_scan_blocks64(&scanstate, 0, nblocks, acc);
        pg_index_logical_scan_end(&scanstate);
    }
    else
    {
        pg_index_logical_scan_end(&scanstate);
        pg_integrity_checksums_parallel_scan_blocks(CHECKSUM_TASK_INDEX_LOGICAL,
                                                    indexoid, false, nblocks,
                                                    workers, 1, NULL, NULL,
                                                    NULL, acc);
    }
}

/*
 * pg_index_logical_checksum_calc64
 */
uint64
pg_index_logical_checksum_calc64(Oid indexoid, bool check_privileges,
                                 bool *is_null)
{
    ChecksumLogicalAccum64 acc = {0, 0};

    pg_index_logical_checksum_accum64(indexoid, check_privileges, &acc,
                                      is_null);

    if (*is_null)
        return 0;

    return logical_accum_finalize64(&acc);
}


/*
 * pg_index_physical_checksum_calc64 - serial 64-bit physical index checksum
 */
uint64
pg_index_physical_checksum_calc64(Oid indexoid, bool check_privileges,
                                  bool *is_null)
{
    Relation    idxRel;
    ChecksumAccum64 acc = {0, 0};
    uint64      index_checksum;
    uint64      index_type_hash;
    bool        is_brin;
    uint32      nblocks;
    int         workers;

    *is_null = false;
    idxRel = index_open(indexoid, AccessShareLock);

    if (check_privileges)
        pg_integrity_checksums_require_select(indexoid);

    if (idxRel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
    {
        List       *parts = pg_integrity_checksums_leaf_indexes(indexoid);
        ChecksumAccum64 pacc = {0, 0};
        ListCell   *lc;
        bool        supported = index_supports_checksum(idxRel->rd_rel->relam);

        index_close(idxRel, AccessShareLock);
        if (!supported)
        {
            *is_null = true;
            return 0;
        }

        foreach(lc, parts)
        {
            Oid         partoid = lfirst_oid(lc);
            bool        part_null = false;
            uint64      part_checksum;

            part_checksum = pg_index_physical_checksum_calc64(partoid, false,
                                                              &part_null);
            if (part_null)
                *is_null = true;
            else
                checksum_accum64_add(&pacc,
                                     combine_checksums_64(part_checksum,
                                                          (uint64) partoid));
        }
        list_free(parts);
        if (*is_null)
            return 0;
        return combine_checksums_64(checksum_accum64_finalize(&pacc),
                                    (uint64) indexoid);
    }

    if (!index_supports_checksum(idxRel->rd_rel->relam))
    {
        index_close(idxRel, AccessShareLock);
        *is_null = true;
        return 0;
    }

    is_brin = (idxRel->rd_rel->relam == BRIN_AM_OID);
    nblocks = RelationGetNumberOfBlocks(idxRel);
    workers = pg_integrity_checksums_worker_count();
    if (IsInParallelMode())
        workers = 0;
    if (workers < 1)
    {
        BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

        scan_index_blocks_physical64(idxRel, 0, nblocks, is_brin, &acc,
                                     bstrategy);
        FreeAccessStrategy(bstrategy);
    }
    else
        pg_integrity_checksums_parallel_scan_blocks(CHECKSUM_TASK_INDEX_PHYSICAL,
                                                    indexoid, false, nblocks,
                                                    workers, 1, NULL, NULL,
                                                    &acc, NULL);

    index_checksum = checksum_accum64_finalize(&acc);

    if (!is_brin)
    {
        index_type_hash = fnv1a_64_hash(&idxRel->rd_rel->relam, sizeof(Oid),
                                        FNV_BASIS_64);
        index_type_hash = combine_checksums_64(index_type_hash,
                                               (uint64) idxRel->rd_rel->relnatts);
        index_checksum = combine_checksums_64(index_checksum, index_type_hash);
    }

    index_checksum = combine_checksums_64(index_checksum,
                                          (uint64) idxRel->rd_rel->relfilenode);
    index_checksum = combine_checksums_64(index_checksum,
                                          (uint64) idxRel->rd_rel->reltablespace);

    index_close(idxRel, AccessShareLock);
    return index_checksum;
}

