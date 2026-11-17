/*-------------------------------------------------------------------------
 *
 * checksum_sequence.c
 *    Logical checksum of a sequence: its definition and current state
 *
 * A sequence has no rows to scan: its state is a single tuple in block 0
 * (last_value, log_cnt, is_called) and its definition lives in pg_sequence.
 * The logical checksum hashes the data type, the parameters from
 * pg_sequence and last_value/is_called, which is exactly what pg_dump
 * restores with CREATE SEQUENCE and setval().  log_cnt is left out: it
 * counts values pre-logged to WAL and changes without the sequence changing.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/xlog.h"
#include "lib/stringinfo.h"
#include "catalog/pg_class.h"
#include "catalog/pg_sequence.h"
#include "commands/sequence.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "checksum_internal.h"

/*
 * The special space of a sequence page holds this magic number.  It is
 * private to sequence.c before PostgreSQL 18.
 */
#ifndef SEQ_MAGIC
#define SEQ_MAGIC 0x1717
#endif

/* Encoding version, part of the hashed bytes. */
#define SEQUENCE_ENCODING_TAG "sequence/1"

/* The state stored in block 0 of a sequence. */
typedef struct SequenceState
{
    int64       last_value;
    bool        is_called;
} SequenceState;

/*
 * Little-endian append helpers: the encoding must not depend on the
 * server's byte order.
 */
static void
seq_append_bytes(StringInfo buf, const void *data, int len)
{
    appendBinaryStringInfo(buf, (const char *) data, len);
}

static void
seq_append_uint32(StringInfo buf, uint32 value)
{
    unsigned char bytes[4];
    int         i;

    for (i = 0; i < 4; i++)
        bytes[i] = (unsigned char) ((value >> (i * 8)) & 0xFF);
    seq_append_bytes(buf, bytes, 4);
}

static void
seq_append_int64(StringInfo buf, int64 value)
{
    uint64      v = (uint64) value;
    unsigned char bytes[8];
    int         i;

    for (i = 0; i < 8; i++)
        bytes[i] = (unsigned char) ((v >> (i * 8)) & 0xFF);
    seq_append_bytes(buf, bytes, 8);
}

static void
seq_append_bool(StringInfo buf, bool value)
{
    unsigned char b = value ? 1 : 0;

    seq_append_bytes(buf, &b, 1);
}

static void
seq_append_string(StringInfo buf, const char *s)
{
    uint32      len = (uint32) strlen(s);

    seq_append_uint32(buf, len);
    seq_append_bytes(buf, s, (int) len);
}

/*
 * read_sequence_state - read last_value and is_called from block 0
 *
 * Mirrors read_seq_tuple() in sequence.c, but with a share lock: nextval()
 * and setval() modify the tuple under an exclusive buffer lock, so a share
 * lock sees a consistent state.  The page is validated before the tuple is
 * dereferenced, so a damaged sequence raises an error instead of crashing.
 */
static void
read_sequence_state(Relation rel, SequenceState *state)
{
    Buffer      buf;
    Page        page;
    PageHeader  phdr;
    ItemId      lp;
    HeapTupleData tuple;
    Form_pg_sequence_data seq;
    bool        ok = false;

    buf = ReadBuffer(rel, 0);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);
    phdr = (PageHeader) page;

    if (!PageIsNew(page) &&
        pg_integrity_checksums_page_header_sane(page) &&
        BLCKSZ - phdr->pd_special >= sizeof(uint32) &&
        *((uint32 *) PageGetSpecialPointer(page)) == SEQ_MAGIC &&
        PageGetMaxOffsetNumber(page) >= FirstOffsetNumber)
    {
        lp = PageGetItemId(page, FirstOffsetNumber);
        if (ItemIdIsNormal(lp) &&
            ItemIdGetOffset(lp) + ItemIdGetLength(lp) <= phdr->pd_special &&
            ItemIdGetLength(lp) >= SizeofHeapTupleHeader)
        {
            tuple.t_data = (HeapTupleHeader) PageGetItem(page, lp);
            tuple.t_len = ItemIdGetLength(lp);
            /* The tuple is not padded to sizeof(FormData_pg_sequence_data). */
            if (tuple.t_data->t_hoff >= SizeofHeapTupleHeader &&
                tuple.t_data->t_hoff +
                offsetof(FormData_pg_sequence_data, is_called) + sizeof(bool) <=
                tuple.t_len)
            {
                seq = (Form_pg_sequence_data) GETSTRUCT(&tuple);
                state->last_value = seq->last_value;
                state->is_called = seq->is_called;
                ok = true;
            }
        }
    }

    UnlockReleaseBuffer(buf);

    if (!ok)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("invalid page in sequence \"%s\"",
                        RelationGetRelationName(rel))));
}

