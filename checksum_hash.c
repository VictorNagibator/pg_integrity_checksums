/*-------------------------------------------------------------------------
 *
 * checksum_hash.c
 *    SQL hash entry points and order-independent aggregation
 *
 * The FNV-1a and hash combination primitives, and the per-item accumulator
 * steps, are static inline functions in checksum_internal.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/htup_details.h"
#include "utils/builtins.h"

#include "checksum_internal.h"

/*-------------------------------------------------------------------------
 * SQL argument handling and hash entry points
 *-------------------------------------------------------------------------
 */

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
 * Order-Independent Aggregation
 *-------------------------------------------------------------------------
 */

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

