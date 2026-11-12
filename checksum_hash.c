/*-------------------------------------------------------------------------
 *
 * checksum_hash.c
 *    FNV-1a hashing, combination and order-independent aggregation
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/htup_details.h"
#include "utils/builtins.h"

#include "checksum_internal.h"

/*-------------------------------------------------------------------------
 * FNV-1a Hash Implementation
 *-------------------------------------------------------------------------
 */

/*
 * fnv1a_32_buf - continue an FNV-1a hash over a byte range
 *
 * Unlike fnv1a_32_hash(), the incoming hash is used as-is (an all-zero value
 * is a legitimate state), so calls may be chained over several buffers and
 * still produce exactly what a single pass over their concatenation would.
 */
uint32
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
 * fnv1a_32_hash - FNV-1a 32-bit hash implementation
 *
 * Standard FNV-1a: each input byte is XORed into the hash, which is then
 * multiplied by the FNV prime.  This is unrelated to PostgreSQL's own
 * 16-bit page checksum algorithm.
 *
 * Parameters:
 *   data: Pointer to the data to hash
 *   len: Length of the data in bytes
 *   seed: Initial hash value.  Zero is treated as "no seed" and selects
 *         FNV_BASIS_32, so seed 0 and seed FNV_BASIS_32 give identical
 *         results.
 *
 * Returns: 32-bit FNV-1a hash value
 */
uint32
fnv1a_32_hash(const void *data, size_t len, uint32 seed)
{
    return fnv1a_32_buf((const unsigned char *) data, len,
                        seed == 0 ? FNV_BASIS_32 : seed);
}

/*
 * pg_checksum_data_custom - FNV-1a 32-bit hash with the zero-seed convention
 *
 * Thin wrapper around fnv1a_32_hash() used by the data/text checksum
 * functions and by the physical tuple checksum.
 */
uint32
pg_checksum_data_custom(const char *data, uint32 len, uint32 init_value)
{
    return fnv1a_32_hash(data, len, init_value);
}

/*
 * fnv1a_64_buf - continue an FNV-1a 64-bit hash over a byte range
 */
uint64
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
 * fnv1a_64_hash - FNV-1a 64-bit hash implementation
 *
 * A zero seed selects the standard FNV-1a offset basis, matching the
 * convention used by the 32-bit helper (so seed 0 and FNV_BASIS_64 give
 * identical results).
 */
uint64
fnv1a_64_hash(const void *data, size_t len, uint64 seed)
{
    return fnv1a_64_buf((const unsigned char *) data, len,
                        seed == 0 ? FNV_BASIS_64 : seed);
}

/*
 * hash_bytes_algo - FNV-1a over a byte range with the selected algorithm
 *
 * "seed" is the running hash (or 0 to start from the offset basis, as in
 * fnv1a_32_hash() / fnv1a_64_hash()).  For fnv32 the result is zero-extended.
 */
uint64
hash_bytes_algo(uint64 seed, const void *data, size_t len, ChecksumAlgo algo)
{
    if (algo == CHECKSUM_ALGO_FNV32)
        return fnv1a_32_hash(data, len, (uint32) seed);

    return fnv1a_64_hash(data, len, seed);
}

/*
 * combine_hash - order-dependent combination with the selected algorithm
 *
 * Dispatches to combine_checksums() or combine_checksums_64().  For fnv32
 * both operands are truncated to 32 bits.
 */
uint64
combine_hash(uint64 current, uint64 new_val, ChecksumAlgo algo)
{
    if (algo == CHECKSUM_ALGO_FNV32)
        return combine_checksums((uint32) current, (uint32) new_val);

    return combine_checksums_64(current, new_val);
}

/*
 * checksum_algorithm - parse the SQL algorithm name
 *
 * The generic SQL functions accept the algorithm as text so future built-in
 * algorithms can be added without creating another family of function names.
 */
ChecksumAlgo
checksum_algorithm(text *value)
{
    char       *name = text_to_cstring(value);
    ChecksumAlgo result;

    if (strcmp(name, "fnv32") == 0)
        result = CHECKSUM_ALGO_FNV32;
    else if (strcmp(name, "fnv64") == 0)
        result = CHECKSUM_ALGO_FNV64;
    else
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("unknown checksum algorithm \"%s\"", name)));

    pfree(name);
    return result;
}

/*
 * checksum_bytea - return a digest as big-endian bytes
 *
 * Big-endian keeps the SQL representation independent of the server's
 * native byte order and makes the value easy to render as hex.
 */
bytea *
checksum_bytea(uint64 value, int len)
{
    bytea      *result = palloc(VARHDRSZ + len);
    unsigned char *bytes;
    int         i;

    SET_VARSIZE(result, VARHDRSZ + len);
    bytes = (unsigned char *) VARDATA(result);

    for (i = 0; i < len; i++)
        bytes[len - 1 - i] = (unsigned char) ((value >> (i * 8)) & 0xFF);

    return result;
}

