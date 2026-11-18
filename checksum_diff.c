/*-------------------------------------------------------------------------
 *
 * checksum_diff.c
 *    Logical checksums of key buckets, block ranges and single rows
 *
 * A table checksum only says *that* two copies of a table differ.  The
 * functions in this file split the same logical checksum into pieces so that
 * the differing rows can be found:
 *
 *  - pg_table_bucket_checksums() hashes the key of every visible row (the
 *    primary key by default) and returns one logical checksum per bucket of
 *    the key hash space.  Rows with the same key fall into the same bucket on
 *    every cluster, so comparing two lists of buckets narrows a difference
 *    down to a few buckets.
 *  - pg_table_row_checksums() returns the key, key hash and row checksum of
 *    every row in the selected buckets, so the rows of a differing bucket
 *    can be compared one by one.
 *  - pg_table_block_checksums() returns one logical checksum per range of
 *    heap blocks.  Block numbers are only comparable between a primary and
 *    its physical standbys, where the same rows sit at the same TIDs.
 *
 * Buckets partition the 64-bit key hash space into equal ranges: bucket b of
 * n holds the key hashes h with floor(h * n / 2^64) = b.  Bucket b of n is
 * therefore exactly the union of buckets b*k .. b*k+k-1 of n*k, so a
 * comparison can be refined by multiplying the bucket count.
 *
 * Row checksums are the same canonical logical row hashes that
 * pg_table_logical_checksum() aggregates, and each bucket is aggregated the
 * same way, so with one bucket the bucket checksum equals the table checksum.
 * Bucket scans hand chunks of blocks to parallel workers, like a table scan,
 * when pg_integrity_checksums.workers allows it; each participant keeps its
 * own bucket accumulators and adds them to a shared array at the end.  Row
 * and block range scans always run in the calling backend.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "checksum_internal.h"

/*
 * Upper bound for the number of buckets.  The bucket function keeps one
 * 16-byte accumulator per bucket in memory and returns one row per bucket.
 */
#define DIFF_MAX_BUCKETS (1 << 20)

typedef enum DiffMode
{
    DIFF_BUCKETS,               /* one accumulator per bucket */
    DIFF_ROWS                   /* one output row per row of chosen buckets */
} DiffMode;

/*
 * DiffScanState - state of a bucket or row scan over one table
 *
 * The key is a list of column names, resolved again in every leaf partition
 * (whose attribute numbers may differ from the queried table's).  nkeys is 0
 * when the whole row is the key.
 */
typedef struct DiffScanState
{
    DiffMode    mode;
    ChecksumAlgo algo;
    uint32      nbuckets;

    int         nkeys;
    char      **keynames;

    /* Per leaf relation, set up by diff_scan_relation(). */
    LogicalRowHasher hasher;
    int        *keyidx;         /* attribute index of each key column */
    LogicalColumnHint *keyhints;

    /* DIFF_BUCKETS */
    ChecksumLogicalAccum *acc32;
    ChecksumLogicalAccum64 *acc64;

    /* DIFF_ROWS */
    bool       *wanted;         /* NULL means every bucket */
    Tuplestorestate *store;
    TupleDesc   resultdesc;
    int         nkeyout;        /* number of columns in the key text */
    FmgrInfo   *keyout;         /* output function of each key column */
} DiffScanState;

/*-------------------------------------------------------------------------
 * Helpers
 *-------------------------------------------------------------------------
 */

/*
 * diff_bucket - bucket of a 64-bit key hash among nbuckets equal ranges
 *
 * Computes floor(hash * nbuckets / 2^64) without 128-bit arithmetic:
 * nbuckets < 2^31, so neither partial product overflows.
 */
static inline uint32
diff_bucket(uint64 hash, uint32 nbuckets)
{
    uint64      hi = hash >> 32;
    uint64      lo = hash & UINT64CONST(0xFFFFFFFF);

    return (uint32) ((hi * nbuckets + ((lo * nbuckets) >> 32)) >> 32);
}

