/*-------------------------------------------------------------------------
 *
 * checksum_internal.h
 *    Internal interfaces shared by the checksum implementation modules
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_INTEGRITY_CHECKSUMS_INTERNAL_H
#define PG_INTEGRITY_CHECKSUMS_INTERNAL_H

#include "postgres.h"
#include "pg_integrity_checksums.h"

#include "access/xact.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "port/atomics.h"
#include "storage/bufmgr.h"
#include "storage/dsm.h"
#include "storage/shm_toc.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/*
 * lnext() gained the list argument in PostgreSQL 13.  Keep the extension
 * buildable on PostgreSQL 12.
 */
#if PG_VERSION_NUM < 130000
#define lnext_compat(list, cell) lnext(cell)
#else
#define lnext_compat(list, cell) lnext(list, cell)
#endif

/* Extension entry point (declared to keep -Wmissing-prototypes quiet). */
extern void _PG_init(void);

/* GUC: fail instead of skipping an unsupported relation. */
extern bool pg_integrity_checksums_strict;

/*
 * ChecksumAccum - order-independent aggregate of 32-bit physical hashes
 *
 * Each hash is mixed and XORed; the count distinguishes an empty scan from
 * a non-empty one.
 */
typedef struct ChecksumAccum
{
    uint32      partial;
    uint64      count;
} ChecksumAccum;

/* 64-bit variant of ChecksumAccum. */
typedef struct ChecksumAccum64
{
    uint64      partial;
    uint64      count;
} ChecksumAccum64;

/*
 * ChecksumLogicalAccum - sum-and-count aggregate for logical checksums
 *
 * Addition preserves duplicate rows while remaining order-independent.
 */
typedef struct ChecksumLogicalAccum
{
    uint64      sum;
    uint64      count;
} ChecksumLogicalAccum;

/* 64-bit variant of ChecksumLogicalAccum. */
typedef struct ChecksumLogicalAccum64
{
    uint64      sum;
    uint64      count;
} ChecksumLogicalAccum64;

/*
 * ChecksumTaskType - identifies which checksum a parallel scan computes
 */
typedef enum ChecksumTaskType
{
    CHECKSUM_TASK_TABLE_PHYSICAL = 1,
    CHECKSUM_TASK_TABLE_LOGICAL,
    CHECKSUM_TASK_INDEX_PHYSICAL,
    CHECKSUM_TASK_INDEX_LOGICAL,
    CHECKSUM_TASK_DATABASE_PHYSICAL,
    CHECKSUM_TASK_DATABASE_LOGICAL
} ChecksumTaskType;

/*
 * ChecksumRelationItem - a single relation processed during a database scan
 */
typedef struct ChecksumRelationItem
{
    Oid         reloid;
    char        relkind;
} ChecksumRelationItem;

/*
 * IndexLogicalScanState - executor state shared by a block range scan
 */
typedef struct IndexLogicalScanState
{
    Relation    idxRel;
    Relation    heapRel;
    IndexInfo  *indexInfo;
    EState     *estate;
    ExprContext *econtext;
    ExprState  *predicate;
    TupleTableSlot *slot;
    MemoryContext context;
    Snapshot    snapshot;
    BufferAccessStrategy bstrategy;
    Datum      *values;
    bool       *nulls;
    Oid        *inputtypid;
    int32      *inputtypmod;
    int         nattrs;
} IndexLogicalScanState;

/*
 * ChecksumWorkerResult - fixed-size result returned by each worker
 *
 * Each worker writes a single entry of this type into shared memory, so
 * no streaming or variable-sized shared memory is required. 32-bit scans
 * zero-extend their aggregate; 64-bit scans store it directly.  had_error is
 * set by database scans when one or more relations had to be skipped.
 */
typedef struct ChecksumWorkerResult
{
    uint64      partial;
    uint64      count;
    bool        had_error;
} ChecksumWorkerResult;

/*
 * ChecksumParallelState - shared state for a parallel checksum scan
 *
 * This structure lives in dynamic shared memory and is used both by the
 * leader and by the parallel workers. nitems holds the number of items
 * (blocks for table/index scans, relations for database scans) and
 * next_item is the atomic counter used to claim work in a dynamic,
 * self-balancing fashion.
 */
typedef struct ChecksumParallelState
{
    ChecksumTaskType task_type;
    Oid         reloid;
    bool        include_header;
    int         algo;
    uint32      nitems;
    uint32      snapshot_size;
    pg_atomic_uint32 next_item;
} ChecksumParallelState;

