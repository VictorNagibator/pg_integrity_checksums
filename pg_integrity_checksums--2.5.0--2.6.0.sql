-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.6.0'" to load this file. \quit

/*-------------------------------------------------------------------------
 * pg_integrity_checksums 2.5.0 -> 2.6.0
 *
 * The pure utility functions are IMMUTABLE and PARALLEL SAFE, so they can be
 * used in index expressions, generated columns and parallel queries.  The
 * generic (algorithm) overloads and pg_checksum_bigint used to be VOLATILE,
 * and no utility function was marked PARALLEL SAFE.
 *
 * The C library also changes physical index and TOAST checksum values; see
 * the Changes file and "Upgrading to 2.6" in docs/installation.md.  Install
 * the new library before running this script (a normal package upgrade does).
 *-------------------------------------------------------------------------
 */

ALTER FUNCTION pg_data_checksum(bytea, integer) PARALLEL SAFE;
ALTER FUNCTION pg_text_checksum(text, integer) PARALLEL SAFE;
ALTER FUNCTION pg_data_checksum(bytea, bigint, text) IMMUTABLE PARALLEL SAFE;
ALTER FUNCTION pg_text_checksum(text, bigint, text) IMMUTABLE PARALLEL SAFE;
ALTER FUNCTION pg_checksum_bigint(bytea) IMMUTABLE PARALLEL SAFE;
ALTER FUNCTION pg_checksum_bigint(bytea, boolean) IMMUTABLE PARALLEL SAFE;
