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
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rangetypes.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/uuid.h"
#if PG_VERSION_NUM >= 140000
#include "utils/multirangetypes.h"
#endif

#include "checksum_internal.h"

/*-------------------------------------------------------------------------
 * Type metadata cache
 *-------------------------------------------------------------------------
 */

/*
 * LogicalFastPath - built-in types whose send output is produced directly
 *
 * The canonical encoding of a scalar is the output of its binary send
 * function.  For the most common built-in types that output is a fixed,
 * documented wire format (network byte order integers, raw text bytes, ...),
 * so it is produced here without a function call and without the 1 kB
 * StringInfo every send function allocates.  The bytes hashed are exactly
 * the bytes the send function returns; the regression tests compare both
 * paths.
 */
typedef enum LogicalFastPath
{
    LFP_NONE = 0,               /* call the send (or output) function */
    LFP_BOOL,                   /* boolsend: one byte, 0 or 1 */
    LFP_CHAR,                   /* charsend: the byte itself */
    LFP_INT2,                   /* int2send */
    LFP_INT4,                   /* int4send, oidsend, date_send */
    LFP_INT8,                   /* int8send, time_send, timestamp(tz)_send,
                                 * cash_send */
    LFP_FLOAT4,                 /* float4send: IEEE bits, network order */
    LFP_FLOAT8,                 /* float8send */
    LFP_TIMETZ,                 /* timetz_send: int64 time, int32 zone */
    LFP_INTERVAL,               /* interval_send: int64, int32, int32 */
    LFP_UUID,                   /* uuid_send: 16 raw bytes */
    LFP_TEXT,                   /* textsend, varcharsend, bpcharsend */
    LFP_NAME,                   /* namesend */
    LFP_BYTEA                   /* byteasend: the raw bytes */
} LogicalFastPath;

/*
 * Cached type metadata for canonical logical encoding.
 *
 * Entries are never removed from the hash table, so a pointer to one stays
 * valid for the life of the backend; dynahash does not move entries.  A
 * syscache invalidation (of pg_type or pg_proc) only bumps
 * logical_type_cache_generation, and an entry whose generation is out of
 * date is rebuilt in place on its next lookup.  This replaces the earlier
 * scheme of destroying the whole table on invalidation, which forced every
 * lookup to return a copy.
 *
 * The send/output function fields describe the base type of a domain, so a
 * domain value is encoded with a single lookup.  The FmgrInfo structs are
 * filled once per entry instead of once per value: OidSendFunctionCall()
 * used to repeat the pg_proc lookup for every cell.
 */
struct LogicalTypeInfo
{
    Oid         typid;          /* hash key; must be the first field */
    uint64      generation;     /* logical_type_cache_generation when built */
    uint32      tag_prefix;
    uint64      tag_prefix64;
    Oid         base_typid;
    int32       base_typmod;
    Oid         elemtype;       /* element type of the base type, if array */
    char        base_typtype;
    bool        is_domain;
    int16       typlen;         /* of typid itself */
    bool        typbyval;
    char        typalign;
    LogicalFastPath fastpath;   /* of the base type */
    bool        has_send;       /* base type has a send function */
    bool        has_output;     /* base type has an output function */
    FmgrInfo    send_finfo;     /* valid when has_send */
    FmgrInfo    output_finfo;   /* valid when has_output and !has_send */
};

static HTAB *logical_type_cache = NULL;
static MemoryContext logical_type_cache_context = NULL;
static uint64 logical_type_cache_generation = 1;

static LogicalTypeInfo *logical_type_info(Oid typid);

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
                                      ChecksumAlgo algo,
                                      LogicalColumnHint *hint);

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
logical_hash_uint32_le(uint64 h, uint32 value, ChecksumAlgo algo)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char) (value & 0xFF);
    bytes[1] = (unsigned char) ((value >> 8) & 0xFF);
    bytes[2] = (unsigned char) ((value >> 16) & 0xFF);
    bytes[3] = (unsigned char) ((value >> 24) & 0xFF);

    return hash_bytes_algo(h, bytes, sizeof(bytes), algo);
}

/* Network byte order (big-endian) helpers matching pq_sendintNN(). */
static inline uint64
logical_hash_be32(uint64 h, uint32 value, ChecksumAlgo algo)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char) (value >> 24);
    bytes[1] = (unsigned char) (value >> 16);
    bytes[2] = (unsigned char) (value >> 8);
    bytes[3] = (unsigned char) value;

    return hash_bytes_algo(h, bytes, sizeof(bytes), algo);
}

