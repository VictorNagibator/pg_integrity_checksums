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
#include "storage/predicate.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "checksum_internal.h"

/*-------------------------------------------------------------------------
 * TUPLE LEVEL FUNCTIONS (Physical and Logical)
 *-------------------------------------------------------------------------
 */

/*
 * hash_tuple_with_header - hash a whole tuple with its hint bits masked
 *
 * The header (t_hoff bytes) is copied and its hint bits cleared, then the
 * hash continues over the unmodified rest of the tuple, so the result equals
 * hashing the concatenation.  "seed" follows the usual convention that zero
 * selects the offset basis.
 */
static uint64
hash_tuple_with_header(HeapTupleHeader tuple, Size len, uint64 seed,
                       ChecksumAlgo algo)
{
    unsigned char header[256];  /* t_hoff is a single byte */
    Size        hoff = tuple->t_hoff;
    const unsigned char *rest = (const unsigned char *) tuple + hoff;

    /* Callers validated hoff <= len. */
    Assert(hoff <= len && hoff <= sizeof(header));

    memcpy(header, tuple, hoff);
    ((HeapTupleHeader) header)->t_infomask &= ~HEAP_HINT_BITS;

    if (algo == CHECKSUM_ALGO_FNV32)
    {
        uint32      h = fnv1a_32_hash(header, hoff, (uint32) seed);

        return fnv1a_32_buf(rest, len - hoff, h);
    }
    else
    {
        uint64      h = fnv1a_64_hash(header, hoff, seed);

        return fnv1a_64_buf(rest, len - hoff, h);
    }
}

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
                                   bool validate, ChecksumAlgo algo)
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

    if (!include_header)
    {
        /* Skip header, checksum only the tuple data */
        data = (char *) tuple + tuple->t_hoff;
        len -= tuple->t_hoff;
    }
    else
        data = (char *) tuple;

    /* Create 64-bit location from block number and offset */
    location = ((uint64)blkno << 32) | (uint64)offnum;

    /* Hash the location with the selected algorithm. */
    location_hash = (algo == CHECKSUM_ALGO_FNV32) ?
        fnv1a_32_hash(&location, sizeof(location), FNV_BASIS_32) :
        fnv1a_64_hash(&location, sizeof(location), FNV_BASIS_64);

    /*
     * Include relation identity and storage identity.  relfilenode changes
     * when VACUUM FULL, CLUSTER or ALTER TABLE SET TABLESPACE rewrite the
     * relation, so a tuple that happens to land at the same (block, offset)
     * still has a different physical checksum.  The relation OID keeps
     * checksums from different relations distinct.
     */
    location_hash = combine_hash(location_hash,
                                     (uint64) RelationGetRelid(rel), algo);
    location_hash = combine_hash(location_hash,
                                     (uint64) pg_integrity_checksums_relfilenumber(rel), algo);
    location_hash = combine_hash(location_hash,
                                     (uint64) rel->rd_rel->reltablespace, algo);

    /*
     * Include the page size/layout version for additional uniqueness.  Do
     * not include the page LSN, pd_checksum or pd_flags: they change when a
     * concurrent scan sets hint bits, which made a physical checksum depend
     * on who happened to read the page first.
     */
    page_info = PageGetPageSize(page) | (PageGetPageLayoutVersion(page) << 16);
    location_hash = combine_hash(location_hash, page_info, algo);

    /*
     * Calculate the checksum using location_hash as the initial value.  When
     * the header is included, its hint bits are masked out first: they are
     * set lazily by whichever backend reads the tuple first (and differ
     * between a primary and its standbys), so they are not part of the
     * tuple's stable physical state.
     */
    if (include_header)
        checksum = hash_tuple_with_header(tuple, len, location_hash, algo);
    else if (algo == CHECKSUM_ALGO_FNV32)
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
    checksum = combine_hash(checksum, itemid_info, algo);

    if (algo == CHECKSUM_ALGO_FNV32)
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
 * pg_integrity_checksums_fetch_visible_tuple - find a tuple by TID, if visible
 *
 * Used by the functions that checksum one tuple or cell.  Returns false when
 * the page is new, the offset is out of range, the line pointer is not a
 * normal one, or the tuple is not visible to the statement's snapshot.  On
 * success *tuple describes the tuple in the buffer.  The buffer must be
 * share-locked; a corrupt page header, line pointer or tuple header raises a
 * clean DATA_CORRUPTED error.
 *
 * The visibility check matters: the TID of a deleted row, of an old version
 * of an updated row or of a row inserted by a transaction that has not
 * committed still points at its data.  Without the check these functions
 * returned checksums of data the caller cannot read with SELECT, and a
 * checksum of a short value (a 32-bit FNV-1a of an integer, say) is easily
 * reversed by trying every candidate.
 */