/*
 * pg_data_checksum_algo - generic binary data checksum
 */
PG_FUNCTION_INFO_V1(pg_data_checksum_algo);

Datum
pg_data_checksum_algo(PG_FUNCTION_ARGS)
{
    bytea      *data;
    int64       seed;
    ChecksumAlgo         algorithm;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(2))
        PG_RETURN_NULL();

    data = PG_GETARG_BYTEA_PP(0);
    seed = PG_GETARG_INT64(1);
    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(2));

    if (algorithm == CHECKSUM_ALGO_FNV32)
    {
        uint32      checksum = pg_checksum_data_custom(VARDATA_ANY(data),
                                                       VARSIZE_ANY_EXHDR(data),
                                                       (uint32) seed);

        PG_RETURN_BYTEA_P(checksum_bytea(checksum, 4));
    }

    PG_RETURN_BYTEA_P(checksum_bytea(fnv1a_64_hash(VARDATA_ANY(data),
                                                    VARSIZE_ANY_EXHDR(data),
                                                    (uint64) seed), 8));
}

/*
 * pg_text_checksum_algo - generic text checksum
 */
PG_FUNCTION_INFO_V1(pg_text_checksum_algo);

Datum
pg_text_checksum_algo(PG_FUNCTION_ARGS)
{
    text       *input_text;
    int64       seed;
    ChecksumAlgo         algorithm;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(2))
        PG_RETURN_NULL();

    input_text = PG_GETARG_TEXT_PP(0);
    seed = PG_GETARG_INT64(1);
    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(2));

    if (algorithm == CHECKSUM_ALGO_FNV32)
    {
        uint32      checksum = pg_checksum_data_custom(VARDATA_ANY(input_text),
                                                       VARSIZE_ANY_EXHDR(input_text),
                                                       (uint32) seed);

        PG_RETURN_BYTEA_P(checksum_bytea(checksum, 4));
    }

    PG_RETURN_BYTEA_P(checksum_bytea(fnv1a_64_hash(VARDATA_ANY(input_text),
                                                    VARSIZE_ANY_EXHDR(input_text),
                                                    (uint64) seed), 8));
}

/*
 * pg_checksum_bigint - convert a 4- or 8-byte digest to a signed bigint
 *
 * Four-byte values are sign-extended so the generic fnv32 result matches the
 * historical int32 SQL functions. Eight-byte values use the two's-complement
 * representation of uint64.
 */
PG_FUNCTION_INFO_V1(pg_checksum_bigint);

Datum
pg_checksum_bigint(PG_FUNCTION_ARGS)
{
    bytea      *value;
    unsigned char *bytes;
    int         len;
    uint64      result = CHECKSUM_ALGO_FNV32;
    int         i;
    bool        unsigned_value = false;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    if (PG_NARGS() > 1)
        unsigned_value = PG_GETARG_BOOL(1);

    value = PG_GETARG_BYTEA_PP(0);
    len = VARSIZE_ANY_EXHDR(value);
    bytes = (unsigned char *) VARDATA_ANY(value);

    if (len != 4 && len != 8)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("checksum digest must be 4 or 8 bytes, got %d", len)));

    for (i = 0; i < len; i++)
        result = (result << 8) | bytes[i];

    if (len == 4)
    {
        if (unsigned_value)
            PG_RETURN_INT64((int64) (uint32) result);
        PG_RETURN_INT64((int64) (int32) (uint32) result);
    }

    PG_RETURN_INT64((int64) result);
}

/*-------------------------------------------------------------------------
 * Checksum Combination Functions
 *-------------------------------------------------------------------------
 */

/*
 * combine_checksums - Combine two 32-bit hash values using FNV-1a
 *
 * Feeds the four bytes of new_val into the running hash, most significant
 * byte first.  The result depends on the order of the arguments, so this is
 * for building ordered (not order-independent) combinations.  Note that
 * combine_checksums_64() feeds its bytes least significant first; both byte
 * orders are part of the stored baseline values and must not change.
 *
 * Parameters:
 *   current: Current hash value
 *   new_val: New hash value to combine
 *
 * Returns: Combined 32-bit hash value
 */