/* Keys for the shm_toc used by the parallel scan. */
#define CHECKSUM_KEY_STATE       0
#define CHECKSUM_KEY_SNAPSHOT    1
#define CHECKSUM_KEY_RESULTS     2
#define CHECKSUM_KEY_ITEMS       3

/* Number of blocks claimed per atomic fetch during a parallel scan. */
#define PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK 64

/* Parallel worker entry point (must be a globally visible symbol). */
PGDLLEXPORT void
pg_integrity_checksums_parallel_worker_main(dsm_segment *seg, shm_toc *toc);

/* Parallel execution and shared scan configuration. */
extern int pg_integrity_checksums_worker_count(void);
extern Snapshot pg_integrity_checksums_get_snapshot(void);
extern void pg_integrity_checksums_parallel_scan_blocks(ChecksumTaskType task_type,
                                                        Oid reloid,
                                                        bool include_header,
                                                        uint32 nblocks,
                                                        int workers, int algo,
                                                        ChecksumAccum *acc,
                                                        ChecksumLogicalAccum *logical_acc,
                                                        ChecksumAccum64 *acc64,
                                                        ChecksumLogicalAccum64 *logical_acc64);
extern void pg_integrity_checksums_parallel_scan_database(bool physical,
                                                           ChecksumRelationItem *items,
                                                           uint32 nitems,
                                                           int workers, int algo,
                                                           ChecksumAccum64 *acc,
                                                           ChecksumLogicalAccum *logical_acc,
                                                           ChecksumLogicalAccum64 *logical_acc64,
                                                           bool *had_error);

/* FNV-1a helpers. */
extern uint32 fnv1a_32_buf(const unsigned char *bytes, size_t len, uint32 hash);
extern uint32 fnv1a_32_hash(const void *data, size_t len, uint32 seed);
extern uint32 pg_checksum_data_custom(const char *data, uint32 len,
                                       uint32 init_value);
extern uint64 fnv1a_64_buf(const unsigned char *bytes, size_t len, uint64 hash);
extern uint64 fnv1a_64_hash(const void *data, size_t len, uint64 seed);

/* SQL argument validation/encoding shared by the hash entry points. */
extern int checksum_algorithm(text *value);
extern bytea *checksum_bytea(uint64 value, int len);

/* Hash combination and order-independent aggregation. */
extern uint32 combine_checksums(uint32 current, uint32 new_val);
extern uint64 combine_checksums_64(uint64 current, uint64 new_val);
extern uint32 hash_mix32(uint32 h);
extern uint64 hash_mix64(uint64 h);
extern void checksum_accum_add(ChecksumAccum *acc, uint32 h);
extern void checksum_accum_merge(ChecksumAccum *acc, uint64 partial,
                                 uint64 count);
extern uint32 checksum_accum_finalize(ChecksumAccum *acc);
extern void checksum_accum64_add(ChecksumAccum64 *acc, uint64 h);
extern void checksum_accum64_merge(ChecksumAccum64 *acc, uint64 partial,
                                   uint64 count);
extern uint64 checksum_accum64_finalize(ChecksumAccum64 *acc);
extern void logical_accum_add(ChecksumLogicalAccum *acc, uint32 h);
extern uint32 logical_accum_finalize(ChecksumLogicalAccum *acc);
extern void logical_accum_add64(ChecksumLogicalAccum64 *acc, uint64 h);
extern uint64 logical_accum_finalize64(ChecksumLogicalAccum64 *acc);

/* Index checksum implementations shared with the parallel scan. */
extern uint32 pg_index_physical_checksum_calc(Oid indexoid,
                                              bool check_privileges,
                                              bool *is_null);
extern uint64 pg_index_physical_checksum_calc64(Oid indexoid,
                                                bool check_privileges,
                                                bool *is_null);
extern uint32 pg_index_logical_checksum_calc(Oid indexoid,
                                             bool check_privileges,
                                             bool *is_null);
extern uint64 pg_index_logical_checksum_calc64(Oid indexoid,
                                               bool check_privileges,
                                               bool *is_null);
extern bool pg_index_logical_scan_init(Oid indexoid, Snapshot snapshot,
                                       IndexLogicalScanState *scanstate);
extern void pg_index_logical_scan_blocks(IndexLogicalScanState *scanstate,
                                         BlockNumber start, BlockNumber end,
                                         ChecksumLogicalAccum *acc);
extern void pg_index_logical_scan_blocks64(IndexLogicalScanState *scanstate,
                                           BlockNumber start, BlockNumber end,
                                           ChecksumLogicalAccum64 *acc);
extern void pg_index_logical_scan_end(IndexLogicalScanState *scanstate);