bool
pg_integrity_checksums_fetch_visible_tuple(Relation rel, Buffer buffer,
                                           BlockNumber blkno,
                                           OffsetNumber offnum,
                                           HeapTuple tuple)
{
    Page        page = BufferGetPage(buffer);
    ItemId      lp;
    HeapTupleHeader htup;
    Size        len;
    Snapshot    snapshot;
    bool        valid;

    if (PageIsNew(page))
        return false;

    pg_integrity_checksums_check_page(rel, blkno, page);

    if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
        return false;

    lp = PageGetItemId(page, offnum);
    if (!ItemIdIsNormal(lp))
        return false;

    if (!pg_integrity_checksums_item_is_sane(page, lp))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("corrupt line pointer in relation \"%s\" at block %u, offset %u",
                        RelationGetRelationName(rel), blkno, offnum)));

    htup = (HeapTupleHeader) PageGetItem(page, lp);
    len = ItemIdGetLength(lp);
    pg_integrity_checksums_check_heap_tuple(rel, blkno, offnum, htup, len);

    tuple->t_len = len;
    tuple->t_data = htup;
    tuple->t_tableOid = RelationGetRelid(rel);
    ItemPointerSet(&tuple->t_self, blkno, offnum);

    snapshot = pg_integrity_checksums_get_snapshot();
    valid = HeapTupleSatisfiesVisibility(tuple, snapshot, buffer);

    /*
     * In a SERIALIZABLE transaction, take a SIREAD lock and check for a
     * conflict as heap_fetch() does.  The lock is page-level rather than on
     * the TID: PredicateLockTID()'s signature differs between releases, and
     * a coarser lock only adds false positives, never misses a conflict.
     */
    if (valid)
        PredicateLockPage(rel, blkno, snapshot);
    pg_integrity_checksums_serializable_conflict_out(valid, rel, tuple,
                                                     buffer, snapshot);
    return valid;
}

/*
 * pg_tuple_physical_checksum_common - shared physical tuple implementation
 */
static bool
pg_tuple_physical_checksum_common(Oid reloid, ItemPointer tid,
                                  bool include_header, ChecksumAlgo algo, uint64 *out)
{
    Relation    rel;
    Buffer      buffer;
    HeapTupleData tuple;
    uint64      checksum = 0;
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

    /*
     * No PG_TRY needed: on ERROR the buffer lock, pin and relation lock are
     * released by transaction abort.
     */
    buffer = ReadBuffer(rel, blkno);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);

    /*
     * The physical checksum reads header fields that concurrent backends
     * change, so it is computed under the share lock, which is fine: it runs
     * no user code.
     */
    if (pg_integrity_checksums_fetch_visible_tuple(rel, buffer, blkno, offnum,
                                                   &tuple))
        checksum = pg_tuple_physical_checksum_internal(rel,
                                                       BufferGetPage(buffer),
                                                       offnum, blkno,
                                                       include_header, false,
                                                       algo);

    UnlockReleaseBuffer(buffer);
    relation_close(rel, AccessShareLock);

    /*
     * A tuple checksum is never zero, so zero means there is no visible tuple
     * at this TID: NULL, as for the logical tuple and cell checksums.  (This
     * used to return 0 for an offset without a tuple.)
     */
    if (checksum == 0)
        return false;

    *out = checksum;
    return true;
}

PG_FUNCTION_INFO_V1(pg_tuple_physical_checksum);

Datum
pg_tuple_physical_checksum(PG_FUNCTION_ARGS)
{
    uint64      checksum;

    if (!pg_tuple_physical_checksum_common(PG_GETARG_OID(0),
                                           PG_GETARG_ITEMPOINTER(1),
                                           PG_GETARG_BOOL(2),
                                           0, &checksum))
        PG_RETURN_NULL();

    PG_RETURN_INT32((int32) checksum);
}

PG_FUNCTION_INFO_V1(pg_tuple_physical_checksum_algo);

Datum
pg_tuple_physical_checksum_algo(PG_FUNCTION_ARGS)
{
    ChecksumAlgo         algorithm;
    uint64      checksum;

    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(3));

    if (!pg_tuple_physical_checksum_common(PG_GETARG_OID(0),
                                           PG_GETARG_ITEMPOINTER(1),
                                           PG_GETARG_BOOL(2),
                                           algorithm, &checksum))
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(checksum, algorithm == CHECKSUM_ALGO_FNV32 ? 4 : 8));
}

/*-------------------------------------------------------------------------
 * Logical Tuple Checksum
 *-------------------------------------------------------------------------
 */

