/*-------------------------------------------------------------------------
 *
 * checksum_types.c
 *    Canonical logical encoding for PostgreSQL data types
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include <math.h>
#include "fmgr.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/arrayaccess.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rangetypes.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#if PG_VERSION_NUM >= 140000
#include "utils/multirangetypes.h"
#endif

#include "checksum_internal.h"

/*
 * Cached type metadata for canonical logical encoding.  Type names and I/O
 * function OIDs are stable until a syscache invalidation, which clears the
 * cache.
 */
typedef struct LogicalTypeInfo
{
    Oid         typid;          /* hash key; must be the first field */
    uint32      tag_prefix;
    uint64      tag_prefix64;
    Oid         base_typid;
    int32       base_typmod;
    Oid         elemtype;
    char        base_typtype;
    bool        is_domain;
    Oid         sendfunc;
    Oid         outfunc;
    bool        has_send;
    int16       typlen;
    bool        typbyval;
    char        typalign;
} LogicalTypeInfo;

static HTAB *logical_type_cache = NULL;

static LogicalTypeInfo *logical_type_info(Oid typid);

/*-------------------------------------------------------------------------
 * Helper functions for column checksum computation
 *-------------------------------------------------------------------------
 */


/*
 * Canonical logical value encoding
 *
 * The goal is a checksum that depends on the value, not on relation or type
 * OIDs.  Scalar values use their binary send function (network byte order);
 * arrays, composites, ranges and numeric are handled recursively so that
 * embedded OIDs and insignificant numeric scale do not leak into the hash.
 */

static uint64 logical_hash_datum_algo(Datum value, bool isnull, Oid typid,
                                      int32 typmod, int attnum, int depth,
                                      int algo);
uint64 combine_logical(uint64 current, uint64 new_val, int algo);

static uint64
logical_hash_bytes(uint64 h, const void *data, size_t len, int algo)
{
    if (algo == 0)
        return fnv1a_32_hash(data, len, (uint32) h);
    return fnv1a_64_hash(data, len, h);
}

/*
 * logical_hash_uint32_le - hash a 32-bit integer in a canonical byte order
 *
 * Logical checksums must not depend on the server's native endianness.  The
 * historical encoding hashed the native bytes of int/uint32 values, which
 * happened to be little-endian on all platforms where 2.x baselines were
 * created.  Keep those values unchanged on little-endian systems and emit the
 * same little-endian bytes on big-endian systems.
 */
static uint64
logical_hash_uint32_le(uint64 h, uint32 value, int algo)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char) (value & 0xFF);
    bytes[1] = (unsigned char) ((value >> 8) & 0xFF);
    bytes[2] = (unsigned char) ((value >> 16) & 0xFF);
    bytes[3] = (unsigned char) ((value >> 24) & 0xFF);

    return logical_hash_bytes(h, bytes, sizeof(bytes), algo);
}

void
logical_type_cache_invalidate(Datum arg, int cacheid, uint32 hashvalue)
{
    (void) arg;
    (void) cacheid;
    (void) hashvalue;

    if (logical_type_cache != NULL)
    {
        hash_destroy(logical_type_cache);
        logical_type_cache = NULL;
    }
}