uint32
combine_checksums(uint32 current, uint32 new_val)
{
    uint32 hash = current;

    /* Feed the bytes of new_val, most significant byte first. */
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
 * combine_checksums_64 - Combine two 64-bit hash values using FNV-1a
 *
 * Similar to combine_checksums but for 64-bit values, using the 64-bit FNV
 * prime.  Unlike the 32-bit variant, the bytes of new_val are fed least
 * significant first.
 */
uint64
combine_checksums_64(uint64 current, uint64 new_val)
{
    uint64 hash = current;
    uint64 prime = FNV_PRIME_64;
    uint8 bytes[8];
    int i;

    /* Split new_val into bytes, least significant first. */
    for (i = 0; i < 8; i++)
    {
        bytes[i] = (new_val >> (i * 8)) & 0xFF;
    }

    /* Process each byte with FNV-1a */
    for (i = 0; i < 8; i++)
    {
        hash ^= bytes[i];
        hash *= prime;
    }

    return hash;
}

/*-------------------------------------------------------------------------
 * Order-Independent Aggregation
 *-------------------------------------------------------------------------
 */

/*
 * hash_mix32 - 32-bit avalanche finalizer
 *
 * Applies a final mixing step (based on MurmurHash3's fmix) to spread the
 * bits of a hash before it is combined into an order-independent
 * aggregate. This ensures that even highly correlated input hashes
 * contribute uniformly to the final result.
 */
uint32
hash_mix32(uint32 h)
{
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

/*
 * hash_mix64 - 64-bit avalanche finalizer
 */
uint64
hash_mix64(uint64 h)
{
    h ^= h >> 33;
    h *= UINT64CONST(0xFF51AFD7ED558CCD);
    h ^= h >> 33;
    h *= UINT64CONST(0xC4CEB9FE1A85EC53);
    h ^= h >> 33;
    return h;
}

/*
 * checksum_accum_add - fold a single raw 32-bit hash into an accumulator
 */
void
checksum_accum_add(ChecksumAccum *acc, uint32 h)
{
    acc->partial ^= hash_mix32(h);
    acc->count++;
}

/*
 * checksum_accum_merge - merge another accumulator's partial result
 *
 * The supplied partial is expected to already be the XOR of mixed hashes,
 * so it is combined without re-mixing. Used by the leader to fold the
 * fixed-size results written by parallel workers.
 */
void
checksum_accum_merge(ChecksumAccum *acc, uint64 partial, uint64 count)
{
    acc->partial ^= (uint32) partial;
    acc->count += count;
}

/*
 * checksum_accum_finalize - convert an accumulator into the final checksum
 *
 * Preserves the historical convention that an empty scan yields
 * FNV_BASIS_32 and guarantees that a non-empty scan never produces the
 * empty sentinel or zero.
 */
uint32
checksum_accum_finalize(ChecksumAccum *acc)
{
    if (acc->count == 0)
        return FNV_BASIS_32;

    if (acc->partial == 0 || acc->partial == FNV_BASIS_32)
        return 0x7FFFFFFF;

    return acc->partial;
}

/*
 * checksum_accum64_add - fold a single raw 64-bit hash into an accumulator
 */
void
checksum_accum64_add(ChecksumAccum64 *acc, uint64 h)
{
    acc->partial ^= hash_mix64(h);
    acc->count++;
}

/*
 * checksum_accum64_merge - merge another 64-bit accumulator's partial result
 */
void
checksum_accum64_merge(ChecksumAccum64 *acc, uint64 partial, uint64 count)
{
    acc->partial ^= partial;
    acc->count += count;
}

/*
 * checksum_accum64_finalize - convert a 64-bit accumulator into the final value
 *
 * The number of items is folded in so that adding or removing relations
 * changes the result even in degenerate cases.
 */
uint64
checksum_accum64_finalize(ChecksumAccum64 *acc)
{
    if (acc->count == 0)
        return FNV_BASIS_64;

    return combine_checksums_64(acc->partial, acc->count);
}

/*
 * logical_accum_add - fold one logical item into an accumulator
 */
void
logical_accum_add(ChecksumLogicalAccum *acc, uint32 h)
{
    acc->sum += hash_mix64((uint64) h);
    acc->count++;
}

/*
 * logical_accum_finalize - fold an accumulator into a 32-bit checksum
 */
uint32
logical_accum_finalize(ChecksumLogicalAccum *acc)
{
    uint64      mixed;
    uint32      result;

    if (acc->count == 0)
        return FNV_BASIS_32;

    mixed = hash_mix64(acc->sum) ^ hash_mix64(acc->count);
    result = hash_mix32((uint32) mixed ^ (uint32) (mixed >> 32));

    if (result == 0 || result == CHECKSUM_NULL)
        result = hash_mix32(result ^ FNV_BASIS_32);

    return result;
}

/*
 * logical_accum_add64 - fold one 64-bit logical row into an accumulator
 */
void
logical_accum_add64(ChecksumLogicalAccum64 *acc, uint64 h)
{
    acc->sum += hash_mix64(h);
    acc->count++;
}

/*
 * logical_accum_finalize64 - fold a 64-bit logical accumulator
 */
uint64
logical_accum_finalize64(ChecksumLogicalAccum64 *acc)
{
    uint64      mixed;

    if (acc->count == 0)
        return FNV_BASIS_64;

    mixed = hash_mix64(acc->sum) ^ hash_mix64(acc->count);

    if (mixed == 0 || mixed == CHECKSUM_NULL_64)
        mixed = hash_mix64(mixed ^ FNV_BASIS_64);

    return mixed;
}