/* diff_buckets_arg - validate the bucket count argument */
static uint32
diff_buckets_arg(FunctionCallInfo fcinfo, int argno)
{
    int32       buckets;

    if (PG_ARGISNULL(argno))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("number of buckets must not be NULL")));

    buckets = PG_GETARG_INT32(argno);
    if (buckets < 1 || buckets > DIFF_MAX_BUCKETS)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("number of buckets must be between 1 and %d",
                        DIFF_MAX_BUCKETS)));

    return (uint32) buckets;
}

/* diff_algo_arg - parse the algorithm argument, rejecting NULL */
static ChecksumAlgo
diff_algo_arg(FunctionCallInfo fcinfo, int argno)
{
    if (PG_ARGISNULL(argno))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("checksum algorithm must not be NULL")));

    return checksum_algorithm(PG_GETARG_TEXT_PP(argno));
}

/*
 * diff_find_column - attribute index of a live column, or -1
 */
static int
diff_find_column(TupleDesc tupdesc, const char *name)
{
    int         i;

    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

        if (!attr->attisdropped && strcmp(NameStr(attr->attname), name) == 0)
            return i;
    }

    return -1;
}

/*
 * diff_primary_key - names of the primary key columns, or NIL
 */
static List *
diff_primary_key(Relation rel)
{
    List       *indexes = RelationGetIndexList(rel);
    List       *result = NIL;
    ListCell   *lc;

    foreach(lc, indexes)
    {
        HeapTuple   tup;
        Form_pg_index index;
        int         i;

        tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lfirst_oid(lc)));
        if (!HeapTupleIsValid(tup))
            elog(ERROR, "cache lookup failed for index %u", lfirst_oid(lc));
        index = (Form_pg_index) GETSTRUCT(tup);

        if (index->indisprimary)
        {
            for (i = 0; i < index->indnkeyatts; i++)
            {
                AttrNumber  attnum = index->indkey.values[i];

                /* A primary key cannot contain expressions. */
                if (attnum <= 0 || attnum > RelationGetDescr(rel)->natts)
                    elog(ERROR, "unexpected attribute %d in primary key of \"%s\"",
                         attnum, RelationGetRelationName(rel));

                result = lappend(result,
                                 pstrdup(NameStr(TupleDescAttr(RelationGetDescr(rel),
                                                               attnum - 1)->attname)));
            }
            ReleaseSysCache(tup);
            break;
        }

        ReleaseSysCache(tup);
    }

    list_free(indexes);
    return result;
}

/*
 * diff_resolve_key - work out the key columns of the queried table
 *
 * A NULL key_columns argument selects the primary key, or the whole row when
 * there is none; an empty array selects the whole row.
 */
static void
diff_resolve_key(Relation rel, ArrayType *key_columns, DiffScanState *state)
{
    TupleDesc   tupdesc = RelationGetDescr(rel);
    List       *names = NIL;
    ListCell   *lc;
    int         k;

    if (key_columns == NULL)
        names = diff_primary_key(rel);
    else
    {
        Datum      *elems;
        bool       *elemnulls;
        int         nelems;
        int         i;

        if (ARR_NDIM(key_columns) > 1)
            ereport(ERROR,
                    (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                     errmsg("key columns must be a one-dimensional array")));

        deconstruct_array(key_columns, TEXTOID, -1, false, 'i',
                          &elems, &elemnulls, &nelems);

        for (i = 0; i < nelems; i++)
        {
            char       *name;

            if (elemnulls[i])
                ereport(ERROR,
                        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                         errmsg("key column names must not be NULL")));

            name = TextDatumGetCString(elems[i]);

            if (diff_find_column(tupdesc, name) < 0)
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_COLUMN),
                         errmsg("column \"%s\" of relation \"%s\" does not exist",
                                name, RelationGetRelationName(rel))));

            foreach(lc, names)
            {
                if (strcmp((char *) lfirst(lc), name) == 0)
                    ereport(ERROR,
                            (errcode(ERRCODE_DUPLICATE_COLUMN),
                             errmsg("key column \"%s\" specified more than once",
                                    name)));
            }

            names = lappend(names, name);
        }
    }

    state->nkeys = list_length(names);
    state->keynames = palloc(sizeof(char *) * Max(state->nkeys, 1));
    k = 0;
    foreach(lc, names)
        state->keynames[k++] = (char *) lfirst(lc);
    list_free(names);
}

