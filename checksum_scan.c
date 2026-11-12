/*-------------------------------------------------------------------------
 *
 * checksum_scan.c
 *    Serial block-range scans shared by serial and parallel execution
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

/*-------------------------------------------------------------------------
 * Serial block-range scans
 *-------------------------------------------------------------------------
 */

/*
 * scan_table_blocks_physical - physical checksum over a range of heap blocks
 *
 * Iterates the raw pages of the relation, applying the same visibility
 * rules as a sequential scan so that the resulting tuple set is identical
 * to what the serial path would have seen.
 */
void
scan_table_blocks_physical(Relation rel, BlockNumber start, BlockNumber end,
                           bool include_header, Snapshot snapshot,
                           ChecksumAccum *acc,
                           BufferAccessStrategy bstrategy)
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
            if ((blkno & 63) == 0)
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
            uint32      tuple_checksum;

            if (!ItemIdIsNormal(lp))
                continue;

            if (!pg_integrity_checksums_item_is_sane(page, lp))
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("corrupt line pointer in relation \"%s\" at block %u, offset %u",
                                RelationGetRelationName(rel), blkno, offnum)));

            htup = (HeapTupleHeader) PageGetItem(page, lp);
            tuple_len = ItemIdGetLength(lp);

            pg_integrity_checksums_check_heap_tuple(rel, blkno, offnum, htup, tuple_len);

            tuple.t_len = tuple_len;
            tuple.t_data = htup;
            tuple.t_tableOid = RelationGetRelid(rel);
            ItemPointerSet(&tuple.t_self, blkno, offnum);

            if (!HeapTupleSatisfiesVisibility(&tuple, snapshot, buffer))
                continue;

            tuple_checksum = pg_tuple_physical_checksum_internal(rel, page,
                                                                 offnum, blkno,
                                                                 include_header,
                                                                 false, CHECKSUM_ALGO_FNV32);
            if (tuple_checksum != 0)
                checksum_accum_add(acc, tuple_checksum);
        }

        UnlockReleaseBuffer(buffer);

        if ((blkno & 63) == 0)
            CHECK_FOR_INTERRUPTS();
    }

}

void
scan_table_blocks_physical64(Relation rel, BlockNumber start, BlockNumber end,
                           bool include_header, Snapshot snapshot,
                           ChecksumAccum64 *acc,
                           BufferAccessStrategy bstrategy)
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
            if ((blkno & 63) == 0)
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
            uint64      tuple_checksum;

            if (!ItemIdIsNormal(lp))
                continue;

            if (!pg_integrity_checksums_item_is_sane(page, lp))
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("corrupt line pointer in relation \"%s\" at block %u, offset %u",
                                RelationGetRelationName(rel), blkno, offnum)));

            htup = (HeapTupleHeader) PageGetItem(page, lp);
            tuple_len = ItemIdGetLength(lp);

            pg_integrity_checksums_check_heap_tuple(rel, blkno, offnum, htup, tuple_len);

            tuple.t_len = tuple_len;
            tuple.t_data = htup;
            tuple.t_tableOid = RelationGetRelid(rel);
            ItemPointerSet(&tuple.t_self, blkno, offnum);

            if (!HeapTupleSatisfiesVisibility(&tuple, snapshot, buffer))
                continue;

            tuple_checksum = pg_tuple_physical_checksum_internal(rel, page,
                                                                 offnum, blkno,
                                                                 include_header,
                                                                 false, CHECKSUM_ALGO_FNV64);
            if (tuple_checksum != 0)
                checksum_accum64_add(acc, tuple_checksum);
        }

        UnlockReleaseBuffer(buffer);

        if ((blkno & 63) == 0)
            CHECK_FOR_INTERRUPTS();
    }

}

/*
 * scan_table_blocks_logical - logical checksum over a range of heap blocks
 */