/*
 * pg_sequence_logical_checksum_common - logical sequence checksum
 *
 * Returns the checksum, or sets *is_null when the state cannot be read: an
 * unlogged sequence on a standby has no main fork to read.
 */
static uint64
pg_sequence_logical_checksum_common(Oid seqoid, ChecksumAlgo algo,
                                    bool *is_null)
{
    Relation    rel;
    HeapTuple   seqtup;
    Form_pg_sequence seqform;
    SequenceState state;
    StringInfoData buf;
    uint64      item;
    uint64      result;

    *is_null = false;

    rel = relation_open(seqoid, AccessShareLock);

    if (rel->rd_rel->relkind != RELKIND_SEQUENCE)
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("relation \"%s\" is not a sequence",
                        RelationGetRelationName(rel))));

    pg_integrity_checksums_require_select(seqoid);

    if (RELATION_IS_OTHER_TEMP(rel))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("cannot access temporary sequences of other sessions")));

    /* An unlogged sequence has no state on a standby, as for setval(). */
    if (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
        RecoveryInProgress())
    {
        relation_close(rel, AccessShareLock);
        *is_null = true;
        return 0;
    }

    seqtup = SearchSysCache1(SEQRELID, ObjectIdGetDatum(seqoid));
    if (!HeapTupleIsValid(seqtup))
        elog(ERROR, "cache lookup failed for sequence %u", seqoid);
    seqform = (Form_pg_sequence) GETSTRUCT(seqtup);

    read_sequence_state(rel, &state);

    initStringInfo(&buf);
    seq_append_string(&buf, SEQUENCE_ENCODING_TAG);
    /* The type name, not its OID; always a built-in integer type. */
    seq_append_string(&buf, format_type_be(seqform->seqtypid));
    seq_append_int64(&buf, seqform->seqstart);
    seq_append_int64(&buf, seqform->seqincrement);
    seq_append_int64(&buf, seqform->seqmax);
    seq_append_int64(&buf, seqform->seqmin);
    seq_append_int64(&buf, seqform->seqcache);
    seq_append_bool(&buf, seqform->seqcycle);
    seq_append_int64(&buf, state.last_value);
    seq_append_bool(&buf, state.is_called);

    ReleaseSysCache(seqtup);
    relation_close(rel, AccessShareLock);

    /*
     * Fold the single item through the logical accumulator, like a one-row
     * table, so the result gets the same finalizer (and never collides with
     * the values reserved for NULL).
     */
    item = hash_bytes_algo(0, buf.data, buf.len, algo);
    pfree(buf.data);

    if (checksum_algo_is32(algo))
    {
        ChecksumLogicalAccum acc = {0, 0};

        logical_accum_add(&acc, (uint32) item);
        result = logical_accum_finalize(&acc);
    }
    else
    {
        ChecksumLogicalAccum64 acc = {0, 0};

        logical_accum_add64(&acc, item);
        result = logical_accum_finalize64(&acc);
    }

    return result;
}

/*
 * pg_sequence_logical_checksum - 32-bit logical checksum of a sequence
 */
PG_FUNCTION_INFO_V1(pg_sequence_logical_checksum);

Datum
pg_sequence_logical_checksum(PG_FUNCTION_ARGS)
{
    bool        is_null;
    uint64      result;

    result = pg_sequence_logical_checksum_common(PG_GETARG_OID(0),
                                                 CHECKSUM_ALGO_FNV32,
                                                 &is_null);
    if (is_null)
        PG_RETURN_NULL();

    PG_RETURN_INT32((int32) (uint32) result);
}

/*
 * pg_sequence_logical_checksum_algo - generic logical sequence checksum
 */
PG_FUNCTION_INFO_V1(pg_sequence_logical_checksum_algo);

Datum
pg_sequence_logical_checksum_algo(PG_FUNCTION_ARGS)
{
    ChecksumAlgo algorithm;
    bool        is_null;
    uint64      result;

    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(1));
    result = pg_sequence_logical_checksum_common(PG_GETARG_OID(0), algorithm,
                                                 &is_null);
    if (is_null)
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(result, checksum_algo_len(algorithm)));
}