static inline uint64
logical_hash_be64(uint64 h, uint64 value, ChecksumAlgo algo)
{
    unsigned char bytes[8];

    for (int i = 0; i < 8; i++)
        bytes[i] = (unsigned char) (value >> (56 - 8 * i));

    return hash_bytes_algo(h, bytes, sizeof(bytes), algo);
}

/*
 * logical_type_cache_invalidate - syscache callback for pg_type and pg_proc
 *
 * Only marks every entry out of date; see struct LogicalTypeInfo.
 */
void
logical_type_cache_invalidate(Datum arg, int cacheid, uint32 hashvalue)
{
    (void) arg;
    (void) cacheid;
    (void) hashvalue;

    logical_type_cache_generation++;
}

/*
 * logical_type_tag - the name that identifies a type in the hash
 *
 * The tag must not depend on the session.  format_type_be() qualifies a name
 * with its schema exactly when the type is not visible in search_path, so two
 * sessions could hash the same enum or composite value differently.  Types in
 * pg_catalog keep the format_type_be() spelling ("integer", "timestamp with
 * time zone", ...); pg_catalog is always searched first, so that name is
 * stable.  Every other type is identified by its unqualified, quoted name,
 * which is also what format_type_be() returns for a visible type; arrays use
 * the tag of their element type plus "[]".
 *
 * The result is palloc'd.
 */
static char *
logical_type_tag(Oid typid)
{
    Oid         elemtype = get_element_type(typid);
    HeapTuple   type_tuple;
    Form_pg_type type_form;
    char       *tag;

    if (OidIsValid(elemtype))
    {
        char       *elem_tag = logical_type_tag(elemtype);

        tag = psprintf("%s[]", elem_tag);
        pfree(elem_tag);
        return tag;
    }

    type_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
    if (!HeapTupleIsValid(type_tuple))
        elog(ERROR, "cache lookup failed for type %u", typid);

    type_form = (Form_pg_type) GETSTRUCT(type_tuple);

    if (type_form->typnamespace == PG_CATALOG_NAMESPACE)
        tag = format_type_be(typid);
    else
        tag = pstrdup(quote_identifier(NameStr(type_form->typname)));

    ReleaseSysCache(type_tuple);

    return tag;
}

/*
 * logical_type_fastpath - the direct encoding for a built-in base type
 */
static LogicalFastPath
logical_type_fastpath(Oid base_typid)
{
    switch (base_typid)
    {
        case BOOLOID:
            return LFP_BOOL;
        case CHAROID:
            return LFP_CHAR;
        case INT2OID:
            return LFP_INT2;
        case INT4OID:
        case OIDOID:
        case DATEOID:
            return LFP_INT4;
        case INT8OID:
        case TIMEOID:
        case TIMESTAMPOID:
        case TIMESTAMPTZOID:
        case CASHOID:
            return LFP_INT8;
        case FLOAT4OID:
            return LFP_FLOAT4;
        case FLOAT8OID:
            return LFP_FLOAT8;
        case TIMETZOID:
            return LFP_TIMETZ;
        case INTERVALOID:
            return LFP_INTERVAL;
        case UUIDOID:
            return LFP_UUID;
        case TEXTOID:
        case VARCHAROID:
        case BPCHAROID:
            return LFP_TEXT;
        case NAMEOID:
            return LFP_NAME;
        case BYTEAOID:
            return LFP_BYTEA;
        default:
            return LFP_NONE;
    }
}

/*
 * logical_type_info - look up (and cache) the metadata of a type
 *
 * The entry is built in a local variable and only copied into the table once
 * all catalog lookups are done: the catalog work may process invalidation
 * messages, and an error part-way must not leave a half-initialized entry
 * behind.  The generation is sampled before the lookups, so an invalidation
 * that arrives during them leaves the new entry already out of date.
 */
