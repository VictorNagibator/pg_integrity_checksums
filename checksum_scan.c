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
#include "storage/predicate.h"
#if PG_VERSION_NUM >= 170000
#include "storage/read_stream.h"
#endif
#include "utils/rel.h"

#include "checksum_internal.h"

/* Check for interrupts once per this many blocks (must be a power of two). */
#define SCAN_INTERRUPT_INTERVAL 64

/*-------------------------------------------------------------------------
 * Block sources and readers
 *-------------------------------------------------------------------------
 */

/*
 * checksum_block_source_range - a source of the blocks [start, end)
 */
void
checksum_block_source_range(ChecksumBlockSource *src, BlockNumber start,
                            BlockNumber end)
{
    src->next = start;
    src->end = end;
    src->pstate = NULL;
}

/*
 * checksum_block_source_shared - a source of the chunks of blocks this
 * participant claims from the shared counter of a parallel scan
 */
void
checksum_block_source_shared(ChecksumBlockSource *src,
                             ChecksumParallelState *pstate)
{
    src->next = 0;
    src->end = 0;
    src->pstate = pstate;
}

/*
 * block_source_next - the next block of a source, or InvalidBlockNumber
 *
 * Called from the read stream callback, so it must not block or error out;
 * claiming a chunk is a single atomic fetch-and-add.
 */
static BlockNumber
block_source_next(ChecksumBlockSource *src)
{
    if (src->next >= src->end)
    {
        uint32      start;
        uint32      end;

        if (src->pstate == NULL ||
            !pg_integrity_checksums_parallel_consume(src->pstate,
                                                     src->pstate->block_chunk,
                                                     &start, &end))
            return InvalidBlockNumber;

        src->next = start;
        src->end = end;
    }

    return src->next++;
}

#if PG_VERSION_NUM >= 170000
static BlockNumber
block_source_stream_cb(ReadStream *stream, void *callback_private_data,
                       void *per_buffer_data)
{
    return block_source_next((ChecksumBlockSource *) callback_private_data);
}
#endif

/*
 * checksum_block_reader_begin - start reading the blocks of a source
 *
 * The caller keeps "src" and "bstrategy" alive until
 * checksum_block_reader_end().
 */
void
checksum_block_reader_begin(ChecksumBlockReader *reader, Relation rel,
                            ChecksumBlockSource *src,
                            BufferAccessStrategy bstrategy)
{
    reader->rel = rel;
    reader->src = src;
    reader->bstrategy = bstrategy;
    reader->stream = NULL;

#if PG_VERSION_NUM >= 170000
    {
        int         flags = READ_STREAM_SEQUENTIAL;

#ifdef READ_STREAM_USE_BATCHING
        /* The callback never blocks, as batch mode requires. */
        flags |= READ_STREAM_USE_BATCHING;
#endif
        reader->stream = read_stream_begin_relation(flags, bstrategy, rel,
                                                    MAIN_FORKNUM,
                                                    block_source_stream_cb,
                                                    src, 0);
    }
#endif
}

/*
 * checksum_block_reader_next - the next block, pinned, or InvalidBuffer
 */
Buffer
checksum_block_reader_next(ChecksumBlockReader *reader)
{
#if PG_VERSION_NUM >= 170000
    return read_stream_next_buffer(reader->stream, NULL);
#else
    BlockNumber blkno = block_source_next(reader->src);

    if (blkno == InvalidBlockNumber)
        return InvalidBuffer;
    return ReadBufferExtended(reader->rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
                              reader->bstrategy);
#endif
}

/*
 * checksum_block_reader_end - release the reader
 *
 * Must only be called after checksum_block_reader_next() returned
 * InvalidBuffer.  After an error the pins are released by the resource
 * owner, like those of an aborted sequential scan.
 */
void
checksum_block_reader_end(ChecksumBlockReader *reader)
{
#if PG_VERSION_NUM >= 170000
    read_stream_end(reader->stream);
    reader->stream = NULL;
#endif
}

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
 *
 * Validation and visibility checks always run under a share lock.  With
 * needs_lock the visitor runs under that lock too.  Without it the scan
 * works like heapgetpage() in page-at-a-time mode: it remembers the visible
 * tuples, releases the content lock but keeps the pin, and then calls the
 * visitor.  This is required for visitors that detoast values, call type
 * send functions or evaluate index expressions: such code reads other
 * buffers and the catalogs and may run user-defined functions, and doing it
 * under a buffer content lock delays query cancellation (interrupts are held
 * off) and risks an undetectable LWLock deadlock.
 */
