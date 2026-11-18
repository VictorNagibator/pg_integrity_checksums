-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.9.1'" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums 2.9.0 -> 2.9.1
 *
 * 2.9.1 reads blocks through a read stream on PostgreSQL 17 and later.
 * There are no SQL object changes and no checksum value changes.
 *-------------------------------------------------------------------------
 */