/*
 * diff_init_key_output - look up the output functions of the key columns
 *
 * Done on the queried table; partitions have the same column types.  The
 * key text is built from them like record_out() would, without creating a
 * row type for the key.
 */
static void
diff_init_key_output(Relation rel, DiffScanState *state)
{
    TupleDesc   tupdesc = RelationGetDescr(rel);
    int         k;

    state->keyout = palloc(sizeof(FmgrInfo) * Max(tupdesc->natts, 1));
    state->nkeyout = 0;

    for (k = 0; k < (state->nkeys > 0 ? state->nkeys : tupdesc->natts); k++)
    {
        int         idx = k;
        Form_pg_attribute attr;
        Oid         outfunc;
        bool        isvarlena;

        if (state->nkeys > 0)
            idx = diff_find_column(tupdesc, state->keynames[k]);

        attr = TupleDescAttr(tupdesc, idx);
        if (attr->attisdropped)
            continue;

        getTypeOutputInfo(attr->atttypid, &outfunc, &isvarlena);
        fmgr_info(outfunc, &state->keyout[state->nkeyout++]);
    }
}

/*
 * diff_key_hash - 64-bit hash of the key of the row last deformed
 *
 * The key columns are hashed with the canonical logical encoding at their
 * position in the key, so the hash only depends on the key values.  The
 * whole-row key is the mixed 64-bit logical row checksum.  row_hash64 is
 * that checksum when the caller already has it, or 0.
 */
static uint64
diff_key_hash(DiffScanState *state, uint64 row_hash64)
{
    LogicalRowHasher *hasher = &state->hasher;
    TupleDesc   tupdesc = hasher->tupdesc;
    MemoryContext oldcontext;
    uint64      hash = FNV_BASIS_64;
    int         k;

    if (state->nkeys == 0)
    {
        if (row_hash64 == 0)
            row_hash64 = logical_row_hash_values(hasher, CHECKSUM_ALGO_FNV64);
        return hash_mix64(row_hash64);
    }

    oldcontext = MemoryContextSwitchTo(hasher->rowcxt);

    for (k = 0; k < state->nkeys; k++)
    {
        int         i = state->keyidx[k];
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        uint64      col_hash;

        col_hash = pg_cell_checksum_canonical_hinted(hasher->values[i],
                                                     hasher->nulls[i],
                                                     attr->atttypid,
                                                     attr->atttypmod,
                                                     k + 1,
                                                     CHECKSUM_ALGO_FNV64,
                                                     &state->keyhints[k]);
        hash = combine_checksums_64(hash, col_hash);
    }

    MemoryContextSwitchTo(oldcontext);

    return hash_mix64(hash);
}

/*
 * diff_key_text - the key of the row last deformed, as record text
 *
 * Produces what record_out() prints for a row of the key columns: values in
 * parentheses separated by commas, NULL as nothing, and a value quoted (with
 * quotes and backslashes doubled) when it is empty or contains a quote,
 * backslash, parenthesis, comma or white space.
 */