typedef struct VisibleTuple
{
    OffsetNumber offnum;
    HeapTupleHeader htup;
    uint32      len;
} VisibleTuple;

void
scan_visible_tuples(Relation rel, ChecksumBlockSource *src,
                    Snapshot snapshot, BufferAccessStrategy bstrategy,
                    bool needs_lock, TupleVisitor visitor, void *arg)
{
    ChecksumBlockReader reader;
    Buffer      buffer;
    VisibleTuple *visible = NULL;

    if (!needs_lock)
        visible = palloc(sizeof(VisibleTuple) * MaxHeapTuplesPerPage);

    /*
     * Like a sequential scan, a checksum reads the whole relation: in a
     * SERIALIZABLE transaction take a relation-level SIREAD lock, so a
     * concurrent write to the relation is detected as a read-write conflict.
     * A no-op at other isolation levels.
     */
    PredicateLockRelation(rel, snapshot);

    checksum_block_reader_begin(&reader, rel, src, bstrategy);

    while ((buffer = checksum_block_reader_next(&reader)) != InvalidBuffer)
    {
        BlockNumber blkno = BufferGetBlockNumber(buffer);
        Page        page;
        OffsetNumber maxoff;
        OffsetNumber offnum;
        bool        all_visible;
        int         nvisible = 0;
        int         i;

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

        /*
         * On an all-visible page every tuple is visible to every MVCC
         * snapshot, so the per-tuple visibility check can be skipped, as
         * heapgetpage() does.  A snapshot taken during recovery cannot rely
         * on the flag: it may be set ahead of what the standby has replayed.
         */
        all_visible = PageIsAllVisible(page) &&
            snapshot->snapshot_type == SNAPSHOT_MVCC &&
            !snapshot->takenDuringRecovery;

        maxoff = PageGetMaxOffsetNumber(page);
        for (offnum = FirstOffsetNumber; offnum <= maxoff;
             offnum = OffsetNumberNext(offnum))
        {
            ItemId      lp = PageGetItemId(page, offnum);
            HeapTupleHeader htup;
            HeapTupleData tuple;
            Size        tuple_len;
            bool        valid;

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

            valid = all_visible ||
                HeapTupleSatisfiesVisibility(&tuple, snapshot, buffer);
            pg_integrity_checksums_serializable_conflict_out(valid, rel,
                                                             &tuple, buffer,
                                                             snapshot);
            if (!valid)
                continue;

            if (needs_lock)
            {
                visitor(rel, page, blkno, offnum, &tuple, arg);
                continue;
            }

            /* More visible tuples than a heap page can hold: not a heap page. */
            if (nvisible >= MaxHeapTuplesPerPage)
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("too many tuples in relation \"%s\" at block %u",
                                RelationGetRelationName(rel), blkno)));

            visible[nvisible].offnum = offnum;
            visible[nvisible].htup = htup;
            visible[nvisible].len = (uint32) tuple_len;
            nvisible++;
        }

        if (needs_lock)
        {
            UnlockReleaseBuffer(buffer);
        }
        else
        {
            /*
             * The pin keeps the tuples where they are: pruning and
             * defragmentation need a cleanup lock, which our pin blocks.
             */
            LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

            for (i = 0; i < nvisible; i++)
            {
                HeapTupleData tuple;

                tuple.t_len = visible[i].len;
                tuple.t_data = visible[i].htup;
                tuple.t_tableOid = RelationGetRelid(rel);
                ItemPointerSet(&tuple.t_self, blkno, visible[i].offnum);

                visitor(rel, page, blkno, visible[i].offnum, &tuple, arg);

                CHECK_FOR_INTERRUPTS();
            }

            ReleaseBuffer(buffer);
        }

        if ((blkno & (SCAN_INTERRUPT_INTERVAL - 1)) == 0)
            CHECK_FOR_INTERRUPTS();
    }

    checksum_block_reader_end(&reader);

    if (visible != NULL)
        pfree(visible);
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

    if (checksum_algo_is32(state->algo))
        checksum_accum_add(state->acc32, (uint32) tuple_checksum);
    else
        checksum_accum64_add(state->acc64, tuple_checksum);
}

/*
 * scan_table_blocks_physical - physical checksum over heap blocks
 */
