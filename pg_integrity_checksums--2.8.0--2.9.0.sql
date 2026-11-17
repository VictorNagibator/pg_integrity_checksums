-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.9.0'" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums 2.8.0 -> 2.9.0
 *
 * Adds pg_relation_page_checksum_status(), which verifies every page of a
 * relation against its stored pd_checksum, pg_sequence_logical_checksum(),
 * which covers a sequence's definition and last_value, and the crc32c and
 * xxh3 algorithms of the generic (algorithm) functions (only their comments
 * change in SQL).  Install the new library before running this script (a
 * normal package upgrade does).
 *-------------------------------------------------------------------------
 */

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

-- The generic functions accept the crc32c and xxh3 algorithms.
COMMENT ON FUNCTION pg_data_checksum(bytea, bigint, text) IS
'Generic binary data checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes, all big-endian. For crc32c the seed is a previous CRC-32C to continue (0 starts a new one).';

COMMENT ON FUNCTION pg_text_checksum(text, bigint, text) IS
'Generic text checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes, all big-endian. For crc32c the seed is a previous CRC-32C to continue (0 starts a new one).';

COMMENT ON FUNCTION pg_table_logical_checksum(regclass, text) IS
'Generic logical table checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

COMMENT ON FUNCTION pg_index_logical_checksum(regclass, text) IS
'Generic logical index checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

COMMENT ON FUNCTION pg_cell_checksum(regclass, tid, integer, text) IS
'Generic logical cell checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

COMMENT ON FUNCTION pg_tuple_logical_checksum(regclass, tid, text) IS
'Generic logical tuple checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

COMMENT ON FUNCTION pg_database_logical_checksum(boolean, boolean, text) IS
'Generic logical database checksum. Requires superuser. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

COMMENT ON FUNCTION pg_checksum_bigint(bytea, boolean) IS
'Convert a digest to bigint. The second argument selects unsigned interpretation for 4-byte (fnv32, crc32c) digests.';

COMMENT ON FUNCTION pg_tuple_physical_checksum(regclass, tid, boolean, text) IS
'Generic physical tuple checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

COMMENT ON FUNCTION pg_table_physical_checksum(regclass, boolean, text) IS
'Generic physical table checksum. Returns an 8-byte digest for every algorithm (fnv32, fnv64, crc32c, xxh3).';

COMMENT ON FUNCTION pg_index_physical_checksum(regclass, text) IS
'Generic physical index checksum. Algorithms fnv32 and crc32c return 4 bytes, fnv64 and xxh3 return 8 bytes.';

COMMENT ON FUNCTION pg_database_physical_checksum(boolean, boolean, text) IS
'Generic physical database checksum. Requires superuser. Returns an 8-byte digest for every algorithm (fnv32, fnv64, crc32c, xxh3).';