static LogicalTypeInfo *
logical_type_info(Oid typid)
{
    LogicalTypeInfo *info;
    bool        found;

    if (logical_type_cache == NULL)
    {
        HASHCTL     ctl;

        MemSet(&ctl, 0, sizeof(ctl));
        ctl.keysize = sizeof(Oid);
        ctl.entrysize = sizeof(LogicalTypeInfo);
        ctl.hcxt = TopMemoryContext;
        logical_type_cache = hash_create("pg_integrity_checksums logical type cache",
                                         64, &ctl,
                                         HASH_ELEM | HASH_BLOBS);
    }

    info = (LogicalTypeInfo *) hash_search(logical_type_cache, &typid,
                                           HASH_ENTER, &found);
    if (!found)
    {
        HeapTuple   type_tuple;
        Form_pg_type type_form;
        char       *tag;

        type_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
        if (!HeapTupleIsValid(type_tuple))
            elog(ERROR, "cache lookup failed for type %u", typid);

        type_form = (Form_pg_type) GETSTRUCT(type_tuple);
        tag = format_type_be(typid);

        info->typid = typid;
        info->tag_prefix = combine_checksums(FNV_BASIS_32,
                                             fnv1a_32_hash(tag, strlen(tag),
                                                           FNV_BASIS_32));
        info->tag_prefix64 = combine_checksums_64(FNV_BASIS_64,
                                                  fnv1a_64_hash(tag, strlen(tag),
                                                                FNV_BASIS_64));
        info->sendfunc = type_form->typsend;
        info->outfunc = type_form->typoutput;
        info->has_send = OidIsValid(type_form->typsend);
        info->typlen = type_form->typlen;
        info->typbyval = type_form->typbyval;
        info->typalign = type_form->typalign;
        info->is_domain = (type_form->typtype == TYPTYPE_DOMAIN);

        if (info->is_domain)
        {
            int32       domain_typmod = -1;

            info->base_typid = getBaseTypeAndTypmod(typid, &domain_typmod);
            info->base_typmod = domain_typmod;
        }
        else
        {
            info->base_typid = typid;
            info->base_typmod = -1;
        }

        info->base_typtype = get_typtype(info->base_typid);
        info->elemtype = get_element_type(info->base_typid);

        pfree(tag);
        ReleaseSysCache(type_tuple);
    }

    return info;
}

static uint64
logical_hash_array(Datum value, int depth, int algo)
{
    AnyArrayType *arr = (AnyArrayType *) DatumGetArrayTypeP(value);
    Oid         elemtype = AARR_ELEMTYPE(arr);
    LogicalTypeInfo *elem_info = logical_type_info(elemtype);
    array_iter  iter;
    int         nelems;
    int         ndim = AARR_NDIM(arr);
    int         i;
    uint64      h = algo == 0 ? FNV_BASIS_32 : FNV_BASIS_64;

    /* Hash the shape first: an empty array differs from an empty string. */
    h = logical_hash_uint32_le(h, (uint32) ndim, algo);
    if (ndim > 0)
    {
        for (i = 0; i < ndim; i++)
            h = logical_hash_uint32_le(h, (uint32) AARR_DIMS(arr)[i], algo);
        for (i = 0; i < ndim; i++)
            h = logical_hash_uint32_le(h, (uint32) AARR_LBOUND(arr)[i], algo);
    }

    nelems = (ndim > 0) ? ArrayGetNItems(ndim, AARR_DIMS(arr)) : 0;
    h = logical_hash_uint32_le(h, (uint32) nelems, algo);

    if (nelems > 0)
    {
#if PG_VERSION_NUM >= 190000
        array_iter_setup(&iter, arr, elem_info->typlen,
                         elem_info->typbyval, elem_info->typalign);
#else
        array_iter_setup(&iter, arr);
#endif

        for (i = 0; i < nelems; i++)
        {
            bool        isnull;
            Datum       elem;
            uint64      elem_hash;

#if PG_VERSION_NUM >= 190000
            elem = array_iter_next(&iter, &isnull, i);
#else
            elem = array_iter_next(&iter, &isnull, i, elem_info->typlen,
                                   elem_info->typbyval, elem_info->typalign);
#endif
            elem_hash = logical_hash_datum_algo(elem, isnull, elemtype, -1,
                                                i + 1, depth + 1, algo);
            h = combine_logical(h, elem_hash, algo);
        }
    }

    if ((Pointer) arr != DatumGetPointer(value))
        pfree(arr);

    return h;
}