static text *
diff_key_text(DiffScanState *state)
{
    LogicalRowHasher *hasher = &state->hasher;
    MemoryContext oldcontext;
    StringInfoData buf;
    text       *result;
    int         k;

    oldcontext = MemoryContextSwitchTo(hasher->rowcxt);

    initStringInfo(&buf);
    appendStringInfoChar(&buf, '(');

    for (k = 0; k < state->nkeyout; k++)
    {
        int         i = state->nkeys > 0 ? state->keyidx[k] : hasher->attidx[k];
        char       *value;
        char       *p;
        bool        quote;

        if (k > 0)
            appendStringInfoChar(&buf, ',');

        if (hasher->nulls[i])
            continue;

        value = OutputFunctionCall(&state->keyout[k], hasher->values[i]);

        quote = (value[0] == '\0');
        for (p = value; *p && !quote; p++)
        {
            char        ch = *p;

            if (ch == '"' || ch == '\\' || ch == '(' || ch == ')' ||
                ch == ',' || isspace((unsigned char) ch))
                quote = true;
        }

        if (!quote)
        {
            appendStringInfoString(&buf, value);
            continue;
        }

        appendStringInfoChar(&buf, '"');
        for (p = value; *p; p++)
        {
            if (*p == '"' || *p == '\\')
                appendStringInfoChar(&buf, *p);
            appendStringInfoChar(&buf, *p);
        }
        appendStringInfoChar(&buf, '"');
    }

    appendStringInfoChar(&buf, ')');
    result = cstring_to_text_with_len(buf.data, buf.len);

    MemoryContextSwitchTo(oldcontext);

    return result;
}

/*-------------------------------------------------------------------------
 * Scans
 *-------------------------------------------------------------------------
 */

/* Visitor for bucket and row scans; the buffer is only pinned. */
static void
diff_tuple_visitor(Relation rel, Page page, BlockNumber blkno,
                   OffsetNumber offnum, HeapTuple tuple, void *arg)
{
    DiffScanState *state = (DiffScanState *) arg;
    LogicalRowHasher *hasher = &state->hasher;
    uint64      row_hash = 0;
    uint64      key_hash;
    uint32      bucket;

    logical_row_deform(hasher, tuple);

    /* The whole-row key reuses the 64-bit row checksum. */
    if (state->nkeys == 0 && state->algo == CHECKSUM_ALGO_FNV64)
        row_hash = logical_row_hash_values(hasher, CHECKSUM_ALGO_FNV64);

    key_hash = diff_key_hash(state, row_hash);
    bucket = diff_bucket(key_hash, state->nbuckets);

    if (state->mode == DIFF_ROWS && state->wanted != NULL &&
        !state->wanted[bucket])
    {
        MemoryContextReset(hasher->rowcxt);
        return;
    }

    if (row_hash == 0)
        row_hash = logical_row_hash_values(hasher, state->algo);

    if (state->mode == DIFF_BUCKETS)
    {
        if (checksum_algo_is32(state->algo))
            logical_accum_add(&state->acc32[bucket], (uint32) row_hash);
        else
            logical_accum_add64(&state->acc64[bucket], row_hash);
    }
    else
    {
        Datum       values[6];
        bool        nulls[6] = {false, false, false, false, false, false};
        ItemPointerData tid;
        MemoryContext oldcontext;

        ItemPointerSet(&tid, blkno, offnum);

        oldcontext = MemoryContextSwitchTo(hasher->rowcxt);
        values[0] = Int32GetDatum((int32) bucket);
        values[1] = PointerGetDatum(checksum_bytea(key_hash, 8));
        values[2] = PointerGetDatum(diff_key_text(state));
        values[3] = PointerGetDatum(checksum_bytea(row_hash,
                                                   checksum_algo_len(state->algo)));
        values[4] = PointerGetDatum(&tid);
        values[5] = ObjectIdGetDatum(RelationGetRelid(rel));
        MemoryContextSwitchTo(oldcontext);

        tuplestore_putvalues(state->store, state->resultdesc, values, nulls);
    }

    MemoryContextReset(hasher->rowcxt);
}

/*
 * diff_leaf_begin - prepare the row hasher and key columns for one heap
 * relation
 */
static void
diff_leaf_begin(Relation rel, TupleDesc order_desc, DiffScanState *state)
{
    int         k;

    logical_row_hasher_init(&state->hasher, RelationGetDescr(rel), order_desc);

    state->keyidx = palloc(sizeof(int) * Max(state->nkeys, 1));
    state->keyhints = palloc0(sizeof(LogicalColumnHint) * Max(state->nkeys, 1));
    for (k = 0; k < state->nkeys; k++)
    {
        state->keyidx[k] = diff_find_column(RelationGetDescr(rel),
                                            state->keynames[k]);
        if (state->keyidx[k] < 0)
            elog(ERROR, "key column \"%s\" was not found in relation \"%s\"",
                 state->keynames[k], RelationGetRelationName(rel));
    }
}