void
scan_table_blocks_logical(Relation rel, BlockNumber start, BlockNumber end,
                          Snapshot snapshot, ChecksumLogicalAccum *acc,
                          BufferAccessStrategy bstrategy)
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
            if ((blkno & 63) == 0)
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
            uint32      tuple_checksum;

            if (!ItemIdIsNormal(lp))
                continue;

            if (!pg_integrity_checksums_item_is_sane(page, lp))
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("corrupt line pointer in relation \"%s\" at block %u, offset %u",
                                RelationGetRelationName(rel), blkno, offnum)));

            htup = (HeapTupleHeader) PageGetItem(page, lp);
            tuple_len = ItemIdGetLength(lp);

            pg_integrity_checksums_check_heap_tuple(rel, blkno, offnum, htup, tuple_len);

            tuple.t_len = tuple_len;
            tuple.t_data = htup;
            tuple.t_tableOid = RelationGetRelid(rel);
            ItemPointerSet(&tuple.t_self, blkno, offnum);

            if (!HeapTupleSatisfiesVisibility(&tuple, snapshot, buffer))
                continue;

            tuple_checksum = pg_tuple_logical_checksum_internal(rel, &tuple,
                                                                  CHECKSUM_ALGO_FNV32);
            logical_accum_add(acc, tuple_checksum);
        }

        UnlockReleaseBuffer(buffer);

        if ((blkno & 63) == 0)
            CHECK_FOR_INTERRUPTS();
    }

}

/*
 * scan_table_blocks_logical64 - 64-bit logical checksum over heap blocks
 *
 * Mirrors scan_table_blocks_logical; the canonical per-row hash is selected
 * by passing algorithm 1 to the shared tuple implementation.
 */
void
scan_table_blocks_logical64(Relation rel, BlockNumber start, BlockNumber end,
                            Snapshot snapshot, ChecksumLogicalAccum64 *acc,
                            BufferAccessStrategy bstrategy)
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
            if ((blkno & 63) == 0)
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
            uint64      tuple_checksum;

            if (!ItemIdIsNormal(lp))
                continue;

            if (!pg_integrity_checksums_item_is_sane(page, lp))
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("corrupt line pointer in relation \"%s\" at block %u, offset %u",
                                RelationGetRelationName(rel), blkno, offnum)));

            htup = (HeapTupleHeader) PageGetItem(page, lp);
            tuple_len = ItemIdGetLength(lp);

            pg_integrity_checksums_check_heap_tuple(rel, blkno, offnum, htup, tuple_len);

            tuple.t_len = tuple_len;
            tuple.t_data = htup;
            tuple.t_tableOid = RelationGetRelid(rel);
            ItemPointerSet(&tuple.t_self, blkno, offnum);

            if (!HeapTupleSatisfiesVisibility(&tuple, snapshot, buffer))
                continue;

            tuple_checksum = pg_tuple_logical_checksum_internal(rel, &tuple,
                                                                  CHECKSUM_ALGO_FNV64);
            logical_accum_add64(acc, tuple_checksum);
        }

        UnlockReleaseBuffer(buffer);

        if ((blkno & 63) == 0)
            CHECK_FOR_INTERRUPTS();
    }
}

/*
 * scan_index_blocks_physical - physical checksum over a range of index blocks
 *
 * Handles both generic index types and BRIN, preserving the per-page hash
 * computation used by the serial path.
 */
void
scan_index_blocks_physical(Relation idxRel, BlockNumber start, BlockNumber end,
                           bool is_brin, ChecksumAccum *acc,
                           BufferAccessStrategy bstrategy)
{
    BlockNumber blkno;
    Size        page_size;

    for (blkno = start; blkno < end; blkno++)
    {
        Buffer      buffer;
        Page        page;
        uint32      page_hash;

        buffer = ReadBufferExtended(idxRel, MAIN_FORKNUM, blkno,
                                    RBM_NORMAL, bstrategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);

        page = BufferGetPage(buffer);

        if (!PageIsNew(page))
        {
            PageHeader phdr = (PageHeader) page;

            /*
             * Always hash a complete block.  PageGetPageSize() comes from
             * the page header and a corrupted value could make us read past
             * the buffer.
             */
            page_size = BLCKSZ;
            page_hash = fnv1a_32_hash((char *) page, page_size, 0);

            if (is_brin)
            {
                uint8      *special_space;
                Size        special_size;

                page_hash = combine_checksums(page_hash, (uint32) page_size);

                /*
                 * The BRIN special area can only be inspected when the page
                 * header is sane; the raw page bytes are still hashed above.
                 */
                if (pg_integrity_checksums_page_header_sane(page))
                {
                    special_space = (uint8 *) PageGetSpecialPointer(page);
                    special_size = PageGetSpecialSize(page);
                    if (special_size >= 4)
                    {
                        uint32      brin_info = 0;

                        memcpy(&brin_info, special_space, 4);
                        page_hash = combine_checksums(page_hash, brin_info);
                    }
                }

                page_hash = combine_checksums(page_hash, blkno);
                page_hash = combine_checksums(page_hash, phdr->pd_lower);
                page_hash = combine_checksums(page_hash, phdr->pd_upper);
                page_hash = combine_checksums(page_hash, phdr->pd_flags);
            }
            else
            {
                page_hash = combine_checksums(page_hash, phdr->pd_lower);
                page_hash = combine_checksums(page_hash, phdr->pd_upper);
                page_hash = combine_checksums(page_hash, phdr->pd_special);
                page_hash = combine_checksums(page_hash, blkno);
                page_hash = combine_checksums(page_hash, phdr->pd_flags);
                page_hash = combine_checksums(page_hash, (uint32) page_size);
            }

            checksum_accum_add(acc, page_hash);
        }
        else
        {
            page_hash = fnv1a_32_hash(is_brin ? "BRIN_NEW" : "NEW_PAGE", 8, blkno);
            page_hash = combine_checksums(page_hash, (uint32) PageGetPageSize(page));
            checksum_accum_add(acc, page_hash);
        }

        UnlockReleaseBuffer(buffer);

        if ((blkno & 63) == 0)
            CHECK_FOR_INTERRUPTS();
    }

}