/* Database relation-range aggregation shared with the parallel scan. */
extern void compute_database_items(ChecksumRelationItem *items, uint32 start,
                                   uint32 end, bool physical, int algo,
                                   ChecksumAccum64 *acc,
                                   ChecksumLogicalAccum *logical_acc,
                                   ChecksumLogicalAccum64 *logical_acc64,
                                   bool *had_error);

/* Table checksum implementations shared with the database scan. */
extern uint64 pg_table_physical_checksum_calc(Oid reloid,
                                              bool include_header,
                                              bool check_privileges);
extern uint64 pg_table_physical_checksum_calc64(Oid reloid,
                                                bool include_header,
                                                bool check_privileges);
extern int64 pg_table_logical_checksum_calc(Oid reloid,
                                            bool check_privileges,
                                            bool *is_null);
extern uint64 pg_table_logical_checksum_calc64(Oid reloid,
                                               bool check_privileges,
                                               bool *is_null);

/* Partition helpers shared by table and index checksums. */
extern List *pg_integrity_checksums_leaf_indexes(Oid indexoid);

/* Relation, page and tuple validation. */
extern void pg_integrity_checksums_require_select(Oid reloid);
extern void pg_integrity_checksums_check_relation_kind(Relation rel,
                                                       bool allow_toast,
                                                       bool allow_partitioned);
extern bool pg_integrity_checksums_relation_has_storage(Relation rel);
extern bool pg_integrity_checksums_page_header_sane(Page page);
extern void pg_integrity_checksums_check_page(Relation rel, BlockNumber blkno,
                                               Page page);
extern bool pg_integrity_checksums_item_is_sane(Page page, ItemId lp);
extern void pg_integrity_checksums_check_heap_tuple(Relation rel,
                                                    BlockNumber blkno,
                                                    OffsetNumber offnum,
                                                    HeapTupleHeader tuple,
                                                    Size len);
extern void scan_relation_raw_pages(Relation rel, BlockNumber start,
                                     BlockNumber end, ChecksumAccum *acc);
extern void scan_relation_raw_pages64(Relation rel, BlockNumber start,
                                       BlockNumber end, ChecksumAccum64 *acc);

/* Tuple checksum cores, shared with the block scan and database modules. */
extern uint64 pg_tuple_physical_checksum_internal(Relation rel, Page page,
                                                  OffsetNumber offnum,
                                                  BlockNumber blkno,
                                                  bool include_header,
                                                  bool validate, int algo);
extern uint64 pg_tuple_logical_checksum_internal(Relation rel,
                                                 HeapTuple tuple, int algo);

/* Serial block-range scans. */
extern void scan_table_blocks_physical(Relation rel, BlockNumber start,
                                       BlockNumber end, bool include_header,
                                       Snapshot snapshot, ChecksumAccum *acc,
                                       BufferAccessStrategy bstrategy);
extern void scan_table_blocks_physical64(Relation rel, BlockNumber start,
                                         BlockNumber end, bool include_header,
                                         Snapshot snapshot,
                                         ChecksumAccum64 *acc,
                                         BufferAccessStrategy bstrategy);
extern void scan_table_blocks_logical(Relation rel, BlockNumber start,
                                      BlockNumber end, Snapshot snapshot,
                                      ChecksumLogicalAccum *acc,
                                      BufferAccessStrategy bstrategy);
extern void scan_table_blocks_logical64(Relation rel, BlockNumber start,
                                        BlockNumber end, Snapshot snapshot,
                                        ChecksumLogicalAccum64 *acc,
                                        BufferAccessStrategy bstrategy);
extern void scan_index_blocks_physical(Relation idxRel, BlockNumber start,
                                       BlockNumber end, bool is_brin,
                                       ChecksumAccum *acc,
                                       BufferAccessStrategy bstrategy);
extern void scan_index_blocks_physical64(Relation idxRel, BlockNumber start,
                                         BlockNumber end, bool is_brin,
                                         ChecksumAccum64 *acc,
                                         BufferAccessStrategy bstrategy);

/* Canonical logical encoding. */
extern uint64 combine_logical(uint64 current, uint64 new_val, int algo);
extern void logical_type_cache_invalidate(Datum arg, int cacheid,
                                          uint32 hashvalue);
extern uint32 pg_cell_checksum_canonical(Datum value, bool isnull, Oid typid,
                                         int32 typmod, int attnum);
extern uint64 pg_cell_checksum_canonical_algo(Datum value, bool isnull,
                                              Oid typid, int32 typmod,
                                              int attnum, int algo);

#endif /* PG_INTEGRITY_CHECKSUMS_INTERNAL_H */
