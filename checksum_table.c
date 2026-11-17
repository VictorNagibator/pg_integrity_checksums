/*-------------------------------------------------------------------------
 *
 * checksum_table.c
 *    Physical and logical table checksums, including partitions
 *
 * Every checksum exists in a 32-bit and a 64-bit flavor.  Both flavors share
 * one implementation that is parameterized by ChecksumAlgo; the *_calc() and
 * *_calc64() functions are thin wrappers that keep the result types of their
 * callers.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "catalog/pg_inherits.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "checksum_internal.h"

static uint64 pg_table_physical_checksum_common(Oid reloid,
                                                bool include_header,
                                                bool check_privileges,
                                                ChecksumAlgo algo);
static uint64 pg_table_logical_checksum_common(Oid reloid,
                                               bool check_privileges,
                                               ChecksumAlgo algo,
                                               bool *is_null);
static void pg_table_logical_checksum_accum(Oid reloid, bool check_privileges,
                                            ChecksumAlgo algo,
                                            ChecksumLogicalAccum *acc32,
                                            ChecksumLogicalAccum64 *acc64,
                                            bool *is_null,
                                            TupleDesc order_desc);

/*-------------------------------------------------------------------------
 * Partitioned Table Helpers
 *-------------------------------------------------------------------------
 */

/*
 * pg_integrity_checksums_leaf_partitions - return the OIDs of all leaf partitions
 * of a partitioned table
 *
 * Uses find_all_inheritors to collect every descendant of the given
 * relation and keeps only the leaf partitions (ordinary tables). Foreign
 * table partitions are skipped because they have no local storage to
 * checksum. The list is empty for a partitioned table with no partitions.
 */
static List *
pg_integrity_checksums_leaf_partitions(Oid reloid)
{
    List       *children;
    List       *leaves = NIL;
    ListCell   *lc;

    children = find_all_inheritors(reloid, AccessShareLock, NULL);

    foreach(lc, children)
    {
        Oid         childoid = lfirst_oid(lc);

        if (childoid == reloid)
            continue;

        if (get_rel_relkind(childoid) == RELKIND_RELATION)
            leaves = lappend_oid(leaves, childoid);
    }

    list_free(children);

    return leaves;
}

/*
 * pg_integrity_checksums_leaf_indexes - return the OIDs of all leaf indexes of a
 * partitioned index
 *
 * Intermediate partitioned indexes are skipped; only ordinary indexes have
 * storage to checksum.  The list is empty for a partitioned index with no
 * partitions.
 */
List *
pg_integrity_checksums_leaf_indexes(Oid indexoid)
{
    List       *children;
    List       *leaves = NIL;
    ListCell   *lc;

    children = find_all_inheritors(indexoid, AccessShareLock, NULL);

    foreach(lc, children)
    {
        Oid         childoid = lfirst_oid(lc);

        if (childoid == indexoid)
            continue;

        if (get_rel_relkind(childoid) == RELKIND_INDEX)
            leaves = lappend_oid(leaves, childoid);
    }

    list_free(children);

    return leaves;
}

/*-------------------------------------------------------------------------
 * TABLE LEVEL FUNCTIONS (Physical and Logical)
 *-------------------------------------------------------------------------
 */

/*
 * pg_table_physical_checksum - SQL function for physical table checksums
 *
 * Computes a physical checksum for an entire table by aggregating
 * physical checksums of all tuples. Uses order-independent aggregation
 * to ensure the same checksum regardless of scan order.
 *
 * Returns a 64-bit value: the upper 32 bits hold the aggregate of all
 * tuple checksums combined with the relation identity (OID, relfilenode and
 * tablespace); the lower 32 bits hold the relation OID.  Including the
 * identity ensures that even empty tables have a distinct checksum.
 */
PG_FUNCTION_INFO_V1(pg_table_physical_checksum);

Datum
pg_table_physical_checksum(PG_FUNCTION_ARGS)
{
    Oid         reloid;
    bool        include_header;

    reloid = PG_GETARG_OID(0);
    include_header = PG_GETARG_BOOL(1);

    PG_RETURN_INT64((int64)
                    pg_table_physical_checksum_calc(reloid, include_header,
                                                    true,
                                                    CHECKSUM_ALGO_FNV32));
}

