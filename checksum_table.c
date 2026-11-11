/*-------------------------------------------------------------------------
 *
 * checksum_table.c
 *    Physical and logical table checksums, including partitions
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

uint64 pg_table_physical_checksum_calc(Oid reloid, bool include_header,
                                              bool check_privileges);
uint64 pg_table_physical_checksum_calc64(Oid reloid, bool include_header,
                                                bool check_privileges);
int64 pg_table_logical_checksum_calc(Oid reloid, bool check_privileges,
                                            bool *is_null);
uint64 pg_table_logical_checksum_calc64(Oid reloid, bool check_privileges,
                                               bool *is_null);
static void pg_table_logical_checksum_accum(Oid reloid, bool check_privileges,
                                            ChecksumLogicalAccum *acc,
                                            bool *is_null);

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
 * Returns a 64-bit value that combines:
 * 1. Aggregate checksum of all tuples (32 bits, shifted left)
 * 2. Relation OID (32 bits)
 *
 * This ensures that even empty tables have a unique checksum.
 */
PG_FUNCTION_INFO_V1(pg_table_physical_checksum);

Datum
pg_table_physical_checksum(PG_FUNCTION_ARGS)
{
    Oid         reloid;
    bool        include_header;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    reloid = PG_GETARG_OID(0);
    include_header = PG_ARGISNULL(1) ? false : PG_GETARG_BOOL(1);

    PG_RETURN_INT64((int64)
                    pg_table_physical_checksum_calc(reloid, include_header,
                                                    true));
}

/*
 * pg_table_physical_checksum_algo - generic physical table checksum
 *
 * Physical table checksums include relation identity, so their SQL shape is
 * an 8-byte digest for both fnv32 and fnv64.
 */
PG_FUNCTION_INFO_V1(pg_table_physical_checksum_algo);

Datum
pg_table_physical_checksum_algo(PG_FUNCTION_ARGS)
{
    Oid         reloid;
    bool        include_header;
    int         algorithm;
    uint64      result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(2))
        PG_RETURN_NULL();

    reloid = PG_GETARG_OID(0);
    include_header = PG_ARGISNULL(1) ? false : PG_GETARG_BOOL(1);
    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(2));

    if (algorithm == 0)
        result = pg_table_physical_checksum_calc(reloid, include_header, true);
    else
        result = pg_table_physical_checksum_calc64(reloid, include_header,
                                                   true);

    PG_RETURN_BYTEA_P(checksum_bytea(result, 8));
}

/*
 * pg_table_physical_checksum_calc - implementation shared with partitions
 *
 * check_privileges is false for leaf partitions reached through a
 * partitioned table: PostgreSQL checks the privilege on the parent when a
 * partitioned table is queried, and requiring SELECT on every partition
 * would reject perfectly valid calls.
 */
uint64
pg_table_physical_checksum_calc(Oid reloid, bool include_header,
                                bool check_privileges)
{
    Relation    rel;
    ChecksumAccum acc = {0, 0};
    uint32      aggregate;
    uint64      final_checksum;
    uint32      relation_hash;
    uint32      nblocks;
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
        ChecksumAccum64 acc64 = {0, 0};
        ListCell   *lc;

        foreach(lc, parts)
        {
            Oid         partoid = lfirst_oid(lc);
            uint64      part_checksum;

            part_checksum = pg_table_physical_checksum_calc(partoid,
                                                            include_header,
                                                            false);

            checksum_accum64_add(&acc64,
                                 combine_checksums_64(part_checksum,
                                                      (uint64) partoid));
        }

        list_free(parts);
        relation_close(rel, AccessShareLock);

        final_checksum = checksum_accum64_finalize(&acc64);
        final_checksum = combine_checksums_64(final_checksum, (uint64) reloid);

        return final_checksum;
    }

    /*
     * TOAST tuples are not HeapTupleHeader structures.  Checksum the raw
     * pages of a TOAST relation instead of trying to interpret them as heap
     * tuples.
     */
    if (rel->rd_rel->relkind == RELKIND_TOASTVALUE)
    {
        nblocks = RelationGetNumberOfBlocks(rel);
        scan_relation_raw_pages(rel, 0, nblocks, &acc);

        aggregate = checksum_accum_finalize(&acc);

        if (aggregate == FNV_BASIS_32)
            aggregate = fnv1a_32_hash(&reloid, sizeof(reloid), FNV_BASIS_32);

        if (aggregate == 0)
            aggregate = 0x7FFFFFFF;

        aggregate = combine_checksums(aggregate,
                                      (uint32) rel->rd_rel->relfilenode);
        aggregate = combine_checksums(aggregate,
                                      (uint32) rel->rd_rel->reltablespace);

        relation_close(rel, AccessShareLock);

        return ((uint64) aggregate << 32) | reloid;
    }

    /*
     * Include the relation and storage identity (but not relpages/reltuples:
     * planner statistics are not part of the physical data and ANALYZE must
     * not change the checksum).  relfilenode changes on VACUUM FULL, CLUSTER
     * and SET TABLESPACE, so even an empty rewritten table gets a new
     * checksum.
     */
    relation_hash = fnv1a_32_hash(&reloid, sizeof(reloid), FNV_BASIS_32);
    relation_hash = combine_checksums(relation_hash,
                                      (uint32) rel->rd_rel->relfilenode);
    relation_hash = combine_checksums(relation_hash,
                                      (uint32) rel->rd_rel->reltablespace);

    nblocks = RelationGetNumberOfBlocks(rel);
    workers = pg_integrity_checksums_worker_count();
    if (nblocks < (uint32) workers)
        workers = (int) nblocks;

    if (workers < 1)
    {
        Snapshot    snapshot = pg_integrity_checksums_get_snapshot();
        BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

        scan_table_blocks_physical(rel, 0, nblocks, include_header,
                                   snapshot, &acc, bstrategy);
        FreeAccessStrategy(bstrategy);
    }
    else
    {
        pg_integrity_checksums_parallel_scan_blocks(CHECKSUM_TASK_TABLE_PHYSICAL,
                                          reloid, include_header, nblocks,
                                          workers, 0, &acc, NULL, NULL, NULL);
    }

    relation_close(rel, AccessShareLock);

    /* Compute order-independent aggregate */
    aggregate = checksum_accum_finalize(&acc);

    /* Combine with relation metadata */
    aggregate = combine_checksums(aggregate, relation_hash);

    if (aggregate == FNV_BASIS_32)
    {
        /* Guarantee hash for empty tables */
        aggregate = fnv1a_32_hash(&reloid, sizeof(reloid), FNV_BASIS_32);
    }

    if (aggregate == 0)
        aggregate = 0x7FFFFFFF;

    /* Combine aggregate hash (upper 32 bits) with OID (lower 32 bits) */
    final_checksum = ((uint64) aggregate << 32) | reloid;

    return final_checksum;
}

