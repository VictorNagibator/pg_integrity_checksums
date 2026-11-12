-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.5.0'" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums 2.4.0 -> 2.5.0
 *
 * 2.5.0 changes the default of pg_integrity_checksums.strict to on, so a
 * database checksum fails instead of silently skipping a relation that
 * cannot be checksummed.  The change lives in the C library; there are no
 * SQL object changes.
 *-------------------------------------------------------------------------
 */
