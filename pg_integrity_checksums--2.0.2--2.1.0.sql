-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.1.0'" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums 2.0.2 -> 2.1.0
 *
 * 2.1.0 makes logical checksums endian-independent.  There are no SQL
 * object changes.
 *-------------------------------------------------------------------------
 */