/*
 * pg_table_physical_checksum_algo - generic physical table checksum
 *
 * Physical table checksums include relation identity, so their SQL shape is
 * an 8-byte digest for every algorithm.
 */
PG_FUNCTION_INFO_V1(pg_table_physical_checksum_algo);

Datum
pg_table_physical_checksum_algo(PG_FUNCTION_ARGS)
{
    Oid         reloid;
    bool        include_header;
    ChecksumAlgo algorithm;
    uint64      result;

    reloid = PG_GETARG_OID(0);
    include_header = PG_GETARG_BOOL(1);
    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(2));

    result = pg_table_physical_checksum_common(reloid, include_header, true,
                                               algorithm);

    PG_RETURN_BYTEA_P(checksum_bytea(result, 8));
}

/*
 * physical_aggregate32_nonzero - keep a 32-bit table aggregate distinct
 *
 * An aggregate equal to the FNV basis (an empty relation) is replaced by a
 * hash of the relation OID, and zero by a fixed non-zero value.
 */
static uint32
physical_aggregate32_nonzero(uint32 aggregate, Oid reloid)
{
    if (aggregate == FNV_BASIS_32)
        aggregate = fnv1a_32_hash(&reloid, sizeof(reloid), FNV_BASIS_32);

    if (aggregate == 0)
        aggregate = 0x7FFFFFFF;

    return aggregate;
}

/*
 * pg_table_physical_checksum_common - physical table checksum for one algo
 *
 * check_privileges is false for leaf partitions reached through a
 * partitioned table: PostgreSQL checks the privilege on the parent when a
 * partitioned table is queried, and requiring SELECT on every partition
 * would reject perfectly valid calls.
 *
 * For fnv32 the result of a table packs the 32-bit aggregate (upper half)
 * with the relation OID (lower half); for fnv64 it is a 64-bit digest.  A
 * partitioned table is a 64-bit digest of its leaf checksums in both cases.
 */