/* diff_leaf_end - release what diff_leaf_begin() set up */
static void
diff_leaf_end(DiffScanState *state)
{
    pfree(state->keyidx);
    pfree(state->keyhints);
    logical_row_hasher_free(&state->hasher);
}

/*
 * diff_bucket_workers - number of parallel workers for a bucket scan of one
 * heap relation, 0 to scan it in the calling backend
 */
static int
diff_bucket_workers(Relation rel, BlockNumber nblocks)
{
    int         workers = pg_integrity_checksums_worker_count_for(rel);
    uint32      chunks;

    if (workers <= 0 || nblocks == 0)
        return 0;

    /* Do not start more workers than there are chunks of blocks. */
    chunks = nblocks / PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK +
        (nblocks % PG_INTEGRITY_CHECKSUMS_BLOCK_CHUNK != 0);
    if ((uint32) workers > chunks)
        workers = (int) chunks;

    return workers;
}

/*
 * diff_scan_relation - scan one heap relation (or the leaves of a
 * partitioned table) into the bucket accumulators or the result rows
 *
 * order_desc is the descriptor of the queried table when rel is one of its
 * partitions: rows are hashed in the queried table's column order and key
 * columns are found by name.
 *
 * A bucket scan of a relation whose rows are hashed in their own column
 * order uses parallel workers when pg_integrity_checksums.workers allows it;
 * row scans always run in the calling backend.
 */
static void
diff_scan_relation(Relation rel, TupleDesc order_desc, Snapshot snapshot,
                   DiffScanState *state)
{
    TupleDesc   tupdesc = RelationGetDescr(rel);
    BufferAccessStrategy bstrategy;
    ChecksumBlockSource src;
    BlockNumber nblocks;
    int         workers = 0;

    if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
    {
        List       *children = find_all_inheritors(RelationGetRelid(rel),
                                                   AccessShareLock, NULL);
        ListCell   *lc;

        foreach(lc, children)
        {
            Oid         childoid = lfirst_oid(lc);
            Relation    child;

            if (get_rel_relkind(childoid) != RELKIND_RELATION)
                continue;

            child = relation_open(childoid, AccessShareLock);
            pg_integrity_checksums_check_relation_kind(child, false, false);
            diff_scan_relation(child, order_desc ? order_desc : tupdesc,
                               snapshot, state);
            relation_close(child, AccessShareLock);
        }

        list_free(children);
        return;
    }

    if (order_desc != NULL && logical_columns_same_order(tupdesc, order_desc))
        order_desc = NULL;

    nblocks = RelationGetNumberOfBlocks(rel);

    /*
     * Workers hash rows in the relation's own column order; a partition
     * whose columns are in another order than the queried table's is
     * scanned here.
     */
    if (state->mode == DIFF_BUCKETS && order_desc == NULL)
        workers = diff_bucket_workers(rel, nblocks);

    if (workers > 0)
    {
        pg_integrity_checksums_parallel_scan_buckets(RelationGetRelid(rel),
                                                     nblocks, workers,
                                                     state->algo,
                                                     state->nbuckets,
                                                     state->nkeys,
                                                     state->keynames,
                                                     snapshot,
                                                     state->acc32,
                                                     state->acc64);
        return;
    }

    diff_leaf_begin(rel, order_desc, state);

    bstrategy = GetAccessStrategy(BAS_BULKREAD);
    checksum_block_source_range(&src, 0, nblocks);
    scan_visible_tuples(rel, &src, snapshot, bstrategy, false,
                        diff_tuple_visitor, state);
    FreeAccessStrategy(bstrategy);

    diff_leaf_end(state);
}

