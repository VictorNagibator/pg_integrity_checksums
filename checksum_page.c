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
#include "common/relpath.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "checksum_internal.h"

/*
 * checksum_impl.h holds the data checksum algorithm, so an extension can
 * compute it without calling pg_checksum_page(), which asserts that the page
 * is initialized (see page_checksum_compute()).  The header also defines
 * pg_checksum_page() itself; rename that copy so it does not clash with the
 * server's function.
 */
#define pg_checksum_page pg_integrity_checksums_impl_checksum_page
extern uint16 pg_checksum_page(char *page, BlockNumber blkno);
#include "storage/checksum_impl.h"
#undef pg_checksum_page

/*-------------------------------------------------------------------------
 * PAGE LEVEL FUNCTIONS (Physical only)
 *-------------------------------------------------------------------------
 */

/*
 * Number of times pg_page_checksum_status() reads a block from disk before it
 * reports a mismatch; see pg_page_checksum_read().
 */
#define PAGE_CHECKSUM_READ_ATTEMPTS 5

/*
 * smgrread() requires an I/O-aligned buffer on PostgreSQL 16 and later
 * (direct I/O); a plain palloc() is only MAXALIGNed.
 */
#if PG_VERSION_NUM >= 160000
typedef PGIOAlignedBlock PageChecksumBlock;
#else
typedef PGAlignedBlock PageChecksumBlock;
#endif

/*
 * page_checksum_require_storage - reject relations without storage
 */
static void
page_checksum_require_storage(Relation rel)
{
    if (!pg_integrity_checksums_relation_has_storage(rel))
    {
        char       *relname = pstrdup(RelationGetRelationName(rel));

        relation_close(rel, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("relation \"%s\" has no storage", relname)));
    }
}

/*
 * page_checksum_require_own_temp - reject temporary relations of other sessions
 *
 * Their pages live in the other backend's local buffers, so the file is not
 * a meaningful copy to verify.
 */
static void
page_checksum_require_own_temp(Relation rel)
{
    if (RELATION_IS_OTHER_TEMP(rel))
    {
        relation_close(rel, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("cannot access temporary tables of other sessions")));
    }
}

/*
 * page_checksum_compute - data checksum of a page copy
 *
 * Same value as pg_checksum_page(), which asserts !PageIsNew() and so cannot
 * be used on a corrupt header with pd_upper = 0 that is not an all-zero page:
 * that page must be reported as a mismatch, not crash an assert-enabled
 * server.  pd_checksum of the copy is zeroed during the computation and then
 * restored.
 */
static uint16
page_checksum_compute(char *page, BlockNumber blkno)
{
    PGChecksummablePage *cpage = (PGChecksummablePage *) page;
    uint16      save_checksum;
    uint32      checksum;

    save_checksum = cpage->phdr.pd_checksum;
    cpage->phdr.pd_checksum = 0;
    checksum = pg_checksum_block(cpage);
    cpage->phdr.pd_checksum = save_checksum;

    checksum ^= blkno;

    return (uint16) ((checksum % 65535) + 1);
}

/*
 * page_checksum_read_block - checksum one block of an open relation
 *
 * Returns the calculated 16-bit checksum and the checksum stored in the page
 * header.  *is_new is true for an uninitialized page; in that case both
 * checksum values are zero.  The stored value is captured before the page is
 * copied, because page_checksum_compute() zeroes pd_checksum in its input.
 * pagecopy is a BLCKSZ buffer, I/O-aligned for the disk path.  The caller
 * holds a lock on the relation and has checked that the block exists.
 */
