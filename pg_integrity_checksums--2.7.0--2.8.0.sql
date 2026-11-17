-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.8.0'" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums 2.7.0 -> 2.8.0
 *
 * Adds pg_table_bucket_checksums(), pg_table_row_checksums() and
 * pg_table_block_checksums(), which locate the rows that differ between two
 * copies of a table.  Existing functions are unchanged; some checksum values
 * change through fixes in the C library (see the Changes file and "2.8" in
 * docs/upgrading.md).  Install the new library before running this script (a
 * normal package upgrade does).
 *-------------------------------------------------------------------------
 */

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
