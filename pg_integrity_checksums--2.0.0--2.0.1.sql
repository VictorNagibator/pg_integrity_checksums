-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.0.1'" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums 2.0.0 -> 2.0.1
 *
 * 2.0.1 fixes logical checksum handling for system catalogs and hash
 * indexes.  There are no SQL object changes.
 *-------------------------------------------------------------------------
 */