static void
page_checksum_read_block(Relation rel, ForkNumber forknum, BlockNumber blkno,
                         bool from_disk, char *pagecopy,
                         uint16 *calculated, uint16 *stored, bool *is_new)
{
    Buffer      buffer;
    Page        page;

    *calculated = 0;
    *stored = 0;
    *is_new = false;

    /*
     * Verification must inspect the on-disk checksum, not a possibly dirty
     * buffer whose pd_checksum has not been updated yet.  That holds for
     * temporary relations too: a local buffer gets its checksum only when it
     * is written out, so a dirty one does not match.  Their file is private
     * to this backend and is read the same way.
     */
    if (from_disk)
    {
        int         attempt;

        /*
         * The read takes no buffer lock, so it can overlap with the write of
         * the same block by the checkpointer or a backend and return a torn
         * page: part old, part new, with a checksum that matches neither.
         * pg_basebackup handles this by reading the block again.  Do the
         * same: report a mismatch only when every read disagrees.  A
         * corrupt page fails each time, while a torn read does not repeat,
         * because the write in progress completes within moments.
         */
        for (attempt = 1;; attempt++)
        {
            smgrread(RelationGetSmgr(rel), forknum, blkno, pagecopy);

            /*
             * A new page is all zeroes.  pd_upper == 0 alone is a corrupt
             * header and falls through to the checksum comparison.
             */
            if (PageIsNew((Page) pagecopy) &&
                pg_integrity_checksums_page_is_zero(pagecopy))
            {
                *stored = 0;
                *calculated = 0;
                *is_new = true;
                return;
            }

            *stored = ((PageHeader) pagecopy)->pd_checksum;
            *calculated = page_checksum_compute(pagecopy, blkno);

            if (*calculated == *stored || !DataChecksumsEnabled() ||
                attempt >= PAGE_CHECKSUM_READ_ATTEMPTS)
                break;

            /* Give a concurrent write time to finish. */
            pg_usleep(10000L * attempt);
            CHECK_FOR_INTERRUPTS();
        }
        return;
    }

    buffer = ReadBufferExtended(rel, forknum, blkno, RBM_NORMAL, NULL);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buffer);

    if (PageIsNew(page))
    {
        UnlockReleaseBuffer(buffer);
        *is_new = true;
        return;
    }

    *stored = ((PageHeader) page)->pd_checksum;

    /*
     * page_checksum_compute() transiently zeroes pd_checksum.  It must not run on
     * a page from the shared buffer pool: a concurrent caller could observe
     * the zeroed field, and an error between the two stores would leave the
     * page with a bogus checksum.  Copy the page and compute on the copy.
     */
    memcpy(pagecopy, page, BLCKSZ);
    UnlockReleaseBuffer(buffer);

    *calculated = page_checksum_compute(pagecopy, blkno);
}

/*
 * pg_page_checksum_read - open a relation and checksum one main-fork block
 */