/*
 * pg_table_bucket_parallel_task - one participant's share of a parallel
 * bucket scan
 *
 * Run by the leader and by every worker of
 * pg_integrity_checksums_parallel_scan_buckets(): claims chunks of blocks of
 * pstate->reloid until none remain, accumulates the visible rows into
 * private per-bucket accumulators and then adds those to the shared
 * (sum, count) pairs.
 */
void
pg_table_bucket_parallel_task(ChecksumParallelState *pstate, char **keynames,
                              pg_atomic_uint64 *shared, Snapshot snapshot)
{
    DiffScanState state;
    Relation    rel;
    BufferAccessStrategy bstrategy;
    ChecksumBlockSource src;
    uint32      b;

    memset(&state, 0, sizeof(state));
    state.mode = DIFF_BUCKETS;
    state.algo = pstate->algo;
    state.nbuckets = pstate->nbuckets;
    state.nkeys = pstate->nkeys;
    state.keynames = keynames;

    if (checksum_algo_is32(state.algo))
        state.acc32 = palloc0(sizeof(ChecksumLogicalAccum) * state.nbuckets);
    else
        state.acc64 = palloc0(sizeof(ChecksumLogicalAccum64) * state.nbuckets);

    rel = relation_open(pstate->reloid, AccessShareLock);
    diff_leaf_begin(rel, NULL, &state);
    bstrategy = GetAccessStrategy(BAS_BULKREAD);

    checksum_block_source_shared(&src, pstate);
    scan_visible_tuples(rel, &src, snapshot, bstrategy, false,
                        diff_tuple_visitor, &state);

    FreeAccessStrategy(bstrategy);
    diff_leaf_end(&state);
    relation_close(rel, AccessShareLock);

    for (b = 0; b < state.nbuckets; b++)
    {
        uint64      sum;
        uint64      count;

        if (checksum_algo_is32(state.algo))
        {
            sum = state.acc32[b].sum;
            count = state.acc32[b].count;
        }
        else
        {
            sum = state.acc64[b].sum;
            count = state.acc64[b].count;
        }

        if (count == 0)
            continue;

        /* Two's complement: adding the reinterpreted value adds mod 2^64. */
        pg_atomic_fetch_add_u64(&shared[2 * b], (int64) sum);
        pg_atomic_fetch_add_u64(&shared[2 * b + 1], (int64) count);
    }

    if (state.acc32)
        pfree(state.acc32);
    if (state.acc64)
        pfree(state.acc64);
}

/*
 * diff_open_table - open the queried table and check that it can be scanned
 */
static Relation
diff_open_table(Oid reloid, bool allow_partitioned)
{
    Relation    rel = relation_open(reloid, AccessShareLock);

    pg_integrity_checksums_require_select(reloid);
    pg_integrity_checksums_check_relation_kind(rel, false, allow_partitioned);

    return rel;
}

/*-------------------------------------------------------------------------
 * SQL functions
 *-------------------------------------------------------------------------
 */

/*
 * pg_table_bucket_checksums(relname regclass, buckets integer,
 *                           key_columns text[], algorithm text)
 *   RETURNS TABLE(bucket integer, row_count bigint, checksum bytea)
 *
 * One row per bucket, empty buckets included, in bucket order.
 */
PG_FUNCTION_INFO_V1(pg_table_bucket_checksums);