void
scan_index_blocks_physical64(Relation idxRel, BlockNumber start, BlockNumber end,
                           bool is_brin, ChecksumAccum64 *acc,
                           BufferAccessStrategy bstrategy)
{
    BlockNumber blkno;
    Size        page_size;

    for (blkno = start; blkno < end; blkno++)
    {
        Buffer      buffer;
        Page        page;
        uint64      page_hash;

        buffer = ReadBufferExtended(idxRel, MAIN_FORKNUM, blkno,
                                    RBM_NORMAL, bstrategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);

        page = BufferGetPage(buffer);

        if (!PageIsNew(page))
        {
            PageHeader phdr = (PageHeader) page;

            /*
             * Always hash a complete block.  PageGetPageSize() comes from
             * the page header and a corrupted value could make us read past
             * the buffer.
             */
            page_size = BLCKSZ;
            page_hash = fnv1a_64_hash((char *) page, page_size, 0);

            if (is_brin)
            {
                uint8      *special_space;
                Size        special_size;

                page_hash = combine_checksums_64(page_hash, (uint32) page_size);

                /*
                 * The BRIN special area can only be inspected when the page
                 * header is sane; the raw page bytes are still hashed above.
                 */
                if (pg_integrity_checksums_page_header_sane(page))
                {
                    special_space = (uint8 *) PageGetSpecialPointer(page);
                    special_size = PageGetSpecialSize(page);
                    if (special_size >= 4)
                    {
                        uint32      brin_info = 0;

                        memcpy(&brin_info, special_space, 4);
                        page_hash = combine_checksums_64(page_hash, brin_info);
                    }
                }

                page_hash = combine_checksums_64(page_hash, blkno);
                page_hash = combine_checksums_64(page_hash, phdr->pd_lower);
                page_hash = combine_checksums_64(page_hash, phdr->pd_upper);
                page_hash = combine_checksums_64(page_hash, phdr->pd_flags);
            }
            else
            {
                page_hash = combine_checksums_64(page_hash, phdr->pd_lower);
                page_hash = combine_checksums_64(page_hash, phdr->pd_upper);
                page_hash = combine_checksums_64(page_hash, phdr->pd_special);
                page_hash = combine_checksums_64(page_hash, blkno);
                page_hash = combine_checksums_64(page_hash, phdr->pd_flags);
                page_hash = combine_checksums_64(page_hash, (uint32) page_size);
            }

            checksum_accum64_add(acc, page_hash);
        }
        else
        {
            page_hash = fnv1a_64_hash(is_brin ? "BRIN_NEW" : "NEW_PAGE", 8, blkno);
            page_hash = combine_checksums_64(page_hash, (uint32) PageGetPageSize(page));
            checksum_accum64_add(acc, page_hash);
        }

        UnlockReleaseBuffer(buffer);

        if ((blkno & 63) == 0)
            CHECK_FOR_INTERRUPTS();
    }

}