/*
 * logical_row_hasher_init - prepare to hash many rows of one relation
 *
 * Works out the live (non-dropped) columns and their ordinals once, and
 * allocates the deform arrays, the per-column type cache hints and a memory
 * context that is reset after every row.  The tuple descriptor must stay
 * valid while the hasher is in use, which holding a lock on the relation
 * guarantees.
 */
void
logical_row_hasher_init(LogicalRowHasher *hasher, TupleDesc tupdesc,
                        TupleDesc order_desc)
{
    int         natts = tupdesc->natts;
    int         i;

    hasher->tupdesc = tupdesc;
    hasher->ncols = 0;
    hasher->attidx = palloc(sizeof(int) * Max(natts, 1));
    hasher->hints = palloc0(sizeof(LogicalColumnHint) * Max(natts, 1));
    hasher->values = palloc(sizeof(Datum) * Max(natts, 1));
    hasher->nulls = palloc(sizeof(bool) * Max(natts, 1));
    hasher->rowcxt = AllocSetContextCreate(CurrentMemoryContext,
                                           "pg_integrity_checksums row",
                                           ALLOCSET_DEFAULT_SIZES);

    if (order_desc == NULL)
    {
        for (i = 0; i < natts; i++)
        {
            if (!TupleDescAttr(tupdesc, i)->attisdropped)
                hasher->attidx[hasher->ncols++] = i;
        }
        return;
    }

    /*
     * Take the columns in the order of order_desc, matched by name: a
     * partition may declare its columns in another order than its parent.
     */
    for (i = 0; i < order_desc->natts; i++)
    {
        Form_pg_attribute oattr = TupleDescAttr(order_desc, i);
        int         j;

        if (oattr->attisdropped)
            continue;

        for (j = 0; j < natts; j++)
        {
            Form_pg_attribute attr = TupleDescAttr(tupdesc, j);

            if (!attr->attisdropped &&
                strcmp(NameStr(attr->attname), NameStr(oattr->attname)) == 0)
                break;
        }

        if (j >= natts)
            elog(ERROR, "column \"%s\" of the partitioned table was not found in a partition",
                 NameStr(oattr->attname));

        hasher->attidx[hasher->ncols++] = j;
    }
}

/*
 * logical_columns_same_order - do both descriptors list the same live columns
 *
 * True when the non-dropped columns of tupdesc have the names of order_desc
 * in the same sequence, so no reordering is needed.
 */
bool
logical_columns_same_order(TupleDesc tupdesc, TupleDesc order_desc)
{
    int         i = 0;
    int         j = 0;

    for (;;)
    {
        while (i < tupdesc->natts && TupleDescAttr(tupdesc, i)->attisdropped)
            i++;
        while (j < order_desc->natts && TupleDescAttr(order_desc, j)->attisdropped)
            j++;

        if (i >= tupdesc->natts || j >= order_desc->natts)
            return i >= tupdesc->natts && j >= order_desc->natts;

        if (strcmp(NameStr(TupleDescAttr(tupdesc, i)->attname),
                   NameStr(TupleDescAttr(order_desc, j)->attname)) != 0)
            return false;

        i++;
        j++;
    }
}

/*
 * logical_row_hasher_free - release what logical_row_hasher_init() allocated
 */
void
logical_row_hasher_free(LogicalRowHasher *hasher)
{
    pfree(hasher->attidx);
    pfree(hasher->hints);
    pfree(hasher->values);
    pfree(hasher->nulls);
    MemoryContextDelete(hasher->rowcxt);
}

/*
 * logical_row_hash - Core logical tuple checksum
 *
 * Computes a canonical logical checksum for a tuple.  All non-dropped
 * columns are hashed; relation OIDs, heap TIDs and the tuple header are not
 * included.  Each column is hashed with its position among the live columns,
 * not its physical attribute number: dropping a column must not change the
 * checksum of the remaining data.
 *
 * This checksum is stable across:
 * - VACUUM FULL
 * - CLUSTER
 * - REINDEX
 * - Physical data movement
 * - Storage reorganization
 *
 * The tuple is deformed once (heap_getattr() per column is quadratic in the
 * number of columns once a varlena or NULL precedes them), and everything
 * the encoding allocates (detoasted values, send function results) is
 * released with the per-row memory context.
 */
uint64
logical_row_hash(LogicalRowHasher *hasher, HeapTuple tuple, ChecksumAlgo algo)
{
    uint64      checksum;

    logical_row_deform(hasher, tuple);
    checksum = logical_row_hash_values(hasher, algo);
    MemoryContextReset(hasher->rowcxt);

    return checksum;
}

/*
 * logical_row_deform - deform a row into hasher->values / hasher->nulls
 *
 * The first step of logical_row_hash(), for callers that hash the same row
 * more than once (with logical_row_hash_values()) or read other values from
 * it.  By-reference values point into the tuple, so the tuple must stay
 * pinned until the caller resets hasher->rowcxt.
 */