Datum
pg_table_bucket_checksums(PG_FUNCTION_ARGS)
{
    DiffScanState state;
    TupleDesc   resultdesc;
    Tuplestorestate *store;
    Relation    rel;
    uint32      b;

    store = pg_integrity_checksums_begin_srf(fcinfo, &resultdesc);

    /* Like a STRICT function: no relation, no rows. */
    if (PG_ARGISNULL(0))
        return (Datum) 0;

    memset(&state, 0, sizeof(state));
    state.mode = DIFF_BUCKETS;
    state.nbuckets = diff_buckets_arg(fcinfo, 1);
    state.algo = diff_algo_arg(fcinfo, 3);

    rel = diff_open_table(PG_GETARG_OID(0), true);
    diff_resolve_key(rel, PG_ARGISNULL(2) ? NULL : PG_GETARG_ARRAYTYPE_P(2),
                     &state);

    if (checksum_algo_is32(state.algo))
        state.acc32 = palloc0(sizeof(ChecksumLogicalAccum) * state.nbuckets);
    else
        state.acc64 = palloc0(sizeof(ChecksumLogicalAccum64) * state.nbuckets);

    diff_scan_relation(rel, NULL, pg_integrity_checksums_get_snapshot(),
                       &state);
    relation_close(rel, AccessShareLock);

    for (b = 0; b < state.nbuckets; b++)
    {
        Datum       values[3];
        bool        nulls[3] = {false, false, false};

        values[0] = Int32GetDatum((int32) b);
        if (checksum_algo_is32(state.algo))
        {
            values[1] = Int64GetDatum((int64) state.acc32[b].count);
            values[2] = PointerGetDatum(checksum_bytea(logical_accum_finalize(&state.acc32[b]), 4));
        }
        else
        {
            values[1] = Int64GetDatum((int64) state.acc64[b].count);
            values[2] = PointerGetDatum(checksum_bytea(logical_accum_finalize64(&state.acc64[b]), 8));
        }

        tuplestore_putvalues(store, resultdesc, values, nulls);
        pfree(DatumGetPointer(values[2]));

        if ((b & 1023) == 0)
            CHECK_FOR_INTERRUPTS();
    }

    return (Datum) 0;
}

/*
 * pg_table_row_checksums(relname regclass, buckets integer,
 *                        only_buckets integer[], key_columns text[],
 *                        algorithm text)
 *   RETURNS TABLE(bucket integer, key_hash bytea, key text,
 *                 row_checksum bytea, ctid tid, tableoid regclass)
 *
 * One row per visible row whose key falls into one of only_buckets (every
 * row when only_buckets is NULL), in physical order.
 */
PG_FUNCTION_INFO_V1(pg_table_row_checksums);

Datum
pg_table_row_checksums(PG_FUNCTION_ARGS)
{
    DiffScanState state;
    Relation    rel;

    memset(&state, 0, sizeof(state));
    state.store = pg_integrity_checksums_begin_srf(fcinfo, &state.resultdesc);

    /* Like a STRICT function: no relation, no rows. */
    if (PG_ARGISNULL(0))
        return (Datum) 0;

    state.mode = DIFF_ROWS;
    state.nbuckets = diff_buckets_arg(fcinfo, 1);
    state.algo = diff_algo_arg(fcinfo, 4);

    if (!PG_ARGISNULL(2))
    {
        ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(2);
        Datum      *elems;
        bool       *elemnulls;
        int         nelems;
        int         i;

        if (ARR_NDIM(arr) > 1)
            ereport(ERROR,
                    (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                     errmsg("buckets must be a one-dimensional array")));

        deconstruct_array(arr, INT4OID, sizeof(int32), true, 'i',
                          &elems, &elemnulls, &nelems);

        state.wanted = palloc0(sizeof(bool) * state.nbuckets);
        for (i = 0; i < nelems; i++)
        {
            int32       b;

            if (elemnulls[i])
                ereport(ERROR,
                        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                         errmsg("bucket numbers must not be NULL")));

            b = DatumGetInt32(elems[i]);
            if (b < 0 || (uint32) b >= state.nbuckets)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("bucket %d is out of range for %u buckets",
                                b, state.nbuckets)));

            state.wanted[b] = true;
        }
    }

    rel = diff_open_table(PG_GETARG_OID(0), true);
    diff_resolve_key(rel, PG_ARGISNULL(3) ? NULL : PG_GETARG_ARRAYTYPE_P(3),
                     &state);
    diff_init_key_output(rel, &state);

    diff_scan_relation(rel, NULL, pg_integrity_checksums_get_snapshot(),
                       &state);
    relation_close(rel, AccessShareLock);

    return (Datum) 0;
}

