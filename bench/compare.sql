-- bench/compare.sql - checksum cost relative to reading the same data
--
-- Times, on the pgbench_accounts table created by "pgbench -i":
--
--   * a sequential scan that only counts rows (the floor: read every page);
--   * a hand-written order-independent hash of every row in SQL;
--   * the extension's logical and physical table checksums;
--   * the extension's logical database checksum (tables and indexes).
--
-- Every case runs once to warm up and then "runs" times; the median is
-- reported.  "workers" sets both max_parallel_workers_per_gather (for the
-- plain SQL queries) and pg_integrity_checksums.workers, so serial and
-- parallel runs are compared like for like.
--
--   psql -v runs=3 -v workers=0 -f bench/compare.sql

\set ON_ERROR_STOP on
\pset footer off

\if :{?runs}
\else
\set runs 3
\endif
\if :{?workers}
\else
\set workers 0
\endif

SET max_parallel_workers_per_gather = :workers;
SET pg_integrity_checksums.workers = :workers;
SET client_min_messages = warning;

CREATE OR REPLACE FUNCTION pg_temp.bench(query text, runs int)
RETURNS numeric
LANGUAGE plpgsql AS $$
DECLARE
    t0      timestamptz;
    times   float8[] := '{}';
    r       text;
BEGIN
    EXECUTE query INTO r;                       -- warm-up
    FOR i IN 1..runs LOOP
        t0 := clock_timestamp();
        EXECUTE query INTO r;
        times := times || extract(epoch FROM clock_timestamp() - t0)::float8 * 1000;
    END LOOP;
    RETURN round((SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY x)
                  FROM unnest(times) AS x)::numeric, 0);
END
$$;

WITH cases(n, bench_case, query) AS (VALUES
    (1, 'seq scan: count(*)',
        $q$SELECT count(*) FROM pgbench_accounts$q$),
    (2, 'SQL: sum(hashtextextended(row::text, 0))',
        $q$SELECT sum(hashtextextended(t::text, 0)) FROM pgbench_accounts AS t$q$),
    (3, 'pg_table_logical_checksum (fnv32)',
        $q$SELECT pg_table_logical_checksum('pgbench_accounts')$q$),
    (4, 'pg_table_logical_checksum (xxh3)',
        $q$SELECT pg_table_logical_checksum('pgbench_accounts', 'xxh3')$q$),
    (5, 'pg_table_physical_checksum (fnv32)',
        $q$SELECT pg_table_physical_checksum('pgbench_accounts', false)$q$),
    (6, 'pg_database_logical_checksum (+indexes)',
        $q$SELECT pg_database_logical_checksum()$q$)
),
timed AS (
    SELECT n, bench_case, pg_temp.bench(query, :runs) AS median_ms
    FROM cases
    ORDER BY n
)
SELECT bench_case,
       median_ms,
       round(median_ms / first_value(median_ms) OVER (ORDER BY n), 1) AS x_seq_scan
FROM timed
ORDER BY n;
