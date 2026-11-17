-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.9.0'" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums 2.8.0 -> 2.9.0
 *
 * Adds pg_relation_page_checksum_status(), which verifies every page of a
 * relation against its stored pd_checksum, and pg_sequence_logical_checksum(),
 * which covers a sequence's definition and last_value.  Install the new
 * library before running this script (a normal package upgrade does).
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
'Generic logical sequence checksum. Algorithm fnv32 returns 4 bytes, fnv64 returns 8 bytes.';

