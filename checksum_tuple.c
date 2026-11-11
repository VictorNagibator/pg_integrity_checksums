/*-------------------------------------------------------------------------
 *
 * checksum_tuple.c
 *    Physical and canonical logical tuple checksums
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "checksum_internal.h"

static uint64 combine_physical(uint64 current, uint64 new_val, int algo);

/*-------------------------------------------------------------------------
 * TUPLE LEVEL FUNCTIONS (Physical and Logical)
 *-------------------------------------------------------------------------
 */

/*
 * pg_tuple_physical_checksum_internal - Core physical tuple checksum
 *
 * Computes a physical checksum for a tuple that depends on its physical
 * location (block number and offset). This checksum will change if the
 * tuple moves (e.g., after VACUUM FULL, CLUSTER).
 *
 * The checksum includes:
 * 1. Physical location (blkno << 16 | offnum) as seed
 * 2. Tuple data (with or without header as specified)
 * 3. MVCC information (xmin/xmax) when header is excluded
 *
 * This is useful for detecting physical corruption and verifying that
 * tuples haven't moved unexpectedly.
 */
uint64
pg_tuple_physical_checksum_internal(Relation rel, Page page,
                                   OffsetNumber offnum,
                                   BlockNumber blkno, bool include_header,
                                   bool validate, int algo)
{
    ItemId      lp;
    HeapTupleHeader tuple;
    char       *data;
    Size        len;
    uint64      checksum;
    uint64      location;
    uint64      location_hash;
    uint64      page_info;
    uint64      mvcc_info;
    uint64      itemid_info;

    if (PageIsNew(page))
        return 0;

    if (validate)
    {
        pg_integrity_checksums_check_page(rel, blkno, page);

        /* Validate offset number range */
        if (offnum < FirstOffsetNumber ||
            offnum > PageGetMaxOffsetNumber(page))
            return 0;

        lp = PageGetItemId(page, offnum);

        /* Only normal heap tuples carry data */
        if (!ItemIdIsNormal(lp))
            return 0;

        if (!pg_integrity_checksums_item_is_sane(page, lp))
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("corrupt line pointer in relation \"%s\" at block %u, offset %u",
                            RelationGetRelationName(rel), blkno, offnum)));

        tuple = (HeapTupleHeader) PageGetItem(page, lp);
        len = ItemIdGetLength(lp);

        pg_integrity_checksums_check_heap_tuple(rel, blkno, offnum, tuple, len);
    }
    else
    {
        /* The caller already validated the page, line pointer and tuple. */
        lp = PageGetItemId(page, offnum);
        tuple = (HeapTupleHeader) PageGetItem(page, lp);
        len = ItemIdGetLength(lp);
    }

    if (include_header)
    {
        /* Include entire tuple (header + data) in checksum */
        data = (char *) tuple;
    }
    else
    {
        /* Skip header, checksum only the tuple data */
        data = (char *) tuple + tuple->t_hoff;
        len -= tuple->t_hoff;
    }

    /* Create 64-bit location from block number and offset */
    location = ((uint64)blkno << 32) | (uint64)offnum;
    
    /* Hash the location with the selected algorithm. */
    location_hash = (algo == 0) ?
        fnv1a_32_hash(&location, sizeof(location), FNV_BASIS_32) :
        fnv1a_64_hash(&location, sizeof(location), FNV_BASIS_64);
    
    /*
     * Include relation identity and storage identity.  relfilenode changes
     * when VACUUM FULL, CLUSTER or ALTER TABLE SET TABLESPACE rewrite the
     * relation, so a tuple that happens to land at the same (block, offset)
     * still has a different physical checksum.  The relation OID keeps
     * checksums from different relations distinct.
     */
    location_hash = combine_physical(location_hash,
                                     (uint64) RelationGetRelid(rel), algo);
    location_hash = combine_physical(location_hash,
                                     (uint64) rel->rd_rel->relfilenode, algo);
    location_hash = combine_physical(location_hash,
                                     (uint64) rel->rd_rel->reltablespace, algo);

    /*
     * Include the page size/layout version for additional uniqueness.  Do
     * not include the page LSN, pd_checksum or pd_flags: they change when a
     * concurrent scan sets hint bits, which made a physical checksum depend
     * on who happened to read the page first.
     */
    page_info = PageGetPageSize(page) | (PageGetPageLayoutVersion(page) << 16);
    location_hash = combine_physical(location_hash, page_info, algo);

    /* Calculate checksum using location_hash as the initial value */
    if (algo == 0)
        checksum = pg_checksum_data_custom(data, len, (uint32) location_hash);
    else
        checksum = fnv1a_64_hash(data, len, location_hash);
    
    /* Incorporate MVCC information and other physical metadata */
    mvcc_info = (HeapTupleHeaderGetRawXmin(tuple) ^ 
                HeapTupleHeaderGetRawXmax(tuple) ^
                HeapTupleHeaderGetRawCommandId(tuple));
    checksum ^= mvcc_info;
    
    /* Include ItemId information */
    itemid_info = (ItemIdGetFlags(lp) << 24) | ItemIdGetLength(lp);
    checksum = combine_physical(checksum, itemid_info, algo);
    
    if (algo == 0)
    {
        if (checksum == CHECKSUM_NULL)
            checksum = (CHECKSUM_NULL ^ location_hash) & 0xFFFFFFFE;
        if (checksum == 0)
            checksum = (location_hash ^ mvcc_info ^ FNV_BASIS_32) & 0xFFFFFFFE;
    }
    else
    {
        if (checksum == CHECKSUM_NULL_64)
            checksum = (CHECKSUM_NULL_64 ^ location_hash) & ~UINT64CONST(1);
        if (checksum == 0)
            checksum = (location_hash ^ mvcc_info ^ FNV_BASIS_64) & ~UINT64CONST(1);
    }
    
    return checksum;
}

