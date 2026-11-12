-- Database-level checksums are strict by default: a relation without a usable
-- checksum aborts the call instead of silently dropping out of the result.
-- An invalid index is the portable way to provoke that in SQL; only the
-- logical checksum has no content for it (a physical checksum can still read
-- the index pages).

CREATE TABLE test_strict_t (v int);
INSERT INTO test_strict_t SELECT g FROM generate_series(1, 2000) g;
CREATE INDEX test_strict_idx ON test_strict_t (v);
UPDATE pg_index SET indisvalid = false
WHERE indexrelid = 'test_strict_idx'::regclass;

-- Run one statement and report how it ended.
CREATE FUNCTION test_strict_outcome(stmt text) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE stmt;
    RETURN 'ok';
EXCEPTION WHEN feature_not_supported THEN
    RETURN 'rejected';
END;
$$;

-- The error names the relation and says why it has no checksum.
DO $$
DECLARE
    msg text;
    detail text;
BEGIN
    BEGIN
        PERFORM pg_database_logical_checksum(false, true);
    EXCEPTION WHEN feature_not_supported THEN
        GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT, detail = PG_EXCEPTION_DETAIL;
        RAISE NOTICE '%', msg;
        RAISE NOTICE '%', detail;
    END;
END;
$$;

-- The default is strict.  The GUC only exists once the library is loaded, which
-- happens on the first call into the extension.
SELECT pg_text_checksum('load the library') IS NOT NULL AS library_loaded;
RESET pg_integrity_checksums.strict;
SHOW pg_integrity_checksums.strict;

-- Every database-level entry point, with the default.
SELECT f AS call, test_strict_outcome('SELECT ' || f) AS default_outcome
FROM (VALUES
    ('pg_database_physical_checksum(false, false)'),
    ('pg_database_logical_checksum(false, true)'),
    ('pg_database_physical_checksum(false, false, ''fnv32'')'),
    ('pg_database_physical_checksum(false, false, ''fnv64'')'),
    ('pg_database_logical_checksum(false, true, ''fnv32'')'),
    ('pg_database_logical_checksum(false, true, ''fnv64'')')
) AS v(f);

-- The same with strict mode switched off: nothing is rejected, the invalid
-- index is skipped with a warning (silenced here).
SET pg_integrity_checksums.strict = off;
SET client_min_messages = error;
SELECT f AS call, test_strict_outcome('SELECT ' || f) AS off_outcome
FROM (VALUES
    ('pg_database_physical_checksum(false, false)'),
    ('pg_database_logical_checksum(false, true)'),
    ('pg_database_physical_checksum(false, false, ''fnv32'')'),
    ('pg_database_physical_checksum(false, false, ''fnv64'')'),
    ('pg_database_logical_checksum(false, true, ''fnv32'')'),
    ('pg_database_logical_checksum(false, true, ''fnv64'')')
) AS v(f);

-- Excluding indexes means there is nothing to reject.
RESET pg_integrity_checksums.strict;
SELECT test_strict_outcome('SELECT pg_database_logical_checksum(false, false)')
       AS no_indexes_outcome;

-- Parallel scans follow the same rules.  Strict mode rejects the invalid
-- index no matter how many workers run ...
SET pg_integrity_checksums.workers = 2;
SELECT test_strict_outcome('SELECT pg_database_logical_checksum(false, true)')
       AS parallel_strict_32,
       test_strict_outcome('SELECT pg_database_logical_checksum(false, true, ''fnv64'')')
       AS parallel_strict_64;

-- ... and with strict mode off the parallel result equals the serial one.
SET pg_integrity_checksums.strict = off;
SET client_min_messages = error;
CREATE TEMP TABLE test_strict_parallel AS
SELECT pg_database_logical_checksum(false, true) AS c32,
       pg_database_logical_checksum(false, true, 'fnv64') AS c64;
SET pg_integrity_checksums.workers = 0;
SELECT (SELECT c32 FROM test_strict_parallel) =
       pg_database_logical_checksum(false, true) AS parallel_equals_serial_32,
       (SELECT c64 FROM test_strict_parallel) =
       pg_database_logical_checksum(false, true, 'fnv64') AS parallel_equals_serial_64;

RESET pg_integrity_checksums.workers;
RESET pg_integrity_checksums.strict;
RESET client_min_messages;

-- Fix the index: strict scans succeed again.
UPDATE pg_index SET indisvalid = true
WHERE indexrelid = 'test_strict_idx'::regclass;
SELECT test_strict_outcome('SELECT pg_database_logical_checksum(false, true)')
       AS valid_index_outcome;

DROP FUNCTION test_strict_outcome(text);
DROP TABLE test_strict_t;
