-- bench/setup.sql - benchmark data set for pg_integrity_checksums
--
-- Creates a dedicated schema "bench" with tables that exercise the hot paths
-- of the checksum functions:
--
--   bench.narrow  fixed-width columns (int, bigint, timestamptz, bool, date)
--   bench.text    variable-width columns (text, varchar, numeric, text[])
--   bench.wide    60 columns with NULLs (attribute access cost)
--   bench.toast   large values stored out of line (detoasting cost)
--
-- plus several indexes per table, including an expression index and a
-- partial index, so that the logical database checksum has to evaluate index
-- definitions on the heap.
--
-- Size is controlled with the psql variable "scale" (default 1):
--   psql -v scale=2 -f bench/setup.sql

\set ON_ERROR_STOP on

\if :{?scale}
\else
\set scale 1
\endif
SELECT set_config('bench.scale', :'scale', false);

SET client_min_messages = warning;
DROP SCHEMA IF EXISTS bench CASCADE;
CREATE SCHEMA bench;

SELECT setseed(0.42);

CREATE TABLE bench.narrow (
    id      integer PRIMARY KEY,
    a       integer NOT NULL,
    b       bigint,
    ts      timestamptz,
    flag    boolean,
    d       date
);

INSERT INTO bench.narrow
SELECT g,
       ((g::bigint * 7919) % 100000)::int,
       g::bigint * 1000003,
       timestamptz '2024-01-01 00:00:00+00' + g * interval '1 second',
       g % 3 = 0,
       date '2020-01-01' + (g % 3650)
FROM generate_series(1, 1000000 * :scale) AS g;

CREATE INDEX narrow_a_b ON bench.narrow (a, b);
CREATE INDEX narrow_ts ON bench.narrow (ts) WHERE flag;

CREATE TABLE bench.text (
    id      integer PRIMARY KEY,
    name    text,
    code    varchar(32),
    amount  numeric(14, 2),
    tags    text[]
);

INSERT INTO bench.text
SELECT g,
       'name-' || md5(g::text),
       substr(md5((g * 31)::text), 1, 12),
       round((g * 1.37)::numeric, 2),
       ARRAY['t' || (g % 17), 't' || (g % 29)]
FROM generate_series(1, 400000 * :scale) AS g;

CREATE INDEX text_lower_name ON bench.text (lower(name));
CREATE INDEX text_code ON bench.text (code) INCLUDE (amount);

-- 60 columns, every third one NULL in a rotating pattern.
DO $$
DECLARE
    cols text := '';
    vals text := '';
    i    int;
BEGIN
    FOR i IN 1..60 LOOP
        IF i % 2 = 0 THEN
            cols := cols || format(', c%s text', i);
            vals := vals || format(', CASE WHEN (g + %s) %% 3 = 0 THEN NULL ELSE ''v%s-'' || g END', i, i);
        ELSE
            cols := cols || format(', c%s integer', i);
            vals := vals || format(', CASE WHEN (g + %s) %% 3 = 0 THEN NULL ELSE g + %s END', i, i);
        END IF;
    END LOOP;

    EXECUTE format('CREATE TABLE bench.wide (id integer PRIMARY KEY%s)', cols);
    EXECUTE format('INSERT INTO bench.wide SELECT g%s FROM generate_series(1, %s) AS g',
                   vals, 100000 * current_setting('bench.scale')::int);
END
$$;

CREATE TABLE bench.toast (
    id      integer PRIMARY KEY,
    payload text
);
ALTER TABLE bench.toast ALTER COLUMN payload SET STORAGE EXTERNAL;

INSERT INTO bench.toast
SELECT g, repeat(md5(g::text), 100)
FROM generate_series(1, 20000 * :scale) AS g;

VACUUM (FREEZE, ANALYZE) bench.narrow, bench.text, bench.wide, bench.toast;
