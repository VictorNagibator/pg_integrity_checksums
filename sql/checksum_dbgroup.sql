-- Grouped logical database scan
--
-- A serial logical database checksum reads each table once for the table
-- itself and all of its indexes.  The result must equal the per-relation
-- computation, which a parallel scan still uses, and a failure in one index
-- must be handled exactly like before: the group is retried relation by
-- relation, so only the failing index is skipped (or named in strict mode).

SET client_min_messages = warning;

CREATE SCHEMA dbgroup;

CREATE TABLE dbgroup.t (
    id      integer PRIMARY KEY,
    a       integer,
    b       text,
    c       numeric
);
INSERT INTO dbgroup.t
SELECT g, g % 37, 'Row ' || g, g / 7.0 FROM generate_series(1, 2000) AS g;
CREATE INDEX dbgroup_t_a ON dbgroup.t (a);
CREATE INDEX dbgroup_t_lower ON dbgroup.t (lower(b)) WHERE a % 2 = 0;
CREATE INDEX dbgroup_t_incl ON dbgroup.t (a) INCLUDE (b, c);
CREATE INDEX dbgroup_t_hash ON dbgroup.t USING hash (b);

CREATE MATERIALIZED VIEW dbgroup.mv AS SELECT a, count(*) AS n FROM dbgroup.t GROUP BY a;
CREATE UNIQUE INDEX dbgroup_mv_a ON dbgroup.mv (a);

-- An empty table with an index, and a table without indexes.
CREATE TABLE dbgroup.empty (x integer);
CREATE INDEX dbgroup_empty_x ON dbgroup.empty (x);
CREATE TABLE dbgroup.noindex (x integer);
INSERT INTO dbgroup.noindex SELECT generate_series(1, 10);

-- Serial (grouped) and parallel (per relation) agree.
SET pg_integrity_checksums.workers = 0;
SELECT pg_database_logical_checksum(false, true, 'fnv64') AS serial64,
       pg_database_logical_checksum(false, true, 'fnv32') AS serial32,
       pg_database_logical_checksum() AS serial_legacy
\gset
SET pg_integrity_checksums.workers = 2;
SELECT pg_database_logical_checksum(false, true, 'fnv64') = :'serial64' AS fnv64_matches,
       pg_database_logical_checksum(false, true, 'fnv32') = :'serial32' AS fnv32_matches,
       pg_database_logical_checksum() = :serial_legacy AS legacy_matches;
SET pg_integrity_checksums.workers = 0;

-- Every index of a grouped table is counted: adding one changes the result,
-- dropping it again restores it.
CREATE INDEX dbgroup_t_c ON dbgroup.t (c);
SELECT pg_database_logical_checksum(false, true, 'fnv64') <> :'serial64' AS new_index_counted;
DROP INDEX dbgroup.dbgroup_t_c;
SELECT pg_database_logical_checksum(false, true, 'fnv64') = :'serial64' AS back_to_serial;

-- An index whose expression fails part-way through the heap.  The function
-- is replaced after the index is built, so the index itself is valid.
CREATE FUNCTION dbgroup.f(x integer) RETURNS integer
LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RETURN x; END $$;
CREATE INDEX dbgroup_t_bad ON dbgroup.t (dbgroup.f(id));
CREATE OR REPLACE FUNCTION dbgroup.f(x integer) RETURNS integer
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    IF x = 1500 THEN
        RAISE EXCEPTION 'dbgroup.f failed';
    END IF;
    RETURN x;
END
$$;

-- Strict mode (the default): the error names the failing index.
DO $$
DECLARE
    msg text;
    ctx text;
BEGIN
    PERFORM pg_database_logical_checksum(false, true, 'fnv64');
    RAISE NOTICE 'no error';
EXCEPTION WHEN OTHERS THEN
    GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT, ctx = PG_EXCEPTION_CONTEXT;
    RAISE WARNING 'error: %, names the index: %', msg,
        ctx LIKE '%relation "dbgroup_t_bad"%';
END
$$;

-- Non-strict: only the failing index is skipped; the table and its other
-- indexes are still counted, so the result equals the database without it.
SET pg_integrity_checksums.strict = off;
SET client_min_messages = error;        -- the skip warnings name OIDs
-- (Only the serial scan is checked: before PostgreSQL 17 a parallel database
-- scan cannot use subtransactions, so any error aborts it.)
SELECT pg_database_logical_checksum(false, true, 'fnv64') AS with_bad \gset
DROP INDEX dbgroup.dbgroup_t_bad;
SELECT pg_database_logical_checksum(false, true, 'fnv64') = :'with_bad' AS bad_index_skipped,
       pg_database_logical_checksum(false, true, 'fnv64') = :'serial64' AS rest_unchanged;
RESET client_min_messages;
RESET pg_integrity_checksums.strict;

-- An invalid index is skipped (non-strict) or reported (strict), as before.
SET client_min_messages = warning;
UPDATE pg_index SET indisvalid = false
WHERE indexrelid = 'dbgroup.dbgroup_t_a'::regclass;
DO $$
BEGIN
    PERFORM pg_database_logical_checksum(false, true, 'fnv64');
    RAISE NOTICE 'no error';
EXCEPTION WHEN feature_not_supported THEN
    RAISE WARNING 'strict: %', SQLERRM;
END
$$;
SET pg_integrity_checksums.strict = off;
SET client_min_messages = error;
SELECT pg_database_logical_checksum(false, true, 'fnv64') <> :'serial64' AS invalid_index_left_out;
RESET client_min_messages;
RESET pg_integrity_checksums.strict;
UPDATE pg_index SET indisvalid = true
WHERE indexrelid = 'dbgroup.dbgroup_t_a'::regclass;

DROP SCHEMA dbgroup CASCADE;
RESET pg_integrity_checksums.workers;
