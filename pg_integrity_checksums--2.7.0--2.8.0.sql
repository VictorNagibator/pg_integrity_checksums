-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.8.0'" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums 2.7.0 -> 2.8.0
 *
 * Adds pg_database_logical_checksums() and pg_database_physical_checksums(),
 * which return the checksum of every relation of a database scan.  Install
 * the new library before running this script (a normal package upgrade
 * does).
 *-------------------------------------------------------------------------
 */

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