static LogicalTypeInfo *
logical_type_info(Oid typid)
{
    LogicalTypeInfo info;
    LogicalTypeInfo *cached = NULL;
    uint64      generation = logical_type_cache_generation;
    HeapTuple   type_tuple;
    Form_pg_type type_form;
    Oid         sendfunc;
    Oid         outfunc;
    char       *tag;
    bool        found;

    if (logical_type_cache != NULL)
    {
        cached = (LogicalTypeInfo *) hash_search(logical_type_cache, &typid,
                                                 HASH_FIND, NULL);
        if (cached != NULL && cached->generation == generation)
            return cached;
    }
    else
    {
        HASHCTL     ctl;

        logical_type_cache_context =
            AllocSetContextCreate(TopMemoryContext,
                                  "pg_integrity_checksums logical type cache",
                                  ALLOCSET_SMALL_SIZES);

        MemSet(&ctl, 0, sizeof(ctl));
        ctl.keysize = sizeof(Oid);
        ctl.entrysize = sizeof(LogicalTypeInfo);
        ctl.hcxt = logical_type_cache_context;
        logical_type_cache = hash_create("pg_integrity_checksums logical type cache",
                                         64, &ctl,
                                         HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    }

    memset(&info, 0, sizeof(info));

    type_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
    if (!HeapTupleIsValid(type_tuple))
        elog(ERROR, "cache lookup failed for type %u", typid);

    type_form = (Form_pg_type) GETSTRUCT(type_tuple);

    info.typid = typid;
    info.generation = generation;
    info.typlen = type_form->typlen;
    info.typbyval = type_form->typbyval;
    info.typalign = type_form->typalign;
    info.is_domain = (type_form->typtype == TYPTYPE_DOMAIN);
    sendfunc = type_form->typsend;
    outfunc = type_form->typoutput;

    ReleaseSysCache(type_tuple);

    tag = logical_type_tag(typid);
    info.tag_prefix = combine_checksums(FNV_BASIS_32,
                                        fnv1a_32_hash(tag, strlen(tag),
                                                      FNV_BASIS_32));
    info.tag_prefix64 = combine_checksums_64(FNV_BASIS_64,
                                             fnv1a_64_hash(tag, strlen(tag),
                                                           FNV_BASIS_64));
    pfree(tag);

    if (info.is_domain)
    {
        int32       domain_typmod = -1;

        info.base_typid = getBaseTypeAndTypmod(typid, &domain_typmod);
        info.base_typmod = domain_typmod;

        /* A domain is encoded with the I/O functions of its base type. */
        type_tuple = SearchSysCache1(TYPEOID,
                                     ObjectIdGetDatum(info.base_typid));
        if (!HeapTupleIsValid(type_tuple))
            elog(ERROR, "cache lookup failed for type %u", info.base_typid);
        type_form = (Form_pg_type) GETSTRUCT(type_tuple);
        sendfunc = type_form->typsend;
        outfunc = type_form->typoutput;
        ReleaseSysCache(type_tuple);
    }
    else
    {
        info.base_typid = typid;
        info.base_typmod = -1;
    }

    info.base_typtype = get_typtype(info.base_typid);
    info.elemtype = get_element_type(info.base_typid);
    info.fastpath = logical_type_fastpath(info.base_typid);
    info.has_send = OidIsValid(sendfunc);
    info.has_output = OidIsValid(outfunc);

    /*
     * Only scalars that reach the generic path call these functions; arrays,
     * composites, ranges and numeric are encoded structurally.  fmgr_info
     * does a catalog lookup of its own, so do it while still building the
     * local copy.
     */
    if (info.has_send)
        fmgr_info_cxt(sendfunc, &info.send_finfo, logical_type_cache_context);
    else if (info.has_output)
        fmgr_info_cxt(outfunc, &info.output_finfo, logical_type_cache_context);

    /*
     * Look the entry up again: the catalog accesses above may have run
     * invalidation callbacks, but they never remove entries, so a pointer
     * found earlier would still be valid; HASH_ENTER returns the existing
     * entry for a stale one, which is then rebuilt in place.
     */
    cached = (LogicalTypeInfo *) hash_search(logical_type_cache, &typid,
                                             HASH_ENTER, &found);
    *cached = info;

    return cached;
}

/*
 * logical_value_prefix - the hash state a value of this type starts from
 */
static inline uint64
logical_value_prefix(LogicalTypeInfo *info, int attnum, ChecksumAlgo algo)
{
    uint64      h;

    if (algo == CHECKSUM_ALGO_FNV32)
        h = info->tag_prefix;
    else
        h = info->tag_prefix64;

    return combine_hash(h, (uint64) attnum, algo);
}

/*
 * text_needs_conversion - does pq_sendtext() convert text of this length?
 *
 * textsend() and namesend() convert to the client encoding.  Mirror the
 * cases in which pg_server_to_any() returns its input unchanged and without
 * validation; everything else goes through the real send function.
 */
static inline bool
text_needs_conversion(int len)
{
    int         client_encoding;

    if (len <= 0)
        return false;

    client_encoding = pg_get_client_encoding();

    return !(client_encoding == GetDatabaseEncoding() ||
             client_encoding == PG_SQL_ASCII);
}

/*
 * logical_hash_send - hash the send (or output) function result of a value
 */
static uint64
logical_hash_send(uint64 h, Datum value, LogicalTypeInfo *info,
                  ChecksumAlgo algo)
{
    if (info->has_send)
    {
        bytea      *bytes = SendFunctionCall(&info->send_finfo, value);

        h = hash_bytes_algo(h, VARDATA(bytes), VARSIZE(bytes) - VARHDRSZ,
                            algo);
        pfree(bytes);
    }
    else if (info->has_output)
    {
        char       *str = OutputFunctionCall(&info->output_finfo, value);

        h = hash_bytes_algo(h, str, strlen(str), algo);
        pfree(str);
    }
    else
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                 errmsg("no output function available for type %s",
                        format_type_be(info->base_typid))));

    return h;
}