static void
pg_page_checksum_read(Oid reloid, int32 blkno, bool from_disk,
                      uint16 *calculated, uint16 *stored, bool *is_new)
{
    Relation    rel;
    BlockNumber nblocks;
    PageChecksumBlock pagebuf;

    rel = relation_open(reloid, AccessShareLock);

    pg_integrity_checksums_require_select(reloid);
    page_checksum_require_storage(rel);
    if (from_disk)
        page_checksum_require_own_temp(rel);

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

    page_checksum_read_block(rel, MAIN_FORKNUM, (BlockNumber) blkno,
                             from_disk, pagebuf.data,
                             calculated, stored, is_new);

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

/*
 * pg_relation_page_checksum_status - verify every page of a relation
 *
 * Arguments: relation, fork name (NULL for every fork that exists) and
 * include_ok.  Returns one row per block (fork, blkno, status,
 * stored_checksum, calculated_checksum) whose status is mismatch, or of
 * every block when include_ok is true.  Each block is checked like
 * pg_page_checksum_status() does it: the on-disk copy, read again before a
 * mismatch is reported.  Fails when data checksums are disabled, since no
 * page could then be verified.
 */
PG_FUNCTION_INFO_V1(pg_relation_page_checksum_status);

Datum
pg_relation_page_checksum_status(PG_FUNCTION_ARGS)
{
    Oid         reloid;
    bool        include_ok;
    bool        all_forks;
    ForkNumber  only_fork = MAIN_FORKNUM;
    ForkNumber  forknum;
    Relation    rel;
    Tuplestorestate *store;
    TupleDesc   resultdesc;
    PageChecksumBlock pagebuf;

    store = pg_integrity_checksums_begin_srf(fcinfo, &resultdesc);

    /* A NULL relation has no pages: return an empty set. */
    if (PG_ARGISNULL(0))
        return (Datum) 0;
    reloid = PG_GETARG_OID(0);
    all_forks = PG_ARGISNULL(1);
    if (!all_forks)
    {
        char       *forkname = text_to_cstring(PG_GETARG_TEXT_PP(1));

        /* Raises "invalid fork name" for anything but main/fsm/vm/init. */
        only_fork = forkname_to_number(forkname);
    }
    include_ok = PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2);

    rel = relation_open(reloid, AccessShareLock);
    pg_integrity_checksums_require_select(reloid);
    page_checksum_require_storage(rel);
    page_checksum_require_own_temp(rel);

    if (!DataChecksumsEnabled())
    {
        relation_close(rel, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("data checksums are not enabled in this cluster"),
                 errhint("Page checksums are only maintained when the cluster was initialized with data checksums (initdb --data-checksums) or they were enabled with pg_checksums --enable.")));
    }

    for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
    {
        BlockNumber nblocks;
        BlockNumber blkno;

        if (!all_forks && forknum != only_fork)
            continue;
        if (!smgrexists(RelationGetSmgr(rel), forknum))
            continue;

        /*
         * Blocks added after this point are not checked.  The lock keeps
         * VACUUM from truncating the relation underneath the scan.
         */
        nblocks = smgrnblocks(RelationGetSmgr(rel), forknum);

        for (blkno = 0; blkno < nblocks; blkno++)
        {
            uint16      calculated;
            uint16      stored;
            bool        is_new;
            const char *status;
            Datum       values[5];
            bool        nulls[5] = {false, false, false, false, false};

            CHECK_FOR_INTERRUPTS();

            page_checksum_read_block(rel, forknum, blkno, true, pagebuf.data,
                                     &calculated, &stored, &is_new);

            if (is_new)
                status = "new";
            else if (calculated == stored)
                status = "ok";
            else
                status = "mismatch";

            if (!include_ok && strcmp(status, "mismatch") != 0)
                continue;

            values[0] = CStringGetTextDatum(forkNames[forknum]);
            values[1] = Int64GetDatum((int64) blkno);
            values[2] = CStringGetTextDatum(status);
            values[3] = Int32GetDatum((int32) stored);
            values[4] = Int32GetDatum((int32) calculated);
            if (is_new)
                nulls[3] = nulls[4] = true;

            tuplestore_putvalues(store, resultdesc, values, nulls);
        }
    }

    relation_close(rel, AccessShareLock);

    return (Datum) 0;
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
    Datum       value;
    bool        isnull;
    HeapTupleData heapTuple;
    TupleDesc   tupdesc;
    Form_pg_attribute attr;
    BlockNumber blkno;
    OffsetNumber offnum;
    int         ordinal = 0;
    int         i;

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

    /* Missing and invisible tuples have no cell; see the helper. */
    if (!pg_integrity_checksums_fetch_visible_tuple(rel, buffer, blkno, offnum,
                                                    &heapTuple))
    {
        UnlockReleaseBuffer(buffer);
        relation_close(rel, AccessShareLock);
        return false;
    }

    attr = TupleDescAttr(tupdesc, attnum_arg - 1);
    if (attr->attisdropped)
    {
        UnlockReleaseBuffer(buffer);
        relation_close(rel, AccessShareLock);
        return false;
    }

    /*
     * Hash the position among the live columns, exactly as the tuple
     * checksum does, so that dropped columns do not matter.
     */
    for (i = 1; i <= attnum_arg; i++)
    {
        if (!TupleDescAttr(tupdesc, i - 1)->attisdropped)
            ordinal++;
    }

    /*
     * Hash with the buffer pinned but not locked: canonicalization may
     * detoast the value and call type send functions, which must not run
     * under a buffer content lock.  The pin keeps the tuple in place.
     */
    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

    value = heap_getattr(&heapTuple, attnum_arg, tupdesc, &isnull);
    *out = pg_cell_checksum_canonical_algo(value, isnull,
                                           attr->atttypid,
                                           attr->atttypmod,
                                           ordinal, algo);

    ReleaseBuffer(buffer);
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

    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(3));

    if (!pg_cell_checksum_common(PG_GETARG_OID(0), PG_GETARG_ITEMPOINTER(1),
                                 PG_GETARG_INT32(2), algorithm, &checksum))
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(checksum, checksum_algo_len(algorithm)));
}

