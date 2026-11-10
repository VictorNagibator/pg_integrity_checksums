-- 2.3 adds page-checksum verification.  pg_page_checksum() remains a
-- calculator for compatibility; the new function compares the calculated
-- value with the checksum stored in the page header.

CREATE OR REPLACE FUNCTION pg_page_checksum_status(relname regclass, blkno integer)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_page_checksum_status'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_page_checksum_status(regclass, integer) IS
'Verify a page against the stored pd_checksum. Returns ok, mismatch, new,
 checksums_disabled, or raises for an invalid block/relation. Requires
 data_checksums = on for ok/mismatch to be meaningful.';

COMMENT ON FUNCTION pg_cell_checksum(regclass, tid, integer) IS
'Logical checksum of a cell value. Returns -1 (0xFFFFFFFF, CHECKSUM_NULL)
 for NULL values. Same value in same column always produces the same checksum.
 Independent of physical storage.';

COMMENT ON FUNCTION pg_page_checksum(regclass, integer) IS
'Calculates PostgreSQL''s built-in 16-bit page checksum. This function does not
 compare the result with the stored pd_checksum. It returns 0 for new
 (uninitialized) pages, so a zero result is not proof of a valid page. The
 block number is validated; an out-of-range block raises an error instead of
 extending the relation.';
