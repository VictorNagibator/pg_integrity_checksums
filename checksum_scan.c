/*-------------------------------------------------------------------------
 *
 * checksum_scan.c
 *    Serial block-range scans shared by serial and parallel execution
 *
 * Every scan exists in a 32-bit and a 64-bit flavor.  Both flavors share one
 * implementation that is parameterized by ChecksumAlgo; the public
 * scan_*_blocks_*() and scan_*_blocks_*64() functions are thin wrappers that
 * keep the accumulator types of their callers.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/rel.h"

#include "checksum_internal.h"

/* Check for interrupts once per this many blocks (must be a power of two). */
#define SCAN_INTERRUPT_INTERVAL 64

/*-------------------------------------------------------------------------
 * Heap scans
 *-------------------------------------------------------------------------
 */

/*
 * scan_visible_tuples - visit the visible tuples of a range of heap blocks
 *
 * Iterates the raw pages of the relation, applying the same visibility
 * rules as a sequential scan so that the resulting tuple set is identical
 * to what the serial path would have seen.  Corrupt page headers, line
 * pointers and tuple headers raise an error instead of being followed.
 */
void
scan_visible_tuples(Relation rel, BlockNumber start, BlockNumber end,
                    Snapshot snapshot, BufferAccessStrategy bstrategy,
                    TupleVisitor visitor, void *arg)
{
    BlockNumber blkno;

    for (blkno = start; blkno < end; blkno++)
    {
        Buffer      buffer;
        Page        page;
        OffsetNumber maxoff;
        OffsetNumber offnum;

        buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
                                    bstrategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);

        if (PageIsNew(page))
        {
            UnlockReleaseBuffer(buffer);
            if ((blkno & (SCAN_INTERRUPT_INTERVAL - 1)) == 0)
                CHECK_FOR_INTERRUPTS();
            continue;
        }

        pg_integrity_checksums_check_page(rel, blkno, page);

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
                                RelationGetRelationName(rel), blkno, offnum)));

            htup = (HeapTupleHeader) PageGetItem(page, lp);
            tuple_len = ItemIdGetLength(lp);

            pg_integrity_checksums_check_heap_tuple(rel, blkno, offnum, htup,
                                                    tuple_len);

            tuple.t_len = tuple_len;
            tuple.t_data = htup;
            tuple.t_tableOid = RelationGetRelid(rel);
            ItemPointerSet(&tuple.t_self, blkno, offnum);

            if (!HeapTupleSatisfiesVisibility(&tuple, snapshot, buffer))
                continue;

            visitor(rel, page, blkno, offnum, &tuple, arg);
        }

        UnlockReleaseBuffer(buffer);

        if ((blkno & (SCAN_INTERRUPT_INTERVAL - 1)) == 0)
            CHECK_FOR_INTERRUPTS();
    }
}

/* State of a physical heap scan; only the accumulator for "algo" is used. */
typedef struct PhysicalVisitorState
{
    bool        include_header;
    ChecksumAlgo algo;
    ChecksumAccum *acc32;
    ChecksumAccum64 *acc64;
} PhysicalVisitorState;

static void
physical_tuple_visitor(Relation rel, Page page, BlockNumber blkno,
                       OffsetNumber offnum, HeapTuple tuple, void *arg)
{
    PhysicalVisitorState *state = (PhysicalVisitorState *) arg;
    uint64      tuple_checksum;

    tuple_checksum = pg_tuple_physical_checksum_internal(rel, page, offnum,
                                                         blkno,
                                                         state->include_header,
                                                         false, state->algo);

    /* Zero means "no checksum" (for example an unusable line pointer). */
    if (tuple_checksum == 0)
        return;

    if (state->algo == CHECKSUM_ALGO_FNV32)
        checksum_accum_add(state->acc32, (uint32) tuple_checksum);
    else
        checksum_accum64_add(state->acc64, tuple_checksum);
}