static uint64
pg_table_physical_checksum_common(Oid reloid, bool include_header,
                                  bool check_privileges, ChecksumAlgo algo)
{
    Relation    rel;
    ChecksumAccum acc32 = {0, 0};
    ChecksumAccum64 acc64 = {0, 0};
    uint64      aggregate;
    uint64      relation_hash;
    BlockNumber nblocks;
    int         workers;

    rel = relation_open(reloid, AccessShareLock);

    if (check_privileges)
        pg_integrity_checksums_require_select(reloid);

    pg_integrity_checksums_check_relation_kind(rel, true, true);

    /*
     * A partitioned table has no storage of its own: its data lives in the
     * leaf partitions. Aggregate the physical checksums of all leaf
     * partitions into a single order-independent 64-bit value.
     */
    if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
    {
        List       *parts = pg_integrity_checksums_leaf_partitions(reloid);
        ChecksumAccum64 pacc = {0, 0};
        ListCell   *lc;

        foreach(lc, parts)
        {
            Oid         partoid = lfirst_oid(lc);
            uint64      part_checksum;

            part_checksum = pg_table_physical_checksum_common(partoid,
                                                              include_header,
                                                              false, algo);
            checksum_accum64_add(&pacc,
                                 combine_checksums_64(part_checksum,
                                                      (uint64) partoid));
        }

        list_free(parts);
        relation_close(rel, AccessShareLock);

        aggregate = checksum_accum64_finalize(&pacc);
        return combine_checksums_64(aggregate, (uint64) reloid);
    }

    /*
     * A TOAST relation is checksummed page by page (with hint bits and other
     * unstable page state masked) rather than through the visible-tuple
     * path; its chunks are covered logically through the parent table.
     */
    if (rel->rd_rel->relkind == RELKIND_TOASTVALUE)
    {
        uint64      relfilenumber = pg_integrity_checksums_relfilenumber(rel);
        uint64      tablespace = rel->rd_rel->reltablespace;

        nblocks = RelationGetNumberOfBlocks(rel);

        scan_relation_raw_pages(rel, 0, nblocks, algo, &acc32, &acc64);

        relation_close(rel, AccessShareLock);

        if (checksum_algo_is32(algo))
        {
            uint32      aggregate32;

            aggregate32 = physical_aggregate32_nonzero(checksum_accum_finalize(&acc32),
                                                       reloid);
            aggregate32 = combine_checksums(aggregate32,
                                            (uint32) relfilenumber);
            aggregate32 = combine_checksums(aggregate32, (uint32) tablespace);

            return ((uint64) aggregate32 << 32) | reloid;
        }

        aggregate = checksum_accum64_finalize(&acc64);
        aggregate = combine_checksums_64(aggregate, (uint64) reloid);
        aggregate = combine_checksums_64(aggregate, relfilenumber);
        return combine_checksums_64(aggregate, tablespace);
    }

    /*
     * Include the relation and storage identity (but not relpages/reltuples:
     * planner statistics are not part of the physical data and ANALYZE must
     * not change the checksum).  relfilenode changes on VACUUM FULL, CLUSTER
     * and SET TABLESPACE, so even an empty rewritten table gets a new
     * checksum.
     */
    relation_hash = hash_bytes_algo(0, &reloid, sizeof(reloid), algo);
    relation_hash = combine_hash(relation_hash,
                                 (uint64) pg_integrity_checksums_relfilenumber(rel),
                                 algo);
    relation_hash = combine_hash(relation_hash,
                                 (uint64) rel->rd_rel->reltablespace, algo);

    nblocks = RelationGetNumberOfBlocks(rel);
    workers = pg_integrity_checksums_worker_count_for(rel);
    if (nblocks < (BlockNumber) workers)
        workers = (int) nblocks;

    if (workers < 1)
    {
        Snapshot    snapshot = pg_integrity_checksums_get_snapshot();
        BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

        scan_table_blocks_physical(rel, 0, nblocks, include_header,
                                   snapshot, algo, &acc32, &acc64, bstrategy);
        FreeAccessStrategy(bstrategy);
    }
    else
        pg_integrity_checksums_parallel_scan_blocks(CHECKSUM_TASK_TABLE_PHYSICAL,
                                                    reloid, include_header,
                                                    nblocks, workers, algo,
                                                    &acc32, NULL, &acc64, NULL);

    relation_close(rel, AccessShareLock);

    if (checksum_algo_is32(algo))
    {
        uint32      aggregate32;

        aggregate32 = combine_checksums(checksum_accum_finalize(&acc32),
                                        (uint32) relation_hash);
        aggregate32 = physical_aggregate32_nonzero(aggregate32, reloid);

        /* Aggregate hash in the upper 32 bits, OID in the lower 32 bits */
        return ((uint64) aggregate32 << 32) | reloid;
    }

    aggregate = checksum_accum64_finalize(&acc64);
    aggregate = combine_checksums_64(aggregate, relation_hash);
    return combine_checksums_64(aggregate, (uint64) reloid);
}

/*
 * pg_table_physical_checksum_calc - physical table checksum for one algo
 */
uint64
pg_table_physical_checksum_calc(Oid reloid, bool include_header,
                                bool check_privileges, ChecksumAlgo algo)
{
    return pg_table_physical_checksum_common(reloid, include_header,
                                             check_privileges, algo);
}

/*
 * pg_table_logical_checksum - SQL function for logical table checksums
 *
 * Computes a canonical logical checksum for an entire table by aggregating
 * logical checksums of all visible rows.  No primary key is required.
 *
 * Returns NULL for a TOAST relation; its content is covered through the
 * parent table.
 */
PG_FUNCTION_INFO_V1(pg_table_logical_checksum);

Datum
pg_table_logical_checksum(PG_FUNCTION_ARGS)
{
    Oid         reloid;
    bool        is_null = false;
    int64       result;

    reloid = PG_GETARG_OID(0);

    result = (int64) pg_table_logical_checksum_calc(reloid, true,
                                                    CHECKSUM_ALGO_FNV32,
                                                    &is_null);

    if (is_null)
        PG_RETURN_NULL();

    PG_RETURN_INT64(result);
}

/*
 * pg_table_logical_checksum_algo - generic table logical checksum
 */
PG_FUNCTION_INFO_V1(pg_table_logical_checksum_algo);

Datum
pg_table_logical_checksum_algo(PG_FUNCTION_ARGS)
{
    Oid         reloid;
    ChecksumAlgo algorithm;
    bool        is_null = false;
    uint64      result;

    reloid = PG_GETARG_OID(0);
    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(1));

    result = pg_table_logical_checksum_common(reloid, true, algorithm,
                                              &is_null);
    if (is_null)
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(result,
                                     checksum_algo_len(algorithm)));
}

