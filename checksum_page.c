/*-------------------------------------------------------------------------
 *
 * checksum_page.c
 *    Page checksum verification and canonical cell checksums
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "checksum_internal.h"

/*-------------------------------------------------------------------------
 * PAGE LEVEL FUNCTIONS (Physical only)
 *-------------------------------------------------------------------------
 */

/*
 * pg_page_checksum_read - shared page read and checksum calculation
 *
 * Returns the calculated 16-bit checksum and the checksum stored in the page
 * header.  *is_new is true for an uninitialized page; in that case both
 * checksum values are zero.  The stored value is captured before the page is
 * copied, because pg_checksum_page() zeroes pd_checksum in its input.
 */
static void
pg_page_checksum_read(Oid reloid, int32 blkno, bool from_disk,
                      uint16 *calculated, uint16 *stored, bool *is_new)
{
    Relation    rel;
    Buffer      buffer;
    Page        page;
    BlockNumber nblocks;
    char       *pagecopy;

    *calculated = 0;
    *stored = 0;
    *is_new = false;

    rel = relation_open(reloid, AccessShareLock);

    pg_integrity_checksums_require_select(reloid);

    if (!pg_integrity_checksums_relation_has_storage(rel))
    {
        char       *relname = pstrdup(RelationGetRelationName(rel));

        relation_close(rel, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("relation \"%s\" has no storage", relname)));
    }

    nblocks = RelationGetNumberOfBlocks(rel);

    /*
     * BlockNumber is unsigned, and P_NEW (0xFFFFFFFF) is what a negative
     * int4 turns into.  ReadBuffer(P_NEW) would extend the relation, so the
     * range must be validated before reading.
     */
    if (blkno < 0 || (uint32) blkno >= nblocks)
    {
        char       *relname = pstrdup(RelationGetRelationName(rel));

        relation_close(rel, AccessShareLock);

        if (nblocks == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("relation \"%s\" has no pages", relname)));

        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("block number %d is out of range for relation \"%s\" (0..%u)",
                        blkno, relname, nblocks - 1)));
    }

    pagecopy = (char *) palloc(BLCKSZ);

    /*
     * Verification must inspect the on-disk checksum, not a possibly dirty
     * buffer whose pd_checksum has not been updated yet.  Temporary relations
     * have no shared storage, so they keep the buffer path.
     */
    if (from_disk && !RelationUsesLocalBuffers(rel))
    {
        smgrread(RelationGetSmgr(rel), MAIN_FORKNUM, (BlockNumber) blkno,
                 pagecopy);

        if (PageIsNew((Page) pagecopy))
        {
            pfree(pagecopy);
            relation_close(rel, AccessShareLock);
            *is_new = true;
            return;
        }

        *stored = ((PageHeader) pagecopy)->pd_checksum;
        *calculated = pg_checksum_page(pagecopy, (BlockNumber) blkno);
        pfree(pagecopy);
        relation_close(rel, AccessShareLock);
        return;
    }

    buffer = ReadBuffer(rel, (BlockNumber) blkno);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buffer);

    if (PageIsNew(page))
    {
        UnlockReleaseBuffer(buffer);
        pfree(pagecopy);
        relation_close(rel, AccessShareLock);
        *is_new = true;
        return;
    }

    *stored = ((PageHeader) page)->pd_checksum;

    /*
     * pg_checksum_page() transiently zeroes pd_checksum.  It must not run on
     * a page from the shared buffer pool: a concurrent caller could observe
     * the zeroed field, and an error between the two stores would leave the
     * page with a bogus checksum.  Copy the page and compute on the copy.
     */
    memcpy(pagecopy, page, BLCKSZ);
    UnlockReleaseBuffer(buffer);

    *calculated = pg_checksum_page(pagecopy, (BlockNumber) blkno);

    pfree(pagecopy);
    relation_close(rel, AccessShareLock);
}

/*
 * pg_page_checksum - calculate PostgreSQL's 16-bit page checksum
 *
 * This is a calculator, not a verifier.  See pg_page_checksum_status() for
 * validation against the stored pd_checksum.
 */
PG_FUNCTION_INFO_V1(pg_page_checksum);

Datum
pg_page_checksum(PG_FUNCTION_ARGS)
{
    Oid         reloid;
    int32       blkno;
    uint16      calculated;
    uint16      stored;
    bool        is_new;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    reloid = PG_GETARG_OID(0);
    blkno = PG_GETARG_INT32(1);

    pg_page_checksum_read(reloid, blkno, false, &calculated, &stored, &is_new);

    PG_RETURN_INT32(is_new ? 0 : (int32) calculated);
}

/*
 * pg_page_checksum_status - verify a page against its stored checksum
 *
 * Returns one of: ok, mismatch, new, checksums_disabled.
 */
PG_FUNCTION_INFO_V1(pg_page_checksum_status);