void
logical_row_deform(LogicalRowHasher *hasher, HeapTuple tuple)
{
    heap_deform_tuple(tuple, hasher->tupdesc, hasher->values, hasher->nulls);
}

/*
 * logical_row_hash_values - logical checksum of the row last deformed
 *
 * Whatever the encoding allocates goes into hasher->rowcxt, which the caller
 * resets once it is done with the row.
 */
uint64
logical_row_hash_values(LogicalRowHasher *hasher, ChecksumAlgo algo)
{
    TupleDesc   tupdesc = hasher->tupdesc;
    uint64      checksum = algo == CHECKSUM_ALGO_FNV32 ? FNV_BASIS_32 : FNV_BASIS_64;
    MemoryContext oldcontext;
    int         c;

    oldcontext = MemoryContextSwitchTo(hasher->rowcxt);

    for (c = 0; c < hasher->ncols; c++)
    {
        int         i = hasher->attidx[c];
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        uint64      col_hash;

        col_hash = pg_cell_checksum_canonical_hinted(hasher->values[i],
                                                     hasher->nulls[i],
                                                     attr->atttypid,
                                                     attr->atttypmod,
                                                     c + 1, algo,
                                                     &hasher->hints[c]);

        checksum = combine_hash(checksum, col_hash, algo);
    }

    MemoryContextSwitchTo(oldcontext);

    if (algo == CHECKSUM_ALGO_FNV32)
    {
        if (checksum == CHECKSUM_NULL || checksum == 0)
            checksum = hash_mix32((uint32) checksum ^ FNV_BASIS_32);
    }
    else if (checksum == CHECKSUM_NULL_64 || checksum == 0)
        checksum = hash_mix64(checksum ^ FNV_BASIS_64);

    return checksum;
}

/*
 * pg_tuple_logical_checksum_internal - logical checksum of a single tuple
 */
uint64
pg_tuple_logical_checksum_internal(Relation rel, HeapTuple tuple, ChecksumAlgo algo)
{
    LogicalRowHasher hasher;
    uint64      checksum;

    logical_row_hasher_init(&hasher, RelationGetDescr(rel), NULL);
    checksum = logical_row_hash(&hasher, tuple, algo);
    logical_row_hasher_free(&hasher);

    return checksum;
}

/*
 * pg_tuple_logical_checksum_common - shared implementations for tuple checksums
 */
static bool
pg_tuple_logical_checksum_common(Oid reloid, ItemPointer tid, ChecksumAlgo algo,
                                 uint64 *out)
{
    Relation    rel;
    Buffer      buffer;
    HeapTupleData heapTuple;
    uint64      checksum = 0;
    BlockNumber blkno;
    OffsetNumber offnum;

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

    /*
     * No PG_TRY needed: on ERROR the buffer lock, pin and relation lock are
     * released by transaction abort.
     */
    buffer = ReadBuffer(rel, blkno);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);

    if (!pg_integrity_checksums_fetch_visible_tuple(rel, buffer, blkno, offnum,
                                                    &heapTuple))
    {
        UnlockReleaseBuffer(buffer);
        relation_close(rel, AccessShareLock);
        return false;
    }

    /*
     * Hash with the buffer pinned but not locked: the logical encoding
     * detoasts values and calls type send functions, which must not run under
     * a buffer content lock.  The pin keeps the tuple in place.
     */
    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
    checksum = pg_tuple_logical_checksum_internal(rel, &heapTuple, algo);
    ReleaseBuffer(buffer);
    relation_close(rel, AccessShareLock);

    if (checksum == 0)
        return false;

    *out = checksum;
    return true;
}

PG_FUNCTION_INFO_V1(pg_tuple_logical_checksum);

Datum
pg_tuple_logical_checksum(PG_FUNCTION_ARGS)
{
    uint64      checksum;

    if (!pg_tuple_logical_checksum_common(PG_GETARG_OID(0),
                                          PG_GETARG_ITEMPOINTER(1),
                                          CHECKSUM_ALGO_FNV32,
                                          &checksum))
        PG_RETURN_NULL();

    PG_RETURN_INT32((int32) checksum);
}

PG_FUNCTION_INFO_V1(pg_tuple_logical_checksum_algo);

Datum
pg_tuple_logical_checksum_algo(PG_FUNCTION_ARGS)
{
    ChecksumAlgo         algorithm;
    uint64      checksum;

    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(2));

    if (!pg_tuple_logical_checksum_common(PG_GETARG_OID(0),
                                          PG_GETARG_ITEMPOINTER(1), algorithm,
                                          &checksum))
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(checksum, algorithm == CHECKSUM_ALGO_FNV32 ? 4 : 8));
}