/*
 * logical_hash_scalar - hash the canonical encoding of a scalar value
 *
 * "info" describes the value's type; its base type selects the encoding.
 * The fast paths hash exactly the bytes the base type's send function would
 * return.
 */
static uint64
logical_hash_scalar(uint64 h, Datum value, LogicalTypeInfo *info,
                    ChecksumAlgo algo)
{
    switch (info->fastpath)
    {
        case LFP_BOOL:
            {
                unsigned char b = DatumGetBool(value) ? 1 : 0;

                return hash_bytes_algo(h, &b, 1, algo);
            }
        case LFP_CHAR:
            {
                char        c = DatumGetChar(value);

                return hash_bytes_algo(h, &c, 1, algo);
            }
        case LFP_INT2:
            {
                uint16      v = (uint16) DatumGetInt16(value);
                unsigned char bytes[2];

                bytes[0] = (unsigned char) (v >> 8);
                bytes[1] = (unsigned char) v;
                return hash_bytes_algo(h, bytes, 2, algo);
            }
        case LFP_INT4:
            return logical_hash_be32(h, DatumGetUInt32(value), algo);
        case LFP_INT8:
            return logical_hash_be64(h, (uint64) DatumGetInt64(value), algo);
        case LFP_FLOAT4:
            {
                union
                {
                    float4      f;
                    uint32      i;
                }           swap;

                swap.f = DatumGetFloat4(value);
                return logical_hash_be32(h, swap.i, algo);
            }
        case LFP_FLOAT8:
            {
                union
                {
                    float8      f;
                    uint64      i;
                }           swap;

                swap.f = DatumGetFloat8(value);
                return logical_hash_be64(h, swap.i, algo);
            }
        case LFP_TIMETZ:
            {
                TimeTzADT  *t = DatumGetTimeTzADTP(value);

                h = logical_hash_be64(h, (uint64) t->time, algo);
                return logical_hash_be32(h, (uint32) t->zone, algo);
            }
        case LFP_INTERVAL:
            {
                Interval   *iv = DatumGetIntervalP(value);

                h = logical_hash_be64(h, (uint64) iv->time, algo);
                h = logical_hash_be32(h, (uint32) iv->day, algo);
                return logical_hash_be32(h, (uint32) iv->month, algo);
            }
        case LFP_UUID:
            return hash_bytes_algo(h, DatumGetUUIDP(value)->data, UUID_LEN,
                                   algo);
        case LFP_TEXT:
        case LFP_BYTEA:
            {
                struct varlena *v = PG_DETOAST_DATUM_PACKED(value);
                int         len = VARSIZE_ANY_EXHDR(v);

                if (info->fastpath == LFP_TEXT && text_needs_conversion(len))
                    h = logical_hash_send(h, PointerGetDatum(v), info, algo);
                else
                    h = hash_bytes_algo(h, VARDATA_ANY(v), len, algo);

                if ((Pointer) v != DatumGetPointer(value))
                    pfree(v);
                return h;
            }
        case LFP_NAME:
            {
                const char *str = NameStr(*DatumGetName(value));
                int         len = (int) strlen(str);

                if (text_needs_conversion(len))
                    return logical_hash_send(h, value, info, algo);
                return hash_bytes_algo(h, str, len, algo);
            }
        case LFP_NONE:
            break;
    }

    return logical_hash_send(h, value, info, algo);
}

