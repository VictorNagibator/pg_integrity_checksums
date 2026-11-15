-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.7.0'" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums 2.6.0 -> 2.7.0
 *
 * No SQL object changes.  All changes are in the C library: tuple and cell
 * checksums return NULL for tuples the caller cannot see, index expressions
 * run as the table owner, and some checksum values change (database logical
 * checksums, physical checksums of mapped catalogs and of some index pages).
 * See the Changes file and "2.7" in docs/upgrading.md.  Install the new
 * library before running this script (a normal package upgrade does).
 *-------------------------------------------------------------------------
 */
