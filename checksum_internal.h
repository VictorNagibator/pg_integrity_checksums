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

/* Extension entry point (declared to keep -Wmissing-prototypes quiet). */
extern void _PG_init(void);

typedef struct ChecksumAccum
{
    uint32      partial;
    uint64      count;
} ChecksumAccum;

typedef struct ChecksumAccum64
{
    uint64      partial;
    uint64      count;
} ChecksumAccum64;

typedef struct ChecksumLogicalAccum
{
    uint64      sum;
    uint64      count;
} ChecksumLogicalAccum;

typedef struct ChecksumLogicalAccum64
{
    uint64      sum;
    uint64      count;
} ChecksumLogicalAccum64;

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