static uint64
logical_hash_array(Datum value, int depth, ChecksumAlgo algo)
{
    /*
     * DatumGetArrayTypeP() returns a flat (detoasted, non-expanded) array,
     * and deconstruct_array() has had the same signature on every supported
     * release, unlike the array_iter API that changed in PostgreSQL 19.
     */
    ArrayType  *arr = DatumGetArrayTypeP(value);
    Oid         elemtype = ARR_ELEMTYPE(arr);
    LogicalTypeInfo *elem_info = logical_type_info(elemtype);
    LogicalColumnHint elem_hint = {0};
    int16       elem_typlen = elem_info->typlen;
    bool        elem_typbyval = elem_info->typbyval;
    char        elem_typalign = elem_info->typalign;
    Datum      *elems = NULL;
    bool       *elem_nulls = NULL;
    int         nelems;
    int         ndim = ARR_NDIM(arr);
    int         i;
    uint64      h = algo == CHECKSUM_ALGO_FNV32 ? FNV_BASIS_32 : FNV_BASIS_64;

    /* Hash the shape first: an empty array differs from an empty string. */
    h = logical_hash_uint32_le(h, (uint32) ndim, algo);
    if (ndim > 0)
    {
        for (i = 0; i < ndim; i++)
            h = logical_hash_uint32_le(h, (uint32) ARR_DIMS(arr)[i], algo);
        for (i = 0; i < ndim; i++)
            h = logical_hash_uint32_le(h, (uint32) ARR_LBOUND(arr)[i], algo);
    }

    nelems = (ndim > 0) ? ArrayGetNItems(ndim, ARR_DIMS(arr)) : 0;
    h = logical_hash_uint32_le(h, (uint32) nelems, algo);

    if (nelems > 0)
    {
        int         ndeconstructed;

        deconstruct_array(arr, elemtype, elem_typlen, elem_typbyval,
                          elem_typalign, &elems, &elem_nulls,
                          &ndeconstructed);
        Assert(ndeconstructed == nelems);

        for (i = 0; i < nelems; i++)
        {
            uint64      elem_hash;

            elem_hash = logical_hash_datum_algo(elems[i], elem_nulls[i],
                                                elemtype, -1, i + 1,
                                                depth + 1, algo, &elem_hint);
            h = combine_hash(h, elem_hash, algo);
        }

        pfree(elems);
        pfree(elem_nulls);
    }

    if ((Pointer) arr != DatumGetPointer(value))
        pfree(arr);

    return h;
}

static uint64
logical_hash_composite(Datum value, Oid typid, int32 typmod, int depth, ChecksumAlgo algo)
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
    uint64      h = algo == CHECKSUM_ALGO_FNV32 ? FNV_BASIS_32 : FNV_BASIS_64;
    int         non_dropped = 0;
    int         ordinal = 0;
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

        /* Position among the live fields, as for table columns. */
        ordinal++;

        field_hash = logical_hash_datum_algo(values[i], nulls[i],
                                             att->atttypid, att->atttypmod,
                                             ordinal, depth + 1, algo, NULL);
        h = combine_hash(h, field_hash, algo);
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
logical_hash_range(Datum value, Oid typid, int depth, ChecksumAlgo algo)
{
    RangeType  *range = DatumGetRangeTypeP(value);
    TypeCacheEntry *typcache = lookup_type_cache(typid, TYPECACHE_RANGE_INFO);
    RangeBound  lower;
    RangeBound  upper;
    bool        empty;
    uint64      h = algo == CHECKSUM_ALGO_FNV32 ? FNV_BASIS_32 : FNV_BASIS_64;

    range_deserialize(typcache, range, &lower, &upper, &empty);

    h = hash_bytes_algo(h, &empty, sizeof(empty), algo);

    if (!empty)
    {
        h = hash_bytes_algo(h, &lower.infinite, sizeof(lower.infinite), algo);
        h = hash_bytes_algo(h, &lower.inclusive, sizeof(lower.inclusive), algo);
        if (!lower.infinite)
        {
            uint64      lower_hash;

            lower_hash = logical_hash_datum_algo(lower.val, false,
                                                 typcache->rngelemtype->type_id,
                                                 -1, 1, depth + 1, algo, NULL);
            h = combine_hash(h, lower_hash, algo);
        }

        h = hash_bytes_algo(h, &upper.infinite, sizeof(upper.infinite), algo);
        h = hash_bytes_algo(h, &upper.inclusive, sizeof(upper.inclusive), algo);
        if (!upper.infinite)
        {
            uint64      upper_hash;

            upper_hash = logical_hash_datum_algo(upper.val, false,
                                                 typcache->rngelemtype->type_id,
                                                 -1, 2, depth + 1, algo, NULL);
            h = combine_hash(h, upper_hash, algo);
        }
    }

    if ((Pointer) range != DatumGetPointer(value))
        pfree(range);

    return h;
}

