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

#include "access/htup_details.h"
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
 * ChecksumAlgo - hash algorithm selected by the SQL "algo" argument
 *
 * The numeric values are part of the shared-memory state layout only; they
 * are never exposed in SQL.
 */
typedef enum ChecksumAlgo
{
    CHECKSUM_ALGO_FNV32 = 0,
    CHECKSUM_ALGO_FNV64 = 1
} ChecksumAlgo;

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

    /*
     * Number of items, starting with this one, that are computed together:
     * a logical database checksum hashes a table and the indexes on it in a
     * single pass over the heap.  1 for an item processed on its own, 0 for
     * an index that the head of its group (the table) covers.
     */
    uint32      group_size;
} ChecksumRelationItem;

/* Opaque per-type cache entry; see checksum_types.c. */
typedef struct LogicalTypeInfo LogicalTypeInfo;

/*
 * LogicalColumnHint - per-column accelerator for logical value hashing
 *
 * Remembers the type cache entry of the last value hashed with it and the
 * hash prefix (type tag and position) of that value; see
 * pg_cell_checksum_canonical_hinted().  Zero-initialize before first use.
 */
typedef struct LogicalColumnHint
{
    LogicalTypeInfo *info;
    uint64      generation;     /* type cache generation of info */
    uint64      prefix;         /* tag prefix combined with attnum */
    int         attnum;
    ChecksumAlgo algo;
} LogicalColumnHint;

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
    LogicalColumnHint *hints;   /* hashing hint per index column */
    int         nattrs;
} IndexLogicalScanState;

/*
 * ChecksumWorkerResult - fixed-size result returned by each worker
 *
 * Each worker writes a single entry of this type into shared memory, so
 * no streaming or variable-sized shared memory is required. 32-bit scans
 * zero-extend their aggregate; 64-bit scans store it directly.  skipped is
 * the number of relations a database scan had to skip.
 */
typedef struct ChecksumWorkerResult
{
    uint64      partial;
    uint64      count;
    uint32      skipped;
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
    ChecksumAlgo         algo;
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
                                                        int workers, ChecksumAlgo algo,
                                                        ChecksumAccum *acc,
                                                        ChecksumLogicalAccum *logical_acc,
                                                        ChecksumAccum64 *acc64,
                                                        ChecksumLogicalAccum64 *logical_acc64);
extern void pg_integrity_checksums_parallel_scan_database(bool physical,
                                                           ChecksumRelationItem *items,
                                                           uint32 nitems,
                                                           int workers, ChecksumAlgo algo,
                                                           ChecksumAccum64 *acc,
                                                           ChecksumLogicalAccum *logical_acc,
                                                           ChecksumLogicalAccum64 *logical_acc64,
                                                           uint32 *skipped);

/*-------------------------------------------------------------------------
 * FNV-1a hashing and hash combination
 *
 * These primitives run several times per column of every row, so they are
 * defined here as static inline functions: calls across translation units
 * cannot be inlined by the compiler, and the call overhead used to be a
 * quarter of a logical table checksum.  Their results are part of stored
 * baselines and must never change.
 *-------------------------------------------------------------------------
 */

/*
 * fnv1a_32_buf - continue an FNV-1a hash over a byte range
 *
 * Unlike fnv1a_32_hash(), the incoming hash is used as-is (an all-zero value
 * is a legitimate state), so calls may be chained over several buffers and
 * still produce exactly what a single pass over their concatenation would.
 */
static inline uint32
fnv1a_32_buf(const unsigned char *bytes, size_t len, uint32 hash)
{
    for (size_t i = 0; i < len; i++)
    {
        hash ^= bytes[i];
        hash *= FNV_PRIME_32;
    }

    return hash;
}

/*
 * fnv1a_32_hash - FNV-1a 32-bit hash
 *
 * Standard FNV-1a: each input byte is XORed into the hash, which is then
 * multiplied by the FNV prime.  This is unrelated to PostgreSQL's own 16-bit
 * page checksum algorithm.  A zero seed is treated as "no seed" and selects
 * FNV_BASIS_32, so seed 0 and seed FNV_BASIS_32 give identical results.
 */
static inline uint32
fnv1a_32_hash(const void *data, size_t len, uint32 seed)
{
    return fnv1a_32_buf((const unsigned char *) data, len,
                        seed == 0 ? FNV_BASIS_32 : seed);
}

/* fnv1a_64_buf - continue an FNV-1a 64-bit hash over a byte range */
static inline uint64
fnv1a_64_buf(const unsigned char *bytes, size_t len, uint64 hash)
{
    for (size_t i = 0; i < len; i++)
    {
        hash ^= bytes[i];
        hash *= FNV_PRIME_64;
    }

    return hash;
}

/*
 * fnv1a_64_hash - FNV-1a 64-bit hash
 *
 * A zero seed selects the standard offset basis, matching the convention of
 * the 32-bit helper.
 */