/*
 * pg_table_physical_checksum_calc64 - serial 64-bit physical table checksum
 */
uint64
pg_table_physical_checksum_calc64(Oid reloid, bool include_header,
                                  bool check_privileges)
{
    Relation    rel;
    ChecksumAccum64 acc = {0, 0};
    uint64      aggregate;
    uint64      relation_hash;
    uint32      nblocks;
    int         workers;

    rel = relation_open(reloid, AccessShareLock);

    if (check_privileges)
        pg_integrity_checksums_require_select(reloid);

    pg_integrity_checksums_check_relation_kind(rel, true, true);

    if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
    {
        List       *parts = pg_integrity_checksums_leaf_partitions(reloid);
        ChecksumAccum64 pacc = {0, 0};
        ListCell   *lc;

        foreach(lc, parts)
        {
            Oid         partoid = lfirst_oid(lc);
            uint64      part_checksum;

            part_checksum = pg_table_physical_checksum_calc64(partoid,
                                                              include_header,
                                                              false);
            checksum_accum64_add(&pacc,
                                 combine_checksums_64(part_checksum,
                                                      (uint64) partoid));
        }
        list_free(parts);
        relation_close(rel, AccessShareLock);

        aggregate = checksum_accum64_finalize(&pacc);
        return combine_checksums_64(aggregate, (uint64) reloid);
    }

    if (rel->rd_rel->relkind == RELKIND_TOASTVALUE)
    {
        nblocks = RelationGetNumberOfBlocks(rel);
        scan_relation_raw_pages64(rel, 0, nblocks, &acc);
        aggregate = checksum_accum64_finalize(&acc);
        aggregate = combine_checksums_64(aggregate, (uint64) reloid);
        aggregate = combine_checksums_64(aggregate,
                                         (uint64) rel->rd_rel->relfilenode);
        aggregate = combine_checksums_64(aggregate,
                                         (uint64) rel->rd_rel->reltablespace);
        relation_close(rel, AccessShareLock);
        return aggregate;
    }

    relation_hash = fnv1a_64_hash(&reloid, sizeof(reloid), FNV_BASIS_64);
    relation_hash = combine_checksums_64(relation_hash,
                                         (uint64) rel->rd_rel->relfilenode);
    relation_hash = combine_checksums_64(relation_hash,
                                         (uint64) rel->rd_rel->reltablespace);

    nblocks = RelationGetNumberOfBlocks(rel);
    workers = pg_integrity_checksums_worker_count();
    if (IsInParallelMode())
        workers = 0;
    if (workers < 1)
    {
        Snapshot    snapshot = pg_integrity_checksums_get_snapshot();
        BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

        scan_table_blocks_physical64(rel, 0, nblocks, include_header, snapshot,
                                     &acc, bstrategy);
        FreeAccessStrategy(bstrategy);
    }
    else
        pg_integrity_checksums_parallel_scan_blocks(CHECKSUM_TASK_TABLE_PHYSICAL,
                                                    reloid, include_header,
                                                    nblocks, workers, 1,
                                                    NULL, NULL, &acc, NULL);

    relation_close(rel, AccessShareLock);

    aggregate = checksum_accum64_finalize(&acc);
    aggregate = combine_checksums_64(aggregate, relation_hash);
    aggregate = combine_checksums_64(aggregate, (uint64) reloid);

    return aggregate;
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

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    reloid = PG_GETARG_OID(0);

    result = pg_table_logical_checksum_calc(reloid, true, &is_null);

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
    int         algorithm;
    bool        is_null = false;
    uint64      result;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        PG_RETURN_NULL();

    reloid = PG_GETARG_OID(0);
    algorithm = checksum_algorithm(PG_GETARG_TEXT_PP(1));

    if (algorithm == 0)
    {
        int64       legacy = pg_table_logical_checksum_calc(reloid, true,
                                                            &is_null);

        if (is_null)
            PG_RETURN_NULL();
        PG_RETURN_BYTEA_P(checksum_bytea((uint32) legacy, 4));
    }

    result = pg_table_logical_checksum_calc64(reloid, true, &is_null);
    if (is_null)
        PG_RETURN_NULL();

    PG_RETURN_BYTEA_P(checksum_bytea(result, 8));
}