/*
 * combine_physical - combine physical metadata with the selected algorithm
 */
static uint64
combine_physical(uint64 current, uint64 new_val, int algo)
{
    if (algo == 0)
        return combine_checksums((uint32) current, (uint32) new_val);
    return combine_checksums_64(current, new_val);
}

/*
 * pg_tuple_physical_checksum - SQL function for physical tuple checksums
 *
 * Public interface for computing physical tuple checksums.
 * Uses exception handling to ensure resources are cleaned up
 * even if an error occurs.
 */
/*
 * pg_tuple_physical_checksum_common - shared physical tuple implementation
 */
static bool
pg_tuple_physical_checksum_common(Oid reloid, ItemPointer tid,
                                  bool include_header, int algo, uint64 *out)
{
    Relation    rel;
    volatile Buffer buffer = InvalidBuffer;
    Page        page;
    volatile uint64 checksum = 0;
    volatile bool lock_held = false;
    BlockNumber blkno;
    OffsetNumber offnum;

    if (!ItemPointerIsValid(tid))
        return false;

    rel = relation_open(reloid, AccessShareLock);
    pg_integrity_checksums_require_select(reloid);
    pg_integrity_checksums_check_relation_kind(rel, false, false);

    blkno = ItemPointerGetBlockNumber(tid);
    offnum = ItemPointerGetOffsetNumber(tid);

    if (blkno >= RelationGetNumberOfBlocks(rel))
    {
        relation_close(rel, AccessShareLock);
        return false;
    }

    PG_TRY();
    {
        buffer = ReadBuffer(rel, blkno);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        lock_held = true;
        page = BufferGetPage(buffer);

        checksum = pg_tuple_physical_checksum_internal(rel, page, offnum,
                                                      blkno, include_header,
                                                      true, algo);
    }
    PG_CATCH();
    {
        if (lock_held && BufferIsValid(buffer))
            UnlockReleaseBuffer(buffer);
        relation_close(rel, AccessShareLock);
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (lock_held && BufferIsValid(buffer))
        UnlockReleaseBuffer(buffer);
    relation_close(rel, AccessShareLock);

    *out = checksum;
    return true;
}

PG_FUNCTION_INFO_V1(pg_tuple_physical_checksum);

Datum
pg_tuple_physical_checksum(PG_FUNCTION_ARGS)
{
    uint64      checksum;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    if (!pg_tuple_physical_checksum_common(PG_GETARG_OID(0),
                                           PG_GETARG_ITEMPOINTER(1),
                                           PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2),
                                           0, &checksum))
        PG_RETURN_NULL();

    PG_RETURN_INT32((int32) checksum);
}

PG_FUNCTION_INFO_V1(pg_tuple_physical_checksum_algo);

Datum
pg_tuple_physical_checksum_algo(PG_FUNCTION_ARGS)
{
    int         algorithm;
    uint64      checksum;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(3))
        PG_RETURN_NULL();

    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(3));

    if (!pg_tuple_physical_checksum_common(PG_GETARG_OID(0),
                                           PG_GETARG_ITEMPOINTER(1),
                                           PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2),
                                           algorithm, &checksum))
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(checksum, algorithm == 0 ? 4 : 8));
}

/*-------------------------------------------------------------------------
 * Logical Tuple Checksum
 *-------------------------------------------------------------------------
 */

/*
 * pg_tuple_logical_checksum_internal - Core logical tuple checksum
 *
 * Computes a canonical logical checksum for a tuple.  All non-dropped
 * columns are hashed; relation OIDs, heap TIDs and the tuple header are not
 * included.
 *
 * This checksum is stable across:
 * - VACUUM FULL
 * - CLUSTER
 * - REINDEX
 * - Physical data movement
 * - Storage reorganization
 */