static inline uint64
fnv1a_64_hash(const void *data, size_t len, uint64 seed)
{
    return fnv1a_64_buf((const unsigned char *) data, len,
                        seed == 0 ? FNV_BASIS_64 : seed);
}

/*
 * pg_checksum_data_custom - FNV-1a 32-bit hash with the zero-seed convention
 *
 * Thin wrapper used by the data/text checksum functions and by the physical
 * tuple checksum.
 */
static inline uint32
pg_checksum_data_custom(const char *data, uint32 len, uint32 init_value)
{
    return fnv1a_32_hash(data, len, init_value);
}

/*
 * hash_bytes_algo - FNV-1a over a byte range with the selected algorithm
 *
 * "seed" is the running hash (or 0 to start from the offset basis).  For
 * fnv32 the result is zero-extended.
 */
static inline uint64
hash_bytes_algo(uint64 seed, const void *data, size_t len, ChecksumAlgo algo)
{
    if (algo == CHECKSUM_ALGO_FNV32)
        return fnv1a_32_hash(data, len, (uint32) seed);

    return fnv1a_64_hash(data, len, seed);
}

/*
 * combine_checksums - combine two 32-bit hash values using FNV-1a
 *
 * Feeds the four bytes of new_val into the running hash, most significant
 * byte first.  The result depends on the order of the arguments.  Note that
 * combine_checksums_64() feeds its bytes least significant first; both byte
 * orders are part of the stored baseline values and must not change.
 */
static inline uint32
combine_checksums(uint32 current, uint32 new_val)
{
    uint32      hash = current;

    hash ^= (new_val >> 24) & 0xFF;
    hash *= FNV_PRIME_32;
    hash ^= (new_val >> 16) & 0xFF;
    hash *= FNV_PRIME_32;
    hash ^= (new_val >> 8) & 0xFF;
    hash *= FNV_PRIME_32;
    hash ^= new_val & 0xFF;
    hash *= FNV_PRIME_32;

    return hash;
}

/*
 * combine_checksums_64 - combine two 64-bit hash values using FNV-1a
 *
 * The bytes of new_val are fed least significant first.
 */
static inline uint64
combine_checksums_64(uint64 current, uint64 new_val)
{
    uint64      hash = current;

    for (int i = 0; i < 8; i++)
    {
        hash ^= (new_val >> (i * 8)) & 0xFF;
        hash *= FNV_PRIME_64;
    }

    return hash;
}

/*
 * combine_hash - order-dependent combination with the selected algorithm
 *
 * For fnv32 both operands are truncated to 32 bits.
 */
static inline uint64
combine_hash(uint64 current, uint64 new_val, ChecksumAlgo algo)
{
    if (algo == CHECKSUM_ALGO_FNV32)
        return combine_checksums((uint32) current, (uint32) new_val);

    return combine_checksums_64(current, new_val);
}

/*
 * hash_mix32 - 32-bit avalanche finalizer (MurmurHash3 fmix32)
 *
 * Spreads the bits of a hash before it is folded into an order-independent
 * aggregate, so that correlated inputs contribute uniformly.
 */