/*
 * pg_table_logical_checksum_calc - implementation shared with partitions
 *
 * Sets *is_null when the checksum is undefined (a TOAST relation).
 */
int64
pg_table_logical_checksum_calc(Oid reloid, bool check_privileges,
                               bool *is_null)
{
    ChecksumLogicalAccum acc = {0, 0};

    pg_table_logical_checksum_accum(reloid, check_privileges, &acc, is_null);

    if (*is_null)
        return 0;

    return (int64) logical_accum_finalize(&acc);
}

/*
 * pg_table_logical_checksum_accum - accumulate canonical logical rows
 *
 * There is no primary key requirement and no relation OID in the result.
 * Partitioned tables accumulate all leaf rows into the same accumulator, so
 * the result does not depend on how the table is partitioned.
 *
 * *is_null is set for TOAST relations, whose content is covered through the
 * parent table.
 */
static void
pg_table_logical_checksum_accum(Oid reloid, bool check_privileges,
                                ChecksumLogicalAccum *acc, bool *is_null)
{
    Relation    rel;
    char        relkind;

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
        List       *parts;
        ListCell   *lc;
        bool        any_null = false;

        parts = pg_integrity_checksums_leaf_partitions(reloid);

        foreach(lc, parts)
        {
            Oid         partoid = lfirst_oid(lc);
            bool        part_null = false;

            pg_table_logical_checksum_accum(partoid, false, acc, &part_null);
            if (part_null)
                any_null = true;
        }

        list_free(parts);
        relation_close(rel, AccessShareLock);

        if (any_null)
            *is_null = true;
        return;
    }

    {
        Snapshot    snapshot = pg_integrity_checksums_get_snapshot();
        BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
        int         workers = pg_integrity_checksums_worker_count();

        if (nblocks < (BlockNumber) workers)
            workers = (int) nblocks;

        if (workers < 1)
        {
            BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

            scan_table_blocks_logical(rel, 0, nblocks, snapshot, acc,
                                      bstrategy);
            FreeAccessStrategy(bstrategy);
        }
        else
            pg_integrity_checksums_parallel_scan_blocks(CHECKSUM_TASK_TABLE_LOGICAL,
                                              reloid, false, nblocks, workers,
                                              0, NULL, acc, NULL, NULL);
    }

    relation_close(rel, AccessShareLock);
}

/*
 * pg_table_logical_checksum_accum64 - accumulate 64-bit canonical rows
 *
 * The result is order-independent because rows are folded through a
 * commutative logical accumulator, so the parallel and serial paths produce
 * the same checksum.
 */
static void
pg_table_logical_checksum_accum64(Oid reloid, bool check_privileges,
                                  ChecksumLogicalAccum64 *acc, bool *is_null)
{
    Relation    rel;
    char        relkind;
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

            pg_table_logical_checksum_accum64(partoid, false, acc, &part_null);
            if (part_null)
                any_null = true;
        }

        list_free(parts);
        relation_close(rel, AccessShareLock);

        if (any_null)
            *is_null = true;
        return;
    }

    {
        BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

        workers = pg_integrity_checksums_worker_count();
        if (IsInParallelMode())
            workers = 0;
        if (workers < 1)
        {
            Snapshot    snapshot = pg_integrity_checksums_get_snapshot();
            BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

            scan_table_blocks_logical64(rel, 0, nblocks, snapshot, acc,
                                        bstrategy);
            FreeAccessStrategy(bstrategy);
        }
        else
            pg_integrity_checksums_parallel_scan_blocks(CHECKSUM_TASK_TABLE_LOGICAL,
                                                        reloid, false, nblocks,
                                                        workers, 1, NULL, NULL,
                                                        NULL, acc);
    }

    relation_close(rel, AccessShareLock);
}

/*
 * pg_table_logical_checksum_calc64 - 64-bit logical table checksum
 */
uint64
pg_table_logical_checksum_calc64(Oid reloid, bool check_privileges,
                                 bool *is_null)
{
    ChecksumLogicalAccum64 acc = {0, 0};

    pg_table_logical_checksum_accum64(reloid, check_privileges, &acc, is_null);

    if (*is_null)
        return 0;

    return logical_accum_finalize64(&acc);
}