/* Visitor for block range scans; the buffer is only pinned. */
typedef struct BlockRangeState
{
    ChecksumAlgo algo;
    LogicalRowHasher hasher;
    ChecksumLogicalAccum acc32;
    ChecksumLogicalAccum64 acc64;
} BlockRangeState;

static void
block_range_visitor(Relation rel, Page page, BlockNumber blkno,
                    OffsetNumber offnum, HeapTuple tuple, void *arg)
{
    BlockRangeState *state = (BlockRangeState *) arg;
    uint64      row_hash = logical_row_hash(&state->hasher, tuple, state->algo);

    if (checksum_algo_is32(state->algo))
        logical_accum_add(&state->acc32, (uint32) row_hash);
    else
        logical_accum_add64(&state->acc64, row_hash);
}

/*
 * pg_table_block_checksums(relname regclass, blocks_per_range integer,
 *                          algorithm text)
 *   RETURNS TABLE(start_block bigint, end_block bigint, row_count bigint,
 *                 checksum bytea)
 *
 * One row per range of blocks_per_range heap blocks, covering the whole
 * relation; end_block is exclusive and always start_block +
 * blocks_per_range, so the ranges line up between servers whose relation
 * has a different length.
 */
PG_FUNCTION_INFO_V1(pg_table_block_checksums);

Datum
pg_table_block_checksums(PG_FUNCTION_ARGS)
{
    TupleDesc   resultdesc;
    Tuplestorestate *store;
    Relation    rel;
    BlockRangeState state;
    BufferAccessStrategy bstrategy;
    ChecksumBlockSource src;
    Snapshot    snapshot;
    BlockNumber nblocks;
    int32       per_range;
    uint64      start;

    store = pg_integrity_checksums_begin_srf(fcinfo, &resultdesc);

    /* Like a STRICT function: no relation, no rows. */
    if (PG_ARGISNULL(0))
        return (Datum) 0;

    if (PG_ARGISNULL(1))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("blocks per range must not be NULL")));
    per_range = PG_GETARG_INT32(1);
    if (per_range < 1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("blocks per range must be at least 1")));

    memset(&state, 0, sizeof(state));
    state.algo = diff_algo_arg(fcinfo, 2);

    rel = diff_open_table(PG_GETARG_OID(0), false);
    snapshot = pg_integrity_checksums_get_snapshot();
    nblocks = RelationGetNumberOfBlocks(rel);
    bstrategy = GetAccessStrategy(BAS_BULKREAD);
    logical_row_hasher_init(&state.hasher, RelationGetDescr(rel), NULL);

    for (start = 0; start < nblocks; start += (uint64) per_range)
    {
        uint64      end = start + (uint64) per_range;
        Datum       values[4];
        bool        nulls[4] = {false, false, false, false};
        uint64      checksum;
        uint64      count;

        memset(&state.acc32, 0, sizeof(state.acc32));
        memset(&state.acc64, 0, sizeof(state.acc64));

        checksum_block_source_range(&src, (BlockNumber) start,
                                    (BlockNumber) Min(end, (uint64) nblocks));
        scan_visible_tuples(rel, &src, snapshot, bstrategy, false,
                            block_range_visitor, &state);

        if (checksum_algo_is32(state.algo))
        {
            checksum = logical_accum_finalize(&state.acc32);
            count = state.acc32.count;
        }
        else
        {
            checksum = logical_accum_finalize64(&state.acc64);
            count = state.acc64.count;
        }

        values[0] = Int64GetDatum((int64) start);
        values[1] = Int64GetDatum((int64) end);
        values[2] = Int64GetDatum((int64) count);
        values[3] = PointerGetDatum(checksum_bytea(checksum,
                                                   checksum_algo_len(state.algo)));

        tuplestore_putvalues(store, resultdesc, values, nulls);
        pfree(DatumGetPointer(values[3]));
    }

    logical_row_hasher_free(&state.hasher);
    FreeAccessStrategy(bstrategy);
    relation_close(rel, AccessShareLock);

    return (Datum) 0;
}
