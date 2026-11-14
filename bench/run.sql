-- bench/run.sql - time the checksum functions on the bench/setup.sql data set
--
-- Every case is run once to warm the buffer cache and then "runs" times
-- (psql variable, default 5); the median wall time is reported together
-- with the checksum value, so two builds can be compared both for speed and
-- for identical results.
--
--   psql -v runs=5 -v workers=0 -f bench/run.sql

\set ON_ERROR_STOP on
\pset footer off

\if :{?runs}
\else
\set runs 5
\endif
\if :{?workers}
\else
\set workers 0
\endif

SET pg_integrity_checksums.workers = :workers;
SET client_min_messages = warning;

CREATE OR REPLACE FUNCTION pg_temp.bench(label text, query text, runs int)
RETURNS TABLE (bench_case text, median_ms numeric, result text)
LANGUAGE plpgsql AS $$
DECLARE
    t0      timestamptz;
    times   float8[] := '{}';
    r       text;
    i       int;
BEGIN
    EXECUTE query INTO r;                       -- warm-up
    FOR i IN 1..runs LOOP
        t0 := clock_timestamp();
        EXECUTE query INTO r;
        times := times || extract(epoch FROM clock_timestamp() - t0)::float8 * 1000;
    END LOOP;

    bench_case := label;
    median_ms := round((SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY x)
                        FROM unnest(times) AS x)::numeric, 1);
    result := r;
    RETURN NEXT;
END
$$;

SELECT b.* FROM (VALUES
    (1,  'table logical   narrow  fnv32', $q$SELECT pg_table_logical_checksum('bench.narrow')::text$q$),
    (2,  'table logical   narrow  fnv64', $q$SELECT pg_table_logical_checksum('bench.narrow', 'fnv64')::text$q$),
    (3,  'table logical   text    fnv64', $q$SELECT pg_table_logical_checksum('bench.text', 'fnv64')::text$q$),
    (4,  'table logical   wide    fnv64', $q$SELECT pg_table_logical_checksum('bench.wide', 'fnv64')::text$q$),
    (5,  'table logical   toast   fnv64', $q$SELECT pg_table_logical_checksum('bench.toast', 'fnv64')::text$q$),
    (6,  'table physical  narrow  fnv64', $q$SELECT pg_table_physical_checksum('bench.narrow', false, 'fnv64')::text$q$),
    (7,  'table physical  wide    fnv32', $q$SELECT pg_table_physical_checksum('bench.wide')::text$q$),
    (8,  'index physical  narrow_a_b fnv64', $q$SELECT pg_index_physical_checksum('bench.narrow_a_b', 'fnv64')::text$q$),
    (9,  'index logical   narrow_a_b fnv64', $q$SELECT pg_index_logical_checksum('bench.narrow_a_b', 'fnv64')::text$q$),
    (10, 'index logical   text_lower_name fnv64', $q$SELECT pg_index_logical_checksum('bench.text_lower_name', 'fnv64')::text$q$),
    (11, 'database logical  fnv32 (+indexes)', $q$SELECT pg_database_logical_checksum()::text$q$),
    (12, 'database logical  fnv64 (+indexes)', $q$SELECT pg_database_logical_checksum(false, true, 'fnv64')::text$q$),
    (13, 'database logical  fnv64 (no indexes)', $q$SELECT pg_database_logical_checksum(false, false, 'fnv64')::text$q$),
    (14, 'database physical fnv64', $q$SELECT pg_database_physical_checksum(false, false, 'fnv64')::text$q$)
) AS c(n, label, query),
LATERAL pg_temp.bench(c.label, c.query, :runs) AS b
ORDER BY c.n;
