-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.9.2'" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums 2.9.1 -> 2.9.2
 *
 * 2.9.2 fixes the logical checksum of xml values and the physical checksum
 * of BRIN indexes and pre-PostgreSQL 11 index pages.  There are no SQL
 * object changes.
 *-------------------------------------------------------------------------
 */