#if PG_VERSION_NUM >= 140000
static uint64
logical_hash_multirange(Datum value, Oid typid, int depth, ChecksumAlgo algo)
{
    MultirangeType *mr = DatumGetMultirangeTypeP(value);
    TypeCacheEntry *typcache = lookup_type_cache(typid,
                                                 TYPECACHE_MULTIRANGE_INFO);
    uint32      count = mr->rangeCount;
    uint64      h = algo == CHECKSUM_ALGO_FNV32 ? FNV_BASIS_32 : FNV_BASIS_64;
    uint32      i;

    h = logical_hash_uint32_le(h, count, algo);

    for (i = 0; i < count; i++)
    {
        RangeBound  lower;
        RangeBound  upper;
        uint64      lower_hash;
        uint64      upper_hash;

        multirange_get_bounds(typcache->rngtype, mr, i, &lower, &upper);

        h = hash_bytes_algo(h, &lower.infinite, sizeof(lower.infinite), algo);
        h = hash_bytes_algo(h, &lower.inclusive, sizeof(lower.inclusive), algo);
        if (!lower.infinite)
        {
            lower_hash = logical_hash_datum_algo(lower.val, false,
                                                 typcache->rngtype->rngelemtype->type_id,
                                                 -1, 1, depth + 1, algo, NULL);
            h = combine_hash(h, lower_hash, algo);
        }

        h = hash_bytes_algo(h, &upper.infinite, sizeof(upper.infinite), algo);
        h = hash_bytes_algo(h, &upper.inclusive, sizeof(upper.inclusive), algo);
        if (!upper.infinite)
        {
            upper_hash = logical_hash_datum_algo(upper.val, false,
                                                 typcache->rngtype->rngelemtype->type_id,
                                                 -1, 2, depth + 1, algo, NULL);
            h = combine_hash(h, upper_hash, algo);
        }
    }

    if ((Pointer) mr != DatumGetPointer(value))
        pfree(mr);

    return h;
}
#endif