uint64
pg_tuple_logical_checksum_internal(Relation rel, HeapTuple tuple, int algo)
{
    TupleDesc   tupdesc = RelationGetDescr(rel);
    uint64      checksum = algo == 0 ? FNV_BASIS_32 : FNV_BASIS_64;
    int         i;

    for (i = 1; i <= tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);
        Datum       value;
        bool        isnull;
        uint64      col_hash;

        if (attr->attisdropped)
            continue;

        value = heap_getattr(tuple, i, tupdesc, &isnull);

        col_hash = pg_cell_checksum_canonical_algo(value, isnull,
                                                   attr->atttypid,
                                                   attr->atttypmod,
                                                   i, algo);

        checksum = combine_logical(checksum, col_hash, algo);
    }

    if (algo == 0)
    {
        if (checksum == CHECKSUM_NULL || checksum == 0)
            checksum = hash_mix32((uint32) checksum ^ FNV_BASIS_32);
    }
    else if (checksum == CHECKSUM_NULL_64 || checksum == 0)
        checksum = hash_mix64(checksum ^ FNV_BASIS_64);

    return checksum;
}

/*
 * pg_tuple_logical_checksum - SQL function for logical tuple checksums
 *
 * Public interface for computing logical tuple checksums.
 * Returns NULL for a missing tuple or a TOAST relation.
 */
/*
 * pg_tuple_logical_checksum_common - shared implementations for tuple checksums
 */
static bool
pg_tuple_logical_checksum_common(Oid reloid, ItemPointer tid, int algo,
                                 uint64 *out)
{
    Relation    rel;
    volatile Buffer buffer = InvalidBuffer;
    Page        page;
    ItemId      lp;
    HeapTupleData heapTuple;
    HeapTupleHeader tuple;
    volatile uint64 checksum = 0;
    volatile bool lock_held = false;
    volatile bool missing = false;
    BlockNumber blkno;
    OffsetNumber offnum;
    Size        tuple_len;

    if (!ItemPointerIsValid(tid))
        return false;

    rel = relation_open(reloid, AccessShareLock);
    pg_integrity_checksums_require_select(reloid);
    pg_integrity_checksums_check_relation_kind(rel, true, false);

    if (rel->rd_rel->relkind == RELKIND_TOASTVALUE)
    {
        relation_close(rel, AccessShareLock);
        return false;
    }

    blkno = ItemPointerGetBlockNumber(tid);
    offnum = ItemPointerGetOffsetNumber(tid);

    if (blkno >= RelationGetNumberOfBlocks(rel))
    {
        relation_close(rel, AccessShareLock);
        return false;
    }

    PG_TRY();
    {
        buffer = ReadBuffer(rel, blkno);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        lock_held = true;
        page = BufferGetPage(buffer);

        if (PageIsNew(page))
            missing = true;
        else
        {
            pg_integrity_checksums_check_page(rel, blkno, page);

            if (offnum < FirstOffsetNumber ||
                offnum > PageGetMaxOffsetNumber(page))
                missing = true;
            else
            {
                lp = PageGetItemId(page, offnum);

                if (!ItemIdIsNormal(lp))
                    missing = true;
                else if (!pg_integrity_checksums_item_is_sane(page, lp))
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("corrupt line pointer in relation \"%s\" at block %u, offset %u",
                                    RelationGetRelationName(rel), blkno, offnum)));
                else
                {
                    tuple = (HeapTupleHeader) PageGetItem(page, lp);
                    tuple_len = ItemIdGetLength(lp);
                    pg_integrity_checksums_check_heap_tuple(rel, blkno, offnum,
                                                  tuple, tuple_len);

                    heapTuple.t_len = tuple_len;
                    heapTuple.t_data = tuple;
                    heapTuple.t_tableOid = reloid;
                    heapTuple.t_self = *tid;

                    checksum = pg_tuple_logical_checksum_internal(rel, &heapTuple,
                                                                  algo);
                }
            }
        }
    }
    PG_CATCH();
    {
        if (lock_held && BufferIsValid(buffer))
            UnlockReleaseBuffer(buffer);
        relation_close(rel, AccessShareLock);
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (lock_held && BufferIsValid(buffer))
        UnlockReleaseBuffer(buffer);
    relation_close(rel, AccessShareLock);

    if (missing || checksum == 0)
        return false;

    *out = checksum;
    return true;
}

PG_FUNCTION_INFO_V1(pg_tuple_logical_checksum);

Datum
pg_tuple_logical_checksum(PG_FUNCTION_ARGS)
{
    uint64      checksum;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    if (!pg_tuple_logical_checksum_common(PG_GETARG_OID(0),
                                          PG_GETARG_ITEMPOINTER(1), 0,
                                          &checksum))
        PG_RETURN_NULL();

    PG_RETURN_INT32((int32) checksum);
}

PG_FUNCTION_INFO_V1(pg_tuple_logical_checksum_algo);

Datum
pg_tuple_logical_checksum_algo(PG_FUNCTION_ARGS)
{
    int         algorithm;
    uint64      checksum;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
        PG_RETURN_NULL();

    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(2));

    if (!pg_tuple_logical_checksum_common(PG_GETARG_OID(0),
                                          PG_GETARG_ITEMPOINTER(1), algorithm,
                                          &checksum))
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(checksum, algorithm == 0 ? 4 : 8));
}

