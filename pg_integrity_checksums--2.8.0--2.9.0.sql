-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.9.0'" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums 2.8.0 -> 2.9.0
 *
 * Adds pg_relation_page_checksum_status(), which verifies every page of a
 * relation against its stored pd_checksum.  Install the new library before
 * running this script (a normal package upgrade does).
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
