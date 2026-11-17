-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_integrity_checksums" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums - Multi-Level Checksum Framework for PostgreSQL
 *
 * This extension provides checksum computation at six granularity levels,
 * with both physical and logical variants (except where noted):
 *
 * 1. PAGE LEVEL: Physical only - detects storage corruption
 * 2. CELL LEVEL: Logical only - based on cell values
 * 3. TUPLE LEVEL: Physical & Logical - row-level integrity
 * 4. TABLE LEVEL: Physical & Logical - table-wide consistency
 * 5. INDEX LEVEL: Physical & Logical - index structure validation
 * 6. DATABASE LEVEL: Physical & Logical - database-wide integrity
 *
 * Physical checksums depend on storage layout and change after:
 *   - VACUUM FULL, CLUSTER, REINDEX
 *   - Physical data movement
 *   - Storage reorganization
 *
 * Logical checksums depend on data content, not on physical layout, relation
 * OIDs or heap TIDs.  Values are canonicalized by type: enum labels, numeric
 * scale, arrays, composites and ranges do not leak type OIDs.  Logical index
 * checksums are derived from the table data and the index definition, so they
 * work for every index access method.  Planner statistics are not part of
 * physical checksums.
 *
 * The default functions return 32-bit FNV-1a values; overloads with an
 * algorithm argument ('fnv32', 'fnv64', 'crc32c' or 'xxh3') return
 * big-endian bytea digests.
 * Page-level functions use PostgreSQL's own 16-bit page checksum.  The
 * pg_integrity_checksums.strict GUC makes database scans fail instead of
 * skipping relations without a usable checksum.
 *
 * The pure utility functions (data, text and digest conversion) are
 * IMMUTABLE and PARALLEL SAFE.  Functions that read relations are VOLATILE
 * and PARALLEL UNSAFE: they may start a parallel context of their own.
 *-------------------------------------------------------------------------
 */

/*-----------------------------------------------------------
 * PAGE LEVEL FUNCTIONS (Physical only)
 *-----------------------------------------------------------
 */