static uint64
logical_hash_composite(Datum value, Oid typid, int32 typmod, int depth, int algo)
{
    HeapTupleHeader rec = DatumGetHeapTupleHeader(value);
    Oid         tupType = typid;
    int32       tupTypmod = typmod;
    TupleDesc   tupdesc;
    HeapTupleData tuple;
    Datum       values_local[32];
    bool        nulls_local[32];
    Datum      *values;
    bool       *nulls;
    bool        local_arrays;
    uint64      h = algo == 0 ? FNV_BASIS_32 : FNV_BASIS_64;
    int         non_dropped = 0;
    int         i;

    if (typid == RECORDOID)
    {
        tupType = HeapTupleHeaderGetTypeId(rec);
        tupTypmod = HeapTupleHeaderGetTypMod(rec);
    }

    tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

    for (i = 0; i < tupdesc->natts; i++)
    {
        if (!TupleDescAttr(tupdesc, i)->attisdropped)
            non_dropped++;
    }

    h = logical_hash_uint32_le(h, (uint32) non_dropped, algo);

    local_arrays = (tupdesc->natts <= 32);
    if (local_arrays)
    {
        values = values_local;
        nulls = nulls_local;
    }
    else
    {
        values = palloc(sizeof(Datum) * tupdesc->natts);
        nulls = palloc(sizeof(bool) * tupdesc->natts);
    }

    tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
    ItemPointerSetInvalid(&tuple.t_self);
    tuple.t_tableOid = InvalidOid;
    tuple.t_data = rec;

    heap_deform_tuple(&tuple, tupdesc, values, nulls);

    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        uint64      field_hash;

        if (att->attisdropped)
            continue;

        field_hash = logical_hash_datum_algo(values[i], nulls[i],
                                             att->atttypid, att->atttypmod,
                                             i + 1, depth + 1, algo);
        h = combine_logical(h, field_hash, algo);
    }

    if (!local_arrays)
    {
        pfree(values);
        pfree(nulls);
    }
    ReleaseTupleDesc(tupdesc);

    if ((Pointer) rec != DatumGetPointer(value))
        pfree(rec);

    return h;
}

static uint64
logical_hash_range(Datum value, Oid typid, int depth, int algo)
{
    RangeType  *range = DatumGetRangeTypeP(value);
    TypeCacheEntry *typcache = lookup_type_cache(typid, TYPECACHE_RANGE_INFO);
    RangeBound  lower;
    RangeBound  upper;
    bool        empty;
    uint64      h = algo == 0 ? FNV_BASIS_32 : FNV_BASIS_64;

    range_deserialize(typcache, range, &lower, &upper, &empty);

    h = logical_hash_bytes(h, &empty, sizeof(empty), algo);

    if (!empty)
    {
        h = logical_hash_bytes(h, &lower.infinite, sizeof(lower.infinite), algo);
        h = logical_hash_bytes(h, &lower.inclusive, sizeof(lower.inclusive), algo);
        if (!lower.infinite)
        {
            uint64      lower_hash;

            lower_hash = logical_hash_datum_algo(lower.val, false,
                                                 typcache->rngelemtype->type_id,
                                                 -1, 1, depth + 1, algo);
            h = combine_logical(h, lower_hash, algo);
        }

        h = logical_hash_bytes(h, &upper.infinite, sizeof(upper.infinite), algo);
        h = logical_hash_bytes(h, &upper.inclusive, sizeof(upper.inclusive), algo);
        if (!upper.infinite)
        {
            uint64      upper_hash;

            upper_hash = logical_hash_datum_algo(upper.val, false,
                                                 typcache->rngelemtype->type_id,
                                                 -1, 2, depth + 1, algo);
            h = combine_logical(h, upper_hash, algo);
        }
    }

    if ((Pointer) range != DatumGetPointer(value))
        pfree(range);

    return h;
}