/*
 * scan_table_blocks_physical - physical checksum over a range of heap blocks
 */
void
scan_table_blocks_physical(Relation rel, BlockNumber start, BlockNumber end,
                           bool include_header, Snapshot snapshot,
                           ChecksumAccum *acc,
                           BufferAccessStrategy bstrategy)
{
    PhysicalVisitorState state = {include_header, CHECKSUM_ALGO_FNV32,
                                  acc, NULL};

    scan_visible_tuples(rel, start, end, snapshot, bstrategy,
                        physical_tuple_visitor, &state);
}

/*
 * scan_table_blocks_physical64 - 64-bit physical checksum over heap blocks
 */
void
scan_table_blocks_physical64(Relation rel, BlockNumber start, BlockNumber end,
                             bool include_header, Snapshot snapshot,
                             ChecksumAccum64 *acc,
                             BufferAccessStrategy bstrategy)
{
    PhysicalVisitorState state = {include_header, CHECKSUM_ALGO_FNV64,
                                  NULL, acc};

    scan_visible_tuples(rel, start, end, snapshot, bstrategy,
                        physical_tuple_visitor, &state);
}

/* State of a logical heap scan; only the accumulator for "algo" is used. */
typedef struct LogicalVisitorState
{
    ChecksumAlgo algo;
    ChecksumLogicalAccum *acc32;
    ChecksumLogicalAccum64 *acc64;
} LogicalVisitorState;

static void
logical_tuple_visitor(Relation rel, Page page, BlockNumber blkno,
                      OffsetNumber offnum, HeapTuple tuple, void *arg)
{
    LogicalVisitorState *state = (LogicalVisitorState *) arg;
    uint64      tuple_checksum;

    tuple_checksum = pg_tuple_logical_checksum_internal(rel, tuple,
                                                        state->algo);

    if (state->algo == CHECKSUM_ALGO_FNV32)
        logical_accum_add(state->acc32, (uint32) tuple_checksum);
    else
        logical_accum_add64(state->acc64, tuple_checksum);
}

/*
 * scan_table_blocks_logical - logical checksum over a range of heap blocks
 */
void
scan_table_blocks_logical(Relation rel, BlockNumber start, BlockNumber end,
                          Snapshot snapshot, ChecksumLogicalAccum *acc,
                          BufferAccessStrategy bstrategy)
{
    LogicalVisitorState state = {CHECKSUM_ALGO_FNV32, acc, NULL};

    scan_visible_tuples(rel, start, end, snapshot, bstrategy,
                        logical_tuple_visitor, &state);
}

/*
 * scan_table_blocks_logical64 - 64-bit logical checksum over heap blocks
 */
void
scan_table_blocks_logical64(Relation rel, BlockNumber start, BlockNumber end,
                            Snapshot snapshot, ChecksumLogicalAccum64 *acc,
                            BufferAccessStrategy bstrategy)
{
    LogicalVisitorState state = {CHECKSUM_ALGO_FNV64, NULL, acc};

    scan_visible_tuples(rel, start, end, snapshot, bstrategy,
                        logical_tuple_visitor, &state);
}

/*-------------------------------------------------------------------------
 * Index scans
 *-------------------------------------------------------------------------
 */

/*
 * index_page_hash - hash one index page together with its header fields
 *
 * Handles both generic index types and BRIN.  The page is hashed as a
 * complete block of BLCKSZ bytes: PageGetPageSize() comes from the page
 * header and a corrupted value could make us read past the buffer.
 */