static inline uint32
hash_mix32(uint32 h)
{
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

/* hash_mix64 - 64-bit avalanche finalizer (MurmurHash3 fmix64) */
static inline uint64
hash_mix64(uint64 h)
{
    h ^= h >> 33;
    h *= UINT64CONST(0xFF51AFD7ED558CCD);
    h ^= h >> 33;
    h *= UINT64CONST(0xC4CEB9FE1A85EC53);
    h ^= h >> 33;
    return h;
}

/* SQL argument validation/encoding shared by the hash entry points. */
extern ChecksumAlgo checksum_algorithm(text *value);
extern bytea *checksum_bytea(uint64 value, int len);

/*
 * Order-independent aggregation.  The per-item "add" steps run once per row
 * or page and are inline for the same reason as the hash primitives.
 */

/* checksum_accum_add - fold a single raw 32-bit hash into an accumulator */
static inline void
checksum_accum_add(ChecksumAccum *acc, uint32 h)
{
    acc->partial ^= hash_mix32(h);
    acc->count++;
}

/* checksum_accum64_add - fold a single raw 64-bit hash into an accumulator */
static inline void
checksum_accum64_add(ChecksumAccum64 *acc, uint64 h)
{
    acc->partial ^= hash_mix64(h);
    acc->count++;
}

/* logical_accum_add - fold one logical item into an accumulator */
static inline void
logical_accum_add(ChecksumLogicalAccum *acc, uint32 h)
{
    acc->sum += hash_mix64((uint64) h);
    acc->count++;
}

/* logical_accum_add64 - fold one 64-bit logical item into an accumulator */
static inline void
logical_accum_add64(ChecksumLogicalAccum64 *acc, uint64 h)
{
    acc->sum += hash_mix64(h);
    acc->count++;
}

extern void checksum_accum_merge(ChecksumAccum *acc, uint64 partial,
                                 uint64 count);
extern uint32 checksum_accum_finalize(ChecksumAccum *acc);
extern void checksum_accum64_merge(ChecksumAccum64 *acc, uint64 partial,
                                   uint64 count);
extern uint64 checksum_accum64_finalize(ChecksumAccum64 *acc);
extern uint32 logical_accum_finalize(ChecksumLogicalAccum *acc);
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
extern bool pg_index_logical_row_hash(IndexLogicalScanState *scanstate,
                                      HeapTuple tuple, ChecksumAlgo algo,
                                      uint64 *row_hash);

/* Database relation-range aggregation shared with the parallel scan. */
extern void compute_database_items(ChecksumRelationItem *items, uint32 start,
                                   uint32 end, bool physical, ChecksumAlgo algo,
                                   ChecksumAccum64 *acc,
                                   ChecksumLogicalAccum *logical_acc,
                                   ChecksumLogicalAccum64 *logical_acc64,
                                   uint32 *skipped);

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
/*
 * Infomask bits that are hints, set lazily by readers and not tuple state.
 * Physical checksums mask them out.
 */
#define HEAP_HINT_BITS \
    (HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID | \
     HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)

/* Page format interpreted by pg_integrity_checksums_mask_page(). */
typedef enum ChecksumPageKind
{
    CHECKSUM_PAGE_HEAP,         /* heap page (TOAST relations) */
    CHECKSUM_PAGE_INDEX         /* index page; relam selects the AM */
} ChecksumPageKind;

extern void pg_integrity_checksums_mask_page(Page page, char *dst,
                                             ChecksumPageKind kind,
                                             Oid relam);
extern void scan_relation_raw_pages(Relation rel, BlockNumber start,
                                     BlockNumber end, ChecksumAccum *acc);
extern void scan_relation_raw_pages64(Relation rel, BlockNumber start,
                                       BlockNumber end, ChecksumAccum64 *acc);

/* Tuple checksum cores, shared with the block scan and database modules. */
extern uint64 pg_tuple_physical_checksum_internal(Relation rel, Page page,
                                                  OffsetNumber offnum,
                                                  BlockNumber blkno,
                                                  bool include_header,
                                                  bool validate, ChecksumAlgo algo);
extern uint64 pg_tuple_logical_checksum_internal(Relation rel,
                                                 HeapTuple tuple, ChecksumAlgo algo);

/*
 * LogicalRowHasher - state for hashing many rows of one relation
 *
 * See logical_row_hasher_init().  Not shared between processes.
 */
typedef struct LogicalRowHasher
{
    TupleDesc   tupdesc;
    int         ncols;          /* number of live columns */
    int        *attidx;         /* 0-based attribute index of each live column */
    LogicalColumnHint *hints;   /* hashing hint per live column */
    Datum      *values;         /* heap_deform_tuple() output, natts long */
    bool       *nulls;
    MemoryContext rowcxt;       /* reset after every row */
} LogicalRowHasher;

extern void logical_row_hasher_init(LogicalRowHasher *hasher,
                                    TupleDesc tupdesc);
extern uint64 logical_row_hash(LogicalRowHasher *hasher, HeapTuple tuple,
                               ChecksumAlgo algo);
extern void logical_row_hasher_free(LogicalRowHasher *hasher);

/*
 * TupleVisitor - called for every heap tuple that is visible to a snapshot
 *
 * When scan_visible_tuples() is called with needs_lock, the page is
 * share-locked for the duration of the call; use that only for visitors that
 * read tuple header fields which concurrent backends may change (xmax,
 * infomask) and that do nothing but local computation.  Otherwise the buffer
 * is only pinned: the pin keeps the tuple in place (pruning needs a cleanup
 * lock), and the visitor may detoast, call type I/O functions or evaluate
 * expressions, none of which may run while a buffer content lock is held.
 */
typedef void (*TupleVisitor) (Relation rel, Page page, BlockNumber blkno,
                              OffsetNumber offnum, HeapTuple tuple,
                              void *arg);

/* Serial block-range scans. */
extern void scan_visible_tuples(Relation rel, BlockNumber start,
                                BlockNumber end, Snapshot snapshot,
                                BufferAccessStrategy bstrategy,
                                bool needs_lock,
                                TupleVisitor visitor, void *arg);
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
extern void logical_type_cache_invalidate(Datum arg, int cacheid,
                                          uint32 hashvalue);
extern uint64 pg_cell_checksum_canonical_algo(Datum value, bool isnull,
                                              Oid typid, int32 typmod,
                                              int attnum, ChecksumAlgo algo);

extern uint64 pg_cell_checksum_canonical_hinted(Datum value, bool isnull,
                                                Oid typid, int32 typmod,
                                                int attnum, ChecksumAlgo algo,
                                                LogicalColumnHint *hint);

#endif /* PG_INTEGRITY_CHECKSUMS_INTERNAL_H */