void
scan_table_blocks_physical(Relation rel, ChecksumBlockSource *src,
                           bool include_header, Snapshot snapshot,
                           ChecksumAlgo algo, ChecksumAccum *acc32,
                           ChecksumAccum64 *acc64,
                           BufferAccessStrategy bstrategy)
{
    PhysicalVisitorState state = {include_header, algo, acc32, acc64};

    /* Reads xmax and the infomask, which concurrent backends change. */
    scan_visible_tuples(rel, src, snapshot, bstrategy, true,
                        physical_tuple_visitor, &state);
}

/* State of a logical heap scan; only the accumulator for "algo" is used. */
typedef struct LogicalVisitorState
{
    ChecksumAlgo algo;
    ChecksumLogicalAccum *acc32;
    ChecksumLogicalAccum64 *acc64;
    LogicalRowHasher hasher;
} LogicalVisitorState;

static void
logical_tuple_visitor(Relation rel, Page page, BlockNumber blkno,
                      OffsetNumber offnum, HeapTuple tuple, void *arg)
{
    LogicalVisitorState *state = (LogicalVisitorState *) arg;
    uint64      tuple_checksum;

    tuple_checksum = logical_row_hash(&state->hasher, tuple, state->algo);

    if (checksum_algo_is32(state->algo))
        logical_accum_add(state->acc32, (uint32) tuple_checksum);
    else
        logical_accum_add64(state->acc64, tuple_checksum);
}

/*
 * scan_table_blocks_logical - logical checksum over heap blocks
 */
void
scan_table_blocks_logical(Relation rel, ChecksumBlockSource *src,
                          Snapshot snapshot, ChecksumAlgo algo,
                          ChecksumLogicalAccum *acc32,
                          ChecksumLogicalAccum64 *acc64,
                          BufferAccessStrategy bstrategy,
                          TupleDesc order_desc)
{
    LogicalVisitorState state;

    state.algo = algo;
    state.acc32 = acc32;
    state.acc64 = acc64;
    logical_row_hasher_init(&state.hasher, RelationGetDescr(rel), order_desc);

    scan_visible_tuples(rel, src, snapshot, bstrategy, false,
                        logical_tuple_visitor, &state);

    logical_row_hasher_free(&state.hasher);
}

/*-------------------------------------------------------------------------
 * Index scans
 *-------------------------------------------------------------------------
 */

/*
 * index_page_hash - hash one masked index page together with its header
 *
 * Handles both generic index types and BRIN.  The page is hashed as a
 * complete block of BLCKSZ bytes: PageGetPageSize() comes from the page
 * header and a corrupted value could make us read past the buffer.
 *
 * "masked" is a private copy made by pg_integrity_checksums_mask_page(), not
 * the buffer itself: the stale in-buffer pd_checksum, pd_lsn and the LP_DEAD
 * and "has garbage" hints set by index scans used to change the checksum
 * after a plain SELECT or a server restart.  Hashing the copy also means the
 * buffer content lock is only held for the memcpy, not for the hash.
 */
static uint64
index_page_hash(const char *masked, BlockNumber blkno, bool is_brin,
                ChecksumAlgo algo)
{
    Page        page = (Page) masked;
    PageHeader  phdr = (PageHeader) masked;
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
 * scan_index_blocks_physical - physical checksum over index blocks
 *
 * Only the accumulator that matches "algo" is used.
 */
void
scan_index_blocks_physical(Relation idxRel, ChecksumBlockSource *src,
                           bool is_brin,
                           ChecksumAlgo algo, ChecksumAccum *acc32,
                           ChecksumAccum64 *acc64,
                           BufferAccessStrategy bstrategy)
{
    ChecksumBlockReader reader;
    Buffer      buffer;
    Oid         relam = idxRel->rd_rel->relam;
    PGAlignedBlock masked;

    checksum_block_reader_begin(&reader, idxRel, src, bstrategy);

    while ((buffer = checksum_block_reader_next(&reader)) != InvalidBuffer)
    {
        BlockNumber blkno = BufferGetBlockNumber(buffer);
        uint64      page_hash;

        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        pg_integrity_checksums_mask_page(BufferGetPage(buffer), masked.data,
                                         CHECKSUM_PAGE_INDEX, relam);
        UnlockReleaseBuffer(buffer);

        page_hash = index_page_hash(masked.data, blkno, is_brin, algo);

        if (checksum_algo_is32(algo))
            checksum_accum_add(acc32, (uint32) page_hash);
        else
            checksum_accum64_add(acc64, page_hash);

        if ((blkno & (SCAN_INTERRUPT_INTERVAL - 1)) == 0)
            CHECK_FOR_INTERRUPTS();
    }

    checksum_block_reader_end(&reader);
}