static uint64
index_page_hash(Page page, BlockNumber blkno, bool is_brin, ChecksumAlgo algo)
{
    PageHeader  phdr = (PageHeader) page;
    const Size  page_size = BLCKSZ;
    uint64      page_hash;

    if (PageIsNew(page))
    {
        page_hash = hash_bytes_algo(blkno,
                                    is_brin ? "BRIN_NEW" : "NEW_PAGE", 8,
                                    algo);
        return combine_hash(page_hash, (uint32) PageGetPageSize(page), algo);
    }

    page_hash = hash_bytes_algo(0, page, page_size, algo);

    if (is_brin)
    {
        page_hash = combine_hash(page_hash, (uint32) page_size, algo);

        /*
         * The BRIN special area can only be inspected when the page header
         * is sane; the raw page bytes are still hashed above.
         */
        if (pg_integrity_checksums_page_header_sane(page))
        {
            uint8      *special_space = (uint8 *) PageGetSpecialPointer(page);
            Size        special_size = PageGetSpecialSize(page);

            if (special_size >= 4)
            {
                uint32      brin_info = 0;

                memcpy(&brin_info, special_space, 4);
                page_hash = combine_hash(page_hash, brin_info, algo);
            }
        }

        page_hash = combine_hash(page_hash, blkno, algo);
        page_hash = combine_hash(page_hash, phdr->pd_lower, algo);
        page_hash = combine_hash(page_hash, phdr->pd_upper, algo);
        page_hash = combine_hash(page_hash, phdr->pd_flags, algo);
    }
    else
    {
        page_hash = combine_hash(page_hash, phdr->pd_lower, algo);
        page_hash = combine_hash(page_hash, phdr->pd_upper, algo);
        page_hash = combine_hash(page_hash, phdr->pd_special, algo);
        page_hash = combine_hash(page_hash, blkno, algo);
        page_hash = combine_hash(page_hash, phdr->pd_flags, algo);
        page_hash = combine_hash(page_hash, (uint32) page_size, algo);
    }

    return page_hash;
}

/*
 * scan_index_blocks_physical_common - physical checksum over index blocks
 *
 * Only the accumulator that matches "algo" is used.
 */
static void
scan_index_blocks_physical_common(Relation idxRel, BlockNumber start,
                                  BlockNumber end, bool is_brin,
                                  ChecksumAlgo algo, ChecksumAccum *acc32,
                                  ChecksumAccum64 *acc64,
                                  BufferAccessStrategy bstrategy)
{
    BlockNumber blkno;

    for (blkno = start; blkno < end; blkno++)
    {
        Buffer      buffer;
        uint64      page_hash;

        buffer = ReadBufferExtended(idxRel, MAIN_FORKNUM, blkno,
                                    RBM_NORMAL, bstrategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);

        page_hash = index_page_hash(BufferGetPage(buffer), blkno, is_brin,
                                    algo);

        if (algo == CHECKSUM_ALGO_FNV32)
            checksum_accum_add(acc32, (uint32) page_hash);
        else
            checksum_accum64_add(acc64, page_hash);

        UnlockReleaseBuffer(buffer);

        if ((blkno & (SCAN_INTERRUPT_INTERVAL - 1)) == 0)
            CHECK_FOR_INTERRUPTS();
    }
}

/*
 * scan_index_blocks_physical - physical checksum over a range of index blocks
 */
void
scan_index_blocks_physical(Relation idxRel, BlockNumber start, BlockNumber end,
                           bool is_brin, ChecksumAccum *acc,
                           BufferAccessStrategy bstrategy)
{
    scan_index_blocks_physical_common(idxRel, start, end, is_brin,
                                      CHECKSUM_ALGO_FNV32, acc, NULL,
                                      bstrategy);
}

/*
 * scan_index_blocks_physical64 - 64-bit physical checksum over index blocks
 */
void
scan_index_blocks_physical64(Relation idxRel, BlockNumber start,
                             BlockNumber end, bool is_brin,
                             ChecksumAccum64 *acc,
                             BufferAccessStrategy bstrategy)
{
    scan_index_blocks_physical_common(idxRel, start, end, is_brin,
                                      CHECKSUM_ALGO_FNV64, NULL, acc,
                                      bstrategy);
}