#if PG_VERSION_NUM >= 140000
static uint64
logical_hash_multirange(Datum value, Oid typid, int depth, int algo)
{
    MultirangeType *mr = DatumGetMultirangeTypeP(value);
    TypeCacheEntry *typcache = lookup_type_cache(typid,
                                                 TYPECACHE_MULTIRANGE_INFO);
    uint32      count = mr->rangeCount;
    uint64      h = algo == 0 ? FNV_BASIS_32 : FNV_BASIS_64;
    uint32      i;

    h = logical_hash_uint32_le(h, count, algo);

    for (i = 0; i < count; i++)
    {
        RangeBound  lower;
        RangeBound  upper;
        uint64      lower_hash;
        uint64      upper_hash;

        multirange_get_bounds(typcache->rngtype, mr, i, &lower, &upper);

        h = logical_hash_bytes(h, &lower.infinite, sizeof(lower.infinite), algo);
        h = logical_hash_bytes(h, &lower.inclusive, sizeof(lower.inclusive), algo);
        if (!lower.infinite)
        {
            lower_hash = logical_hash_datum_algo(lower.val, false,
                                                 typcache->rngtype->rngelemtype->type_id,
                                                 -1, 1, depth + 1, algo);
            h = combine_logical(h, lower_hash, algo);
        }

        h = logical_hash_bytes(h, &upper.infinite, sizeof(upper.infinite), algo);
        h = logical_hash_bytes(h, &upper.inclusive, sizeof(upper.inclusive), algo);
        if (!upper.infinite)
        {
            upper_hash = logical_hash_datum_algo(upper.val, false,
                                                 typcache->rngtype->rngelemtype->type_id,
                                                 -1, 2, depth + 1, algo);
            h = combine_logical(h, upper_hash, algo);
        }
    }

    if ((Pointer) mr != DatumGetPointer(value))
        pfree(mr);

    return h;
}
#endif

static uint64
logical_hash_datum_algo(Datum value, bool isnull, Oid typid,
                        int32 typmod, int attnum, int depth, int algo)
{
    LogicalTypeInfo *type_info;
    char        typtype;
    Oid         elemtype;
    bool        specialized = false;
    uint64      h = algo == 0 ? FNV_BASIS_32 : FNV_BASIS_64;

    if (depth > 32)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("logical checksum recursion is too deep")));

    check_stack_depth();

    if (isnull)
        return CHECKSUM_NULL;

    type_info = logical_type_info(typid);
    if (algo == 0)
        h = type_info->tag_prefix;
    else
        h = type_info->tag_prefix64;
    h = combine_logical(h, (uint64) attnum, algo);

    typid = type_info->base_typid;
    if (type_info->is_domain)
        typmod = type_info->base_typmod;
    typtype = type_info->base_typtype;
    elemtype = type_info->elemtype;

    /*
     * Real array types have a concrete element type.  Polymorphic anyarray
     * values (for example pg_statistic.stavalues) are still arrays at
     * runtime, but get_element_type() returns InvalidOid for the pseudotype.
     * Treat them as arrays and take the actual element type from the value's
     * array header.
     */
    if (OidIsValid(elemtype) ||
        typid == ANYARRAYOID
#if PG_VERSION_NUM >= 140000
        || typid == ANYCOMPATIBLEARRAYOID
#endif
        )
    {
        h = combine_logical(h, logical_hash_array(value, depth, algo), algo);
        specialized = true;
    }

    if (typtype == TYPTYPE_COMPOSITE || typid == RECORDOID)
    {
        h = combine_logical(h,
                            logical_hash_composite(value, typid, typmod,
                                                   depth, algo), algo);
        specialized = true;
    }

    if (typtype == TYPTYPE_RANGE)
    {
        h = combine_logical(h, logical_hash_range(value, typid, depth, algo), algo);
        specialized = true;
    }

#if PG_VERSION_NUM >= 140000
    if (typtype == TYPTYPE_MULTIRANGE)
    {
        h = combine_logical(h,
                            logical_hash_multirange(value, typid, depth, algo),
                            algo);
        specialized = true;
    }