-- Page-level checksum (wraps PostgreSQL's built-in page checksums)
CREATE FUNCTION pg_page_checksum(relname regclass, blkno integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_page_checksum'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_page_checksum(regclass, integer) IS 
'Calculates PostgreSQL''s built-in 16-bit page checksum. This function does not
 compare the result with the stored pd_checksum. It returns 0 for new
 (uninitialized) pages, so a zero result is not proof of a valid page. The
 block number is validated; an out-of-range block raises an error instead of
 extending the relation.';

/*-----------------------------------------------------------
 * CELL LEVEL FUNCTIONS (Logical only)
 *-----------------------------------------------------------
 */

-- Cell-level checksum (logical - depends on Cell value only)
CREATE FUNCTION pg_cell_checksum(relname regclass, tid tid, attnum integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_cell_checksum'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_cell_checksum(regclass, tid, integer) IS 
'Logical checksum of a cell value. Returns -1 (0xFFFFFFFF, CHECKSUM_NULL)
 for NULL values. Same value in same column always produces the same checksum.
 Independent of physical storage.';

/*-----------------------------------------------------------
 * TUPLE LEVEL FUNCTIONS (Physical and Logical)
 *-----------------------------------------------------------
 */

-- Physical tuple checksum (depends on physical location)
CREATE FUNCTION pg_tuple_physical_checksum(relname regclass, tid tid, include_header boolean DEFAULT false)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_tuple_physical_checksum'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_tuple_physical_checksum(regclass, tid, boolean) IS 
'Physical checksum of a tuple (row). Depends on physical location (block, offset).
 Changes after VACUUM, CLUSTER, or physical data movement. Useful for detecting physical corruption.';

-- Logical tuple checksum (canonical content, no physical header)
CREATE FUNCTION pg_tuple_logical_checksum(relname regclass, tid tid)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_tuple_logical_checksum'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_tuple_logical_checksum(regclass, tid) IS
'Logical checksum of a tuple (row). Hashes canonical column values, without
 relation OIDs, heap TIDs or the tuple header. No primary key is required.
 Stable across physical reorganization.';

/*-----------------------------------------------------------
 * TABLE LEVEL FUNCTIONS (Physical and Logical)
 *-----------------------------------------------------------
 */

-- Physical table checksum (aggregates physical tuple checksums)
CREATE FUNCTION pg_table_physical_checksum(relname regclass, include_header boolean DEFAULT false)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_table_physical_checksum'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_table_physical_checksum(regclass, boolean) IS 
'Physical checksum of entire table. Aggregates physical checksums of all tuples.
 Order-independent aggregation. Changes after physical reorganization. 64-bit return value.';

-- Logical table checksum (aggregates logical tuple checksums)
CREATE FUNCTION pg_table_logical_checksum(relname regclass)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_table_logical_checksum'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_table_logical_checksum(regclass) IS
'Logical checksum of an entire table. No primary key is required. Aggregates
 canonical row hashes with a sum-plus-count aggregate, preserving duplicate
 rows. Relation OIDs and heap TIDs are excluded. Stable across VACUUM FULL,
 CLUSTER and REINDEX. Partitioned tables aggregate all leaf rows.';

/*-----------------------------------------------------------
 * INDEX LEVEL FUNCTIONS (Physical and Logical)
 *-----------------------------------------------------------
 */

-- Physical index checksum (depends on physical index structure)
CREATE FUNCTION pg_index_physical_checksum(relname regclass)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_index_physical_checksum'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_index_physical_checksum(regclass) IS 
'Physical checksum of an index. Depends on physical index structure.
 Supports B-tree, Hash, GiST, GIN, SP-GiST, and BRIN indexes.
 A partitioned index aggregates the checksums of its leaf indexes.
 Changes after REINDEX or physical reorganization.';

-- Logical index checksum (depends on index key values)
CREATE FUNCTION pg_index_logical_checksum(relname regclass)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_index_logical_checksum'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_index_logical_checksum(regclass) IS
'Logical checksum of an index. Derived from the visible table rows and the
 index definition, not from index pages: works for every index access method,
 including expression, partial and INCLUDE indexes. No heap TIDs or relation
 OIDs are included. Stable across VACUUM, CLUSTER and REINDEX. Physical
 checksums remain the tool for detecting missing or corrupt index entries.';

/*-----------------------------------------------------------
 * SEQUENCE FUNCTIONS (Logical)
 *-----------------------------------------------------------
 */

-- Logical sequence checksum: definition and current state
CREATE FUNCTION pg_sequence_logical_checksum(relname regclass)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_sequence_logical_checksum'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_sequence_logical_checksum(regclass) IS
'Logical checksum of a sequence: data type, start, increment, minimum,
 maximum, cache and cycle from pg_sequence, plus last_value and is_called, the
 state pg_dump restores with setval(). log_cnt is not included. The state is
 read as it is now, not as of the transaction snapshot: sequences are not
 transactional. Returns NULL for an unlogged sequence on a standby.';

CREATE FUNCTION pg_sequence_logical_checksum(regclass, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_sequence_logical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_sequence_logical_checksum(regclass, text) IS
'Generic logical sequence checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

/*-----------------------------------------------------------
 * DATABASE LEVEL FUNCTIONS (Physical and Logical)
 *-----------------------------------------------------------
 */

-- Physical database checksum (aggregates physical relation checksums)
CREATE FUNCTION pg_database_physical_checksum(include_system boolean DEFAULT false, include_toast boolean DEFAULT false)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_database_physical_checksum'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_database_physical_checksum(boolean, boolean) IS 
'Physical checksum of entire database. Aggregates physical checksums of all relations.
 Superuser-only (enforced by the C implementation). Optional filtering of system
 catalogs and TOAST tables. Relations protected by row-level security are rejected.
 Changes after any physical reorganization. 64-bit return value.';

REVOKE EXECUTE ON FUNCTION pg_database_physical_checksum(boolean, boolean) FROM PUBLIC;

-- Logical database checksum (aggregates logical relation checksums)
CREATE FUNCTION pg_database_logical_checksum(include_system boolean DEFAULT false, include_indexes boolean DEFAULT true)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_database_logical_checksum'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_database_logical_checksum(boolean, boolean) IS
'Logical checksum of an entire database. Aggregates canonical table checksums;
no primary key is required. include_indexes controls whether logical index
checksums are included. TOAST content is covered through the parent table.
Superuser-only (enforced by the C implementation).';

REVOKE EXECUTE ON FUNCTION pg_database_logical_checksum(boolean, boolean) FROM PUBLIC;

-- Per-relation database checksums: one row per relation of the scan, with
-- the same relations, checksums and snapshot as the aggregate functions.
CREATE FUNCTION pg_database_logical_checksums(
    include_system boolean DEFAULT false,
    include_indexes boolean DEFAULT true,
    relations regclass[] DEFAULT NULL,
    algorithm text DEFAULT 'fnv32',
    OUT relid regclass,
    OUT schema_name name,
    OUT rel_name name,
    OUT relkind "char",
    OUT checksum bytea,
    OUT status text,
    OUT reason text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_database_logical_checksums'
LANGUAGE C CALLED ON NULL INPUT;

COMMENT ON FUNCTION pg_database_logical_checksums(boolean, boolean, regclass[], text) IS
'Logical checksum of every relation of the database, one row per relation,
 computed in one scan with one snapshot. checksum equals what
 pg_table_logical_checksum / pg_index_logical_checksum with the same algorithm
 return. status is ok, skipped or error (reason says why). relations (NULL for
 all) limits the scan to the listed relations. Requires superuser.';

REVOKE EXECUTE ON FUNCTION pg_database_logical_checksums(boolean, boolean, regclass[], text) FROM PUBLIC;

CREATE FUNCTION pg_database_physical_checksums(
    include_system boolean DEFAULT false,
    include_toast boolean DEFAULT false,
    relations regclass[] DEFAULT NULL,
    algorithm text DEFAULT 'fnv32',
    OUT relid regclass,
    OUT schema_name name,
    OUT rel_name name,
    OUT relkind "char",
    OUT checksum bytea,
    OUT status text,
    OUT reason text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_database_physical_checksums'
LANGUAGE C CALLED ON NULL INPUT;

COMMENT ON FUNCTION pg_database_physical_checksums(boolean, boolean, regclass[], text) IS
'Physical checksum of every relation of the database, one row per relation,
 computed in one scan with one snapshot. checksum equals what
 pg_table_physical_checksum / pg_index_physical_checksum with the same
 algorithm return. status is ok, skipped or error (reason says why). relations
 (NULL for all) limits the scan to the listed relations. Requires superuser.';

REVOKE EXECUTE ON FUNCTION pg_database_physical_checksums(boolean, boolean, regclass[], text) FROM PUBLIC;

/*-----------------------------------------------------------
 * UTILITY FUNCTIONS
 *-----------------------------------------------------------
 */

-- Generic binary data checksum
CREATE FUNCTION pg_data_checksum(data bytea, seed integer DEFAULT 0)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_data_checksum'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION pg_data_checksum(bytea, integer) IS 
'Generic checksum for arbitrary binary data. Uses FNV-1a 32-bit hash.
 Seed parameter allows different contexts to produce different checksums for same data.';

-- Text data checksum
CREATE FUNCTION pg_text_checksum(text text, seed integer DEFAULT 0)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_text_checksum'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION pg_text_checksum(text, integer) IS 
'Convenience wrapper for text data checksum. Uses FNV-1a 32-bit hash.
 Useful for string comparison and text content verification.';
/*-----------------------------------------------------------
 * GENERIC CHECKSUM ALGORITHM FUNCTIONS
 *-----------------------------------------------------------
 */

-- Generic binary data checksum. The digest is big-endian raw bytes; use
-- pg_checksum_bigint for a signed integer representation.
CREATE FUNCTION pg_data_checksum(bytea, bigint, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_data_checksum_algo'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION pg_data_checksum(bytea, bigint, text) IS
'Generic binary data checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes, all big-endian. For crc32c the seed is a previous CRC-32C to continue (0 starts a new one).';

CREATE FUNCTION pg_text_checksum(text, bigint, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_text_checksum_algo'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION pg_text_checksum(text, bigint, text) IS
'Generic text checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes, all big-endian. For crc32c the seed is a previous CRC-32C to continue (0 starts a new one).';

CREATE FUNCTION pg_checksum_bigint(bytea)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_checksum_bigint'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION pg_checksum_bigint(bytea) IS
'Convert a 4- or 8-byte checksum digest to signed bigint. Four-byte values are sign-extended to match the legacy int32 functions.';

CREATE FUNCTION pg_table_logical_checksum(regclass, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_table_logical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_table_logical_checksum(regclass, text) IS
'Generic logical table checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

CREATE FUNCTION pg_index_logical_checksum(regclass, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_index_logical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_index_logical_checksum(regclass, text) IS
'Generic logical index checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

CREATE FUNCTION pg_cell_checksum(regclass, tid, integer, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_cell_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_cell_checksum(regclass, tid, integer, text) IS
'Generic logical cell checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

CREATE FUNCTION pg_tuple_logical_checksum(regclass, tid, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_tuple_logical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_tuple_logical_checksum(regclass, tid, text) IS
'Generic logical tuple checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

CREATE FUNCTION pg_database_logical_checksum(boolean, boolean, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_database_logical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_database_logical_checksum(boolean, boolean, text) IS
'Generic logical database checksum. Requires superuser. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

REVOKE EXECUTE ON FUNCTION pg_database_logical_checksum(boolean, boolean, text) FROM PUBLIC;

CREATE FUNCTION pg_checksum_bigint(bytea, boolean)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_checksum_bigint'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION pg_checksum_bigint(bytea, boolean) IS
'Convert a digest to bigint. The second argument selects unsigned interpretation for 4-byte (fnv32, crc32c) digests.';

CREATE FUNCTION pg_tuple_physical_checksum(regclass, tid, boolean, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_tuple_physical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_tuple_physical_checksum(regclass, tid, boolean, text) IS
'Generic physical tuple checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

CREATE FUNCTION pg_table_physical_checksum(regclass, boolean, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_table_physical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_table_physical_checksum(regclass, boolean, text) IS
'Generic physical table checksum. Returns an 8-byte digest for every algorithm (fnv32, fnv64, crc32c, xxh3).';

CREATE FUNCTION pg_index_physical_checksum(regclass, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_index_physical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_index_physical_checksum(regclass, text) IS
'Generic physical index checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

CREATE FUNCTION pg_database_physical_checksum(boolean, boolean, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_database_physical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_database_physical_checksum(boolean, boolean, text) IS
'Generic physical database checksum. Requires superuser. Returns an 8-byte digest for every algorithm (fnv32, fnv64, crc32c, xxh3).';

REVOKE EXECUTE ON FUNCTION pg_database_physical_checksum(boolean, boolean, text) FROM PUBLIC;

-- Verify a page against the checksum stored in its header.  The plain
-- pg_page_checksum() function only calculates the value.
CREATE FUNCTION pg_page_checksum_status(relname regclass, blkno integer)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_page_checksum_status'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_page_checksum_status(regclass, integer) IS
'Verify a page against the stored pd_checksum. Returns ok, mismatch, new,
 checksums_disabled, or raises for an invalid block/relation. Requires
 data_checksums = on for ok/mismatch to be meaningful.';

-- Verify every page of a relation against its stored checksum
CREATE FUNCTION pg_relation_page_checksum_status(
    relname regclass,
    fork_name text DEFAULT NULL,
    include_ok boolean DEFAULT false,
    OUT fork text,
    OUT blkno bigint,
    OUT status text,
    OUT stored_checksum integer,
    OUT calculated_checksum integer)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_relation_page_checksum_status'
LANGUAGE C ROWS 100;

COMMENT ON FUNCTION pg_relation_page_checksum_status(regclass, text, boolean) IS
'Verify every page of a relation against the stored pd_checksum, reading the
 on-disk copy of each block as pg_page_checksum_status does. Checks every fork
 that exists (main, fsm, vm, init) unless fork_name names one. Returns the
 blocks whose status is mismatch; with include_ok = true, every block with
 status ok, mismatch or new. Fails when data checksums are disabled.';

/*-----------------------------------------------------------
 * DIFF FUNCTIONS (Logical): locate differing rows
 *-----------------------------------------------------------
 */

-- Logical checksum per bucket of the key hash space
CREATE FUNCTION pg_table_bucket_checksums(
    relname regclass,
    buckets integer,
    key_columns text[] DEFAULT NULL,
    algorithm text DEFAULT 'fnv64')
RETURNS TABLE(bucket integer, row_count bigint, checksum bytea)
AS 'MODULE_PATHNAME', 'pg_table_bucket_checksums'
LANGUAGE C ROWS 1000;

COMMENT ON FUNCTION pg_table_bucket_checksums(regclass, integer, text[], text) IS
'Logical checksum of each bucket of a table. Every visible row goes to the
 bucket of the hash of its key (the primary key, or the whole row when there is
 none, unless key_columns is given; an empty array selects the whole row).
 Returns all buckets, empty ones included. Compare the result of two servers
 to find the buckets that hold differing rows; with one bucket the checksum
 equals pg_table_logical_checksum(relname, algorithm).';

-- Key, key hash and logical checksum of the rows in selected buckets
CREATE FUNCTION pg_table_row_checksums(
    relname regclass,
    buckets integer DEFAULT 1,
    only_buckets integer[] DEFAULT NULL,
    key_columns text[] DEFAULT NULL,
    algorithm text DEFAULT 'fnv64')
RETURNS TABLE(bucket integer, key_hash bytea, key text, row_checksum bytea,
              ctid tid, tableoid regclass)
AS 'MODULE_PATHNAME', 'pg_table_row_checksums'
LANGUAGE C ROWS 1000;

COMMENT ON FUNCTION pg_table_row_checksums(regclass, integer, integer[], text[], text) IS
'Logical checksum of every visible row whose key falls into one of only_buckets
 (all rows when NULL), with the bucket, the portable key hash, the key as
 record text and the row location. Compare by key_hash between two servers to
 find missing, extra and changed rows.';

-- Logical checksum per range of heap blocks
CREATE FUNCTION pg_table_block_checksums(
    relname regclass,
    blocks_per_range integer DEFAULT 1,
    algorithm text DEFAULT 'fnv64')
RETURNS TABLE(start_block bigint, end_block bigint, row_count bigint,
              checksum bytea)
AS 'MODULE_PATHNAME', 'pg_table_block_checksums'
LANGUAGE C ROWS 1000;

COMMENT ON FUNCTION pg_table_block_checksums(regclass, integer, text) IS
'Logical checksum of the visible rows of each range of heap blocks
 [start_block, end_block). Block numbers are only comparable between a primary
 and its physical standbys. Not available for partitioned tables; call it for
 each partition.';