Datum
pg_page_checksum_status(PG_FUNCTION_ARGS)
{
    Oid         reloid;
    int32       blkno;
    uint16      calculated;
    uint16      stored;
    bool        is_new;
    const char *status;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    reloid = PG_GETARG_OID(0);
    blkno = PG_GETARG_INT32(1);

    pg_page_checksum_read(reloid, blkno, true, &calculated, &stored, &is_new);

    if (!DataChecksumsEnabled())
        status = "checksums_disabled";
    else if (is_new)
        status = "new";
    else if (calculated == stored)
        status = "ok";
    else
        status = "mismatch";

    PG_RETURN_TEXT_P(cstring_to_text(status));
}

/*-------------------------------------------------------------------------
 * CELL LEVEL FUNCTIONS (Logical only)
 *-------------------------------------------------------------------------
 */

/*
 * pg_cell_checksum_common - shared implementation for cell checksums
 *
 * Returns false for missing tuples, dropped columns and invalid TIDs. The
 * algorithm selects the canonical hash width; *out is only valid when the
 * function returns true.
 */
static bool
pg_cell_checksum_common(Oid reloid, ItemPointer tid, int32 attnum_arg,
                        ChecksumAlgo algo, uint64 *out)
{
    Relation    rel;
    Buffer      buffer;
    Page        page;
    HeapTupleHeader tuple;
    ItemId      lp;
    Datum       value;
    bool        isnull;
    HeapTupleData heapTuple;
    TupleDesc   tupdesc;
    Form_pg_attribute attr;
    BlockNumber blkno;
    OffsetNumber offnum;
    Size        tuple_len;

    if (attnum_arg <= 0 || !ItemPointerIsValid(tid))
        return false;

    rel = relation_open(reloid, AccessShareLock);
    pg_integrity_checksums_require_select(reloid);
    pg_integrity_checksums_check_relation_kind(rel, false, false);
    tupdesc = RelationGetDescr(rel);

    if (attnum_arg > tupdesc->natts)
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

    buffer = ReadBuffer(rel, blkno);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buffer);

    if (PageIsNew(page))
    {
        UnlockReleaseBuffer(buffer);
        relation_close(rel, AccessShareLock);
        return false;
    }

    pg_integrity_checksums_check_page(rel, blkno, page);

    if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
    {
        UnlockReleaseBuffer(buffer);
        relation_close(rel, AccessShareLock);
        return false;
    }

    lp = PageGetItemId(page, offnum);
    if (!ItemIdIsNormal(lp))
    {
        UnlockReleaseBuffer(buffer);
        relation_close(rel, AccessShareLock);
        return false;
    }

    if (!pg_integrity_checksums_item_is_sane(page, lp))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("corrupt line pointer in relation \"%s\" at block %u, offset %u",
                        RelationGetRelationName(rel), blkno, offnum)));

    tuple = (HeapTupleHeader) PageGetItem(page, lp);
    tuple_len = ItemIdGetLength(lp);
    pg_integrity_checksums_check_heap_tuple(rel, blkno, offnum, tuple, tuple_len);

    heapTuple.t_len = tuple_len;
    heapTuple.t_data = tuple;
    heapTuple.t_tableOid = reloid;
    heapTuple.t_self = *tid;

    attr = TupleDescAttr(tupdesc, attnum_arg - 1);
    if (attr->attisdropped)
    {
        UnlockReleaseBuffer(buffer);
        relation_close(rel, AccessShareLock);
        return false;
    }

    value = heap_getattr(&heapTuple, attnum_arg, tupdesc, &isnull);
    *out = pg_cell_checksum_canonical_algo(value, isnull,
                                           attr->atttypid,
                                           attr->atttypmod,
                                           attnum_arg, algo);

    UnlockReleaseBuffer(buffer);
    relation_close(rel, AccessShareLock);
    return true;
}

/*
 * pg_cell_checksum - default SQL function for cell-level checksums
 *
 * Returns a 32-bit integer.  The generic pg_cell_checksum(..., text)
 * overload returns a bytea digest.
 */
PG_FUNCTION_INFO_V1(pg_cell_checksum);

Datum
pg_cell_checksum(PG_FUNCTION_ARGS)
{
    uint64      checksum;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
        PG_RETURN_NULL();

    if (!pg_cell_checksum_common(PG_GETARG_OID(0), PG_GETARG_ITEMPOINTER(1),
                                 PG_GETARG_INT32(2), CHECKSUM_ALGO_FNV32,
                                 &checksum))
        PG_RETURN_NULL();

    PG_RETURN_INT32((int32) checksum);
}

PG_FUNCTION_INFO_V1(pg_cell_checksum_algo);

Datum
pg_cell_checksum_algo(PG_FUNCTION_ARGS)
{
    ChecksumAlgo         algorithm;
    uint64      checksum;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) ||
        PG_ARGISNULL(3))
        PG_RETURN_NULL();

    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(3));

    if (!pg_cell_checksum_common(PG_GETARG_OID(0), PG_GETARG_ITEMPOINTER(1),
                                 PG_GETARG_INT32(2), algorithm, &checksum))
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(checksum, algorithm == CHECKSUM_ALGO_FNV32 ? 4 : 8));
}