/*
 * pg_table_logical_checksum_accum - accumulate canonical logical rows
 *
 * There is no primary key requirement and no relation OID in the result.
 * Partitioned tables accumulate all leaf rows into the same accumulator, so
 * the result does not depend on how the table is partitioned.  The
 * accumulator is commutative, so the parallel and serial paths produce the
 * same checksum.  Only the accumulator that matches "algo" is used.
 *
 * *is_null is set for TOAST relations, whose content is covered through the
 * parent table.
 */
static void
pg_table_logical_checksum_accum(Oid reloid, bool check_privileges,
                                ChecksumAlgo algo,
                                ChecksumLogicalAccum *acc32,
                                ChecksumLogicalAccum64 *acc64,
                                bool *is_null, TupleDesc order_desc)
{
    Relation    rel;
    char        relkind;
    BlockNumber nblocks;
    int         workers;

    *is_null = false;

    rel = relation_open(reloid, AccessShareLock);

    if (check_privileges)
        pg_integrity_checksums_require_select(reloid);

    pg_integrity_checksums_check_relation_kind(rel, true, true);

    relkind = rel->rd_rel->relkind;

    if (relkind == RELKIND_TOASTVALUE)
    {
        relation_close(rel, AccessShareLock);
        *is_null = true;
        return;
    }

    if (relkind == RELKIND_PARTITIONED_TABLE)
    {
        List       *parts = pg_integrity_checksums_leaf_partitions(reloid);
        ListCell   *lc;
        bool        any_null = false;

        foreach(lc, parts)
        {
            Oid         partoid = lfirst_oid(lc);
            bool        part_null = false;

            pg_table_logical_checksum_accum(partoid, false, algo, acc32, acc64,
                                            &part_null,
                                            order_desc ? order_desc :
                                            RelationGetDescr(rel));
            if (part_null)
                any_null = true;
        }

        list_free(parts);
        relation_close(rel, AccessShareLock);

        if (any_null)
            *is_null = true;
        return;
    }

    nblocks = RelationGetNumberOfBlocks(rel);
    workers = pg_integrity_checksums_worker_count_for(rel);

    /*
     * A partition that declares its columns in another order than the
     * partitioned table is hashed in the table's column order, which only
     * the serial scan knows about.
     */
    if (order_desc != NULL &&
        logical_columns_same_order(RelationGetDescr(rel), order_desc))
        order_desc = NULL;
    if (order_desc != NULL)
        workers = 0;

    if (nblocks < (BlockNumber) workers)
        workers = (int) nblocks;

    if (workers < 1)
    {
        Snapshot    snapshot = pg_integrity_checksums_get_snapshot();
        BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

        scan_table_blocks_logical(rel, 0, nblocks, snapshot, algo, acc32,
                                  acc64, bstrategy, order_desc);
        FreeAccessStrategy(bstrategy);
    }
    else
        pg_integrity_checksums_parallel_scan_blocks(CHECKSUM_TASK_TABLE_LOGICAL,
                                                    reloid, false, nblocks,
                                                    workers, algo, NULL, acc32,
                                                    NULL, acc64);

    relation_close(rel, AccessShareLock);
}

/*
 * pg_table_logical_checksum_common - logical table checksum for one algo
 *
 * Sets *is_null when the checksum is undefined (a TOAST relation).
 */
static uint64
pg_table_logical_checksum_common(Oid reloid, bool check_privileges,
                                 ChecksumAlgo algo, bool *is_null)
{
    ChecksumLogicalAccum acc32 = {0, 0};
    ChecksumLogicalAccum64 acc64 = {0, 0};

    pg_table_logical_checksum_accum(reloid, check_privileges, algo,
                                    &acc32, &acc64, is_null, NULL);

    if (*is_null)
        return 0;

    if (checksum_algo_is32(algo))
        return logical_accum_finalize(&acc32);

    return logical_accum_finalize64(&acc64);
}

/*
 * pg_table_logical_checksum_calc - logical table checksum for one algo
 */
uint64
pg_table_logical_checksum_calc(Oid reloid, bool check_privileges,
                               ChecksumAlgo algo, bool *is_null)
{
    return pg_table_logical_checksum_common(reloid, check_privileges, algo,
                                            is_null);
}