#endif

    if (!specialized && typid == NUMERICOID)
    {
        Numeric     num = DatumGetNumeric(value);
        char       *str = numeric_normalize(num);

        h = logical_hash_bytes(h, str, strlen(str), algo);
        pfree(str);

        if ((Pointer) num != DatumGetPointer(value))
            pfree(num);
    }
    else if (!specialized && typid == FLOAT4OID)
    {
        float4      f = DatumGetFloat4(value);

        if (isnan(f))
            h = logical_hash_bytes(h, "NaN", 3, algo);
        else if (f == 0)
        {
            uint32      zero = 0;

            h = logical_hash_bytes(h, &zero, sizeof(zero), algo);
        }
        else
        {
            LogicalTypeInfo *base_info = type_info->is_domain ?
                logical_type_info(typid) : type_info;
            bytea      *bytes = OidSendFunctionCall(base_info->sendfunc, value);

            h = logical_hash_bytes(h, VARDATA(bytes),
                                   VARSIZE(bytes) - VARHDRSZ, algo);
            pfree(bytes);
        }
    }
    else if (!specialized && typid == FLOAT8OID)
    {
        float8      f = DatumGetFloat8(value);

        if (isnan(f))
            h = logical_hash_bytes(h, "NaN", 3, algo);
        else if (f == 0)
        {
            uint64      zero = 0;

            h = logical_hash_bytes(h, &zero, sizeof(zero), algo);
        }
        else
        {
            LogicalTypeInfo *base_info = type_info->is_domain ?
                logical_type_info(typid) : type_info;
            bytea      *bytes = OidSendFunctionCall(base_info->sendfunc, value);

            h = logical_hash_bytes(h, VARDATA(bytes),
                                   VARSIZE(bytes) - VARHDRSZ, algo);
            pfree(bytes);
        }
    }
    else if (!specialized)
    {
        LogicalTypeInfo *base_info = type_info->is_domain ?
            logical_type_info(typid) : type_info;

        if (base_info->has_send)
        {
            bytea      *bytes = OidSendFunctionCall(base_info->sendfunc, value);

            h = logical_hash_bytes(h, VARDATA(bytes),
                                   VARSIZE(bytes) - VARHDRSZ, algo);
            pfree(bytes);
        }
        else if (OidIsValid(base_info->outfunc))
        {
            char       *str = OidOutputFunctionCall(base_info->outfunc, value);

            h = logical_hash_bytes(h, str, strlen(str), algo);
            pfree(str);
        }
        else
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_FUNCTION),
                     errmsg("no output function available for type %s",
                            format_type_be(typid))));
    }

    if (algo == 0)
    {
        if (h == 0 || h == CHECKSUM_NULL)
            h = hash_mix32((uint32) h ^ FNV_BASIS_32 ^ (uint32) attnum);
    }
    else
    {
        if (h == 0 || h == CHECKSUM_NULL_64)
            h = hash_mix64(h ^ FNV_BASIS_64 ^ (uint64) attnum);
    }

    return h;
}

/*
 * combine_logical - combine two canonical logical hashes
 */
uint64
combine_logical(uint64 current, uint64 new_val, int algo)
{
    if (algo == 0)
        return combine_checksums((uint32) current, (uint32) new_val);
    return combine_checksums_64(current, new_val);
}

/*
 * logical_hash_datum - legacy 32-bit canonical hash wrapper
 */
static uint32
logical_hash_datum(Datum value, bool isnull, Oid typid,
                   int32 typmod, int attnum, int depth)
{
    return (uint32) logical_hash_datum_algo(value, isnull, typid, typmod,
                                            attnum, depth, 0);
}

/*
 * pg_cell_checksum_canonical_algo - canonical logical cell checksum
 *
 * algo == 0 selects the historical 32-bit FNV path; algo == 1 selects
 * FNV-1a 64-bit. The canonicalization is shared by both paths.
 */
uint64
pg_cell_checksum_canonical_algo(Datum value, bool isnull, Oid typid,
                                int32 typmod, int attnum, int algo)
{
    return logical_hash_datum_algo(value, isnull, typid, typmod, attnum, 0,
                                   algo);
}

/*
 * pg_cell_checksum_canonical - legacy 32-bit logical cell checksum
 */
uint32
pg_cell_checksum_canonical(Datum value, bool isnull, Oid typid,
                           int32 typmod, int attnum)
{
    return logical_hash_datum(value, isnull, typid, typmod, attnum, 0);
}