static uint64
logical_hash_datum_algo(Datum value, bool isnull, Oid typid,
                        int32 typmod, int attnum, int depth, ChecksumAlgo algo,
                        LogicalColumnHint *hint)
{
    LogicalTypeInfo *type_info;
    char        typtype;
    Oid         elemtype;
    uint64      h;

    /* Only nested values recurse; a top-level cell needs no stack check. */
    if (depth > 0)
    {
        if (depth > 32)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("logical checksum recursion is too deep")));

        check_stack_depth();
    }

    if (isnull)
        return CHECKSUM_NULL;

    /*
     * Every value starts with its type tag and position.  A column hint
     * remembers the cache entry and that prefix, which saves a hash table
     * lookup and a hash combination per value when a scan hashes the same
     * column row after row.
     */
    if (hint != NULL && hint->info != NULL && hint->info->typid == typid &&
        hint->generation == logical_type_cache_generation)
    {
        type_info = hint->info;
        if (hint->attnum == attnum && hint->algo == algo)
            h = hint->prefix;
        else
        {
            h = logical_value_prefix(type_info, attnum, algo);
            hint->attnum = attnum;
            hint->algo = algo;
            hint->prefix = h;
        }
    }
    else
    {
        type_info = logical_type_info(typid);
        h = logical_value_prefix(type_info, attnum, algo);
        if (hint != NULL)
        {
            hint->info = type_info;
            hint->generation = type_info->generation;
            hint->attnum = attnum;
            hint->algo = algo;
            hint->prefix = h;
        }
    }

    /*
     * Copy what the structural cases need: they recurse, and the recursion
     * may rebuild cache entries in place.
     */
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
     *
     * The structural cases below are mutually exclusive in practice; the
     * historical encoding nevertheless tests each of them in turn, and that
     * is kept so the result cannot change.
     */
    if (OidIsValid(elemtype) ||
        typid == ANYARRAYOID
#if PG_VERSION_NUM >= 140000
        || typid == ANYCOMPATIBLEARRAYOID
#endif
        ||
        typtype == TYPTYPE_COMPOSITE || typid == RECORDOID ||
        typtype == TYPTYPE_RANGE
#if PG_VERSION_NUM >= 140000
        || typtype == TYPTYPE_MULTIRANGE
#endif
        )
    {
        if (OidIsValid(elemtype) ||
            typid == ANYARRAYOID
#if PG_VERSION_NUM >= 140000
            || typid == ANYCOMPATIBLEARRAYOID
#endif
            )
            h = combine_hash(h, logical_hash_array(value, depth, algo), algo);

        if (typtype == TYPTYPE_COMPOSITE || typid == RECORDOID)
            h = combine_hash(h,
                             logical_hash_composite(value, typid, typmod,
                                                    depth, algo), algo);

        if (typtype == TYPTYPE_RANGE)
            h = combine_hash(h, logical_hash_range(value, typid, depth, algo),
                             algo);

#if PG_VERSION_NUM >= 140000
        if (typtype == TYPTYPE_MULTIRANGE)
            h = combine_hash(h,
                             logical_hash_multirange(value, typid, depth, algo),
                             algo);
#endif
    }
    else if (typid == NUMERICOID)
    {
        Numeric     num = DatumGetNumeric(value);
        char       *str = numeric_normalize(num);

        h = hash_bytes_algo(h, str, strlen(str), algo);
        pfree(str);

        if ((Pointer) num != DatumGetPointer(value))
            pfree(num);
    }
    else if (typid == FLOAT4OID && isnan(DatumGetFloat4(value)))
        h = hash_bytes_algo(h, "NaN", 3, algo);
    else if (typid == FLOAT4OID && DatumGetFloat4(value) == 0)
    {
        /* +0 and -0 are equal; both hash as four zero bytes. */
        uint32      zero = 0;

        h = hash_bytes_algo(h, &zero, sizeof(zero), algo);
    }
    else if (typid == FLOAT8OID && isnan(DatumGetFloat8(value)))
        h = hash_bytes_algo(h, "NaN", 3, algo);
    else if (typid == FLOAT8OID && DatumGetFloat8(value) == 0)
    {
        uint64      zero = 0;

        h = hash_bytes_algo(h, &zero, sizeof(zero), algo);
    }
    else
        h = logical_hash_scalar(h, value, type_info, algo);

    if (algo == CHECKSUM_ALGO_FNV32)
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
 * pg_cell_checksum_canonical_algo - canonical logical cell checksum
 *
 * algo == CHECKSUM_ALGO_FNV32 selects the historical 32-bit FNV path;
 * CHECKSUM_ALGO_FNV64 selects FNV-1a 64-bit.  The canonicalization is shared
 * by both paths.
 */
uint64
pg_cell_checksum_canonical_algo(Datum value, bool isnull, Oid typid,
                                int32 typmod, int attnum, ChecksumAlgo algo)
{
    return logical_hash_datum_algo(value, isnull, typid, typmod, attnum, 0,
                                   algo, NULL);
}

/*
 * pg_cell_checksum_canonical_hinted - the same, for repeated calls
 *
 * "hint" is caller-owned state, zero-initialized, best kept per column; it
 * is only an accelerator and any type or attnum may be passed with it.
 */
uint64
pg_cell_checksum_canonical_hinted(Datum value, bool isnull, Oid typid,
                                  int32 typmod, int attnum, ChecksumAlgo algo,
                                  LogicalColumnHint *hint)
{
    return logical_hash_datum_algo(value, isnull, typid, typmod, attnum, 0,
                                   algo, hint);
}
