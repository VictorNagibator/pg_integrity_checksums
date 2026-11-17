-- Per-relation database checksums
--
-- pg_database_logical_checksums() and pg_database_physical_checksums() return
-- one row per relation of a database scan.  Every checksum must equal the
-- per-relation function with the same algorithm, serial and parallel scans
-- must agree, the relations filter must return a subset of the full result,
-- and a changed table must show up as exactly its own rows.

SET client_min_messages = warning;

CREATE SCHEMA dbrel;

CREATE TABLE dbrel.t (
    id      integer PRIMARY KEY,
    a       integer,
    b       text,
    c       numeric
);
INSERT INTO dbrel.t
SELECT g, g % 37, 'Row ' || g, g / 7.0 FROM generate_series(1, 2000) AS g;
CREATE INDEX dbrel_t_a ON dbrel.t (a);
CREATE INDEX dbrel_t_lower_b ON dbrel.t (lower(b));
CREATE INDEX dbrel_t_partial ON dbrel.t (c) WHERE a < 10;

-- Large values, so the table has a TOAST relation with data in it.
CREATE TABLE dbrel.toasty (id integer, payload text);
INSERT INTO dbrel.toasty
SELECT g, string_agg(md5((g * 1000 + i)::text), '')
FROM generate_series(1, 20) AS g, generate_series(1, 200) AS i
GROUP BY g;

CREATE TABLE dbrel.empty (x integer);
CREATE MATERIALIZED VIEW dbrel.mv AS SELECT a, count(*) AS n FROM dbrel.t GROUP BY a;

-- Temporary relations are never part of a database scan.
CREATE TEMP TABLE dbrel_temp (x integer);
INSERT INTO dbrel_temp VALUES (1);

-- Expected checksum of one relation, from the per-relation functions.
CREATE FUNCTION dbrel_expected(rel regclass, physical boolean, algo text)
RETURNS bytea LANGUAGE sql AS $$
    SELECT CASE
        WHEN physical AND c.relkind = 'i' THEN pg_index_physical_checksum(rel, algo)
        WHEN physical THEN pg_table_physical_checksum(rel, false, algo)
        WHEN c.relkind = 'i' THEN pg_index_logical_checksum(rel, algo)
        ELSE pg_table_logical_checksum(rel, algo)
    END
    FROM pg_class c WHERE c.oid = rel
$$;

-- The relations of the dbrel schema and its TOAST tables.
CREATE VIEW dbrel_rels AS
SELECT c.oid::regclass AS rel
FROM pg_class c
WHERE c.relnamespace = 'dbrel'::regnamespace
   OR c.oid IN (SELECT reltoastrelid FROM pg_class
                WHERE relnamespace = 'dbrel'::regnamespace);

-- Rows of the dbrel schema for every mode and algorithm.
CREATE TEMP TABLE dbrel_result AS
SELECT 'logical' AS mode, 'fnv32' AS algo, r.*
FROM pg_database_logical_checksums() AS r WHERE r.schema_name = 'dbrel'
UNION ALL
SELECT 'logical', 'fnv64', r.*
FROM pg_database_logical_checksums(algorithm => 'fnv64') AS r WHERE r.schema_name = 'dbrel'
UNION ALL
SELECT 'physical', 'fnv32', r.*
FROM pg_database_physical_checksums(include_toast => true,
                                    relations => (SELECT array_agg(rel) FROM dbrel_rels)) AS r
UNION ALL
SELECT 'physical', 'fnv64', r.*
FROM pg_database_physical_checksums(include_toast => true,
                                    relations => (SELECT array_agg(rel) FROM dbrel_rels),
                                    algorithm => 'fnv64') AS r;

-- Which relations each mode returns.  Logical scans leave TOAST relations
-- out (the parent table covers their content); physical scans include them
-- with include_toast.
SELECT mode, algo,
       CASE WHEN schema_name = 'pg_toast' THEN 'pg_toast.*' ELSE rel_name::text END AS rel,
       relkind, status, reason, length(checksum) AS bytes
FROM dbrel_result
ORDER BY mode, algo, rel;

-- Every checksum equals the per-relation function.
SELECT mode, algo, relid
FROM dbrel_result
WHERE checksum IS DISTINCT FROM dbrel_expected(relid, mode = 'physical', algo);

-- No temporary relation appears, with or without a filter.
SELECT count(*) AS temp_rows
FROM pg_database_logical_checksums(relations => ARRAY['dbrel_temp'::regclass]);
SELECT count(*) AS temp_rows
FROM pg_database_physical_checksums() AS r
WHERE r.relid = 'dbrel_temp'::regclass;

-- include_indexes = false leaves the indexes out.
SELECT rel_name, relkind
FROM pg_database_logical_checksums(include_indexes => false) AS r
WHERE r.schema_name = 'dbrel'
ORDER BY rel_name;

-- A parallel scan returns exactly the rows of the serial scan.
SET pg_integrity_checksums.workers = 2;
CREATE TEMP TABLE dbrel_parallel AS
SELECT 'logical' AS mode, 'fnv32' AS algo, r.*
FROM pg_database_logical_checksums() AS r WHERE r.schema_name = 'dbrel'
UNION ALL
SELECT 'logical', 'fnv64', r.*
FROM pg_database_logical_checksums(algorithm => 'fnv64') AS r WHERE r.schema_name = 'dbrel'
UNION ALL
SELECT 'physical', 'fnv32', r.*
FROM pg_database_physical_checksums(include_toast => true,
                                    relations => (SELECT array_agg(rel) FROM dbrel_rels)) AS r
UNION ALL
SELECT 'physical', 'fnv64', r.*
FROM pg_database_physical_checksums(include_toast => true,
                                    relations => (SELECT array_agg(rel) FROM dbrel_rels),
                                    algorithm => 'fnv64') AS r;
RESET pg_integrity_checksums.workers;

(SELECT 'serial only' AS side, * FROM dbrel_result EXCEPT SELECT 'serial only', * FROM dbrel_parallel)
UNION ALL
(SELECT 'parallel only', * FROM dbrel_parallel EXCEPT SELECT 'parallel only', * FROM dbrel_result);
SELECT (SELECT count(*) FROM dbrel_result) = (SELECT count(*) FROM dbrel_parallel)
       AS same_row_count;

-- The parallel scans above ran inside CREATE TABLE AS, a statement that has
-- used its command ID.  On PostgreSQL 17+ the leader's subtransactions
-- failed there ("cannot start commands during a parallel operation").  The
-- aggregate functions share the scan; check one of them the same way.
SET pg_integrity_checksums.workers = 2;
CREATE TEMP TABLE dbrel_agg AS
SELECT pg_database_logical_checksum(false, true, 'fnv64') AS parallel_value;
RESET pg_integrity_checksums.workers;
SELECT parallel_value = pg_database_logical_checksum(false, true, 'fnv64')
       AS parallel_in_ctas_matches_serial
FROM dbrel_agg;

-- The relations filter returns the matching rows of the full scan; NULL
-- elements are ignored, and an empty list returns nothing.
SELECT r.rel_name, r.checksum = f.checksum AS same_checksum
FROM pg_database_logical_checksums(
         relations => ARRAY['dbrel.t'::regclass, NULL, 'dbrel.dbrel_t_a'::regclass]) AS r
JOIN dbrel_result f ON f.mode = 'logical' AND f.algo = 'fnv32' AND f.relid = r.relid
ORDER BY r.rel_name;
SELECT count(*) AS empty_list_rows
FROM pg_database_logical_checksums(relations => '{}');

-- A listed relation outside the scan (a catalog without include_system, a
-- view) returns no row.
SELECT count(*) AS out_of_scope_rows
FROM pg_database_logical_checksums(relations => ARRAY['pg_class'::regclass]);
SELECT count(*) AS catalog_rows
FROM pg_database_logical_checksums(include_system => true,
                                   relations => ARRAY['pg_class'::regclass]);

-- information_schema is a system schema too: its tables hold the server
-- version, so they would make the checksum differ between PostgreSQL
-- versions.  Only include_system brings them in, in both modes.
SELECT count(*) AS infoschema_rows_logical
FROM pg_database_logical_checksums()
WHERE schema_name = 'information_schema';
SELECT count(*) AS infoschema_rows_physical
FROM pg_database_physical_checksums()
WHERE schema_name = 'information_schema';
SELECT count(*) > 0 AS infoschema_rows_with_system
FROM pg_database_logical_checksums(include_system => true)
WHERE schema_name = 'information_schema';

-- After a change to one table, only its own rows differ: the table and the
-- indexes whose entries changed.  The partial index does not cover the row.
UPDATE dbrel.t SET b = 'changed' WHERE id = 50;
SELECT r.rel_name
FROM pg_database_logical_checksums() AS r
JOIN dbrel_result f ON f.mode = 'logical' AND f.algo = 'fnv32' AND f.relid = r.relid
WHERE r.checksum IS DISTINCT FROM f.checksum
ORDER BY r.rel_name;
UPDATE dbrel.t SET b = 'Row 50' WHERE id = 50;
SELECT count(*) AS differing_after_revert
FROM pg_database_logical_checksums() AS r
JOIN dbrel_result f ON f.mode = 'logical' AND f.algo = 'fnv32' AND f.relid = r.relid
WHERE r.checksum IS DISTINCT FROM f.checksum;

-- An invalid index is reported in strict mode and returned as skipped
-- otherwise; the other rows are unaffected.
UPDATE pg_index SET indisvalid = false
WHERE indexrelid = 'dbrel.dbrel_t_a'::regclass;
SELECT count(*) FROM pg_database_logical_checksums(relations => ARRAY['dbrel.dbrel_t_a'::regclass]);
SET pg_integrity_checksums.strict = off;
SELECT rel_name, checksum, status, reason
FROM pg_database_logical_checksums(
         relations => ARRAY['dbrel.t'::regclass, 'dbrel.dbrel_t_a'::regclass]) AS r
ORDER BY rel_name;
UPDATE pg_index SET indisvalid = true
WHERE indexrelid = 'dbrel.dbrel_t_a'::regclass;

-- A relation whose checksum fails is returned with status error in
-- non-strict mode (the warning names it by OID, so it is hidden here).
CREATE ACCESS METHOD dbrel_heap2 TYPE TABLE HANDLER heap_tableam_handler;
CREATE TABLE dbrel.other_am (a integer) USING dbrel_heap2;
INSERT INTO dbrel.other_am VALUES (1);
SET client_min_messages = error;
SELECT rel_name, checksum, status, reason
FROM pg_database_logical_checksums(
         relations => ARRAY['dbrel.other_am'::regclass, 'dbrel.empty'::regclass]) AS r
ORDER BY rel_name;
SET client_min_messages = warning;
RESET pg_integrity_checksums.strict;
-- Strict mode fails instead (the context names the OID, so print the message).
DO $$
BEGIN
    PERFORM count(*) FROM pg_database_logical_checksums(
        relations => ARRAY['dbrel.other_am'::regclass]);
    RAISE NOTICE 'no error';
EXCEPTION WHEN feature_not_supported THEN
    RAISE WARNING 'strict: %', SQLERRM;
END
$$;
DROP TABLE dbrel.other_am;
DROP ACCESS METHOD dbrel_heap2;

-- Argument checks.
SELECT * FROM pg_database_logical_checksums(algorithm => 'md5');
SELECT * FROM pg_database_logical_checksums(include_system => NULL);
SELECT * FROM pg_database_physical_checksums(algorithm => NULL);
-- Usable in the target list too.
SELECT pg_database_logical_checksums(relations => ARRAY['dbrel.empty'::regclass]);

-- Superuser only, like the aggregate database functions.
CREATE ROLE regress_dbrel_user;
GRANT EXECUTE ON FUNCTION pg_database_logical_checksums(boolean, boolean, regclass[], text)
    TO regress_dbrel_user;
SET ROLE regress_dbrel_user;
SELECT * FROM pg_database_logical_checksums();
SELECT * FROM pg_database_physical_checksums();
RESET ROLE;
REVOKE EXECUTE ON FUNCTION pg_database_logical_checksums(boolean, boolean, regclass[], text)
    FROM regress_dbrel_user;
DROP ROLE regress_dbrel_user;

-- Cleanup.
DROP SCHEMA dbrel CASCADE;
DROP FUNCTION dbrel_expected(regclass, boolean, text);
DROP TABLE dbrel_temp, dbrel_result, dbrel_parallel, dbrel_agg;
RESET client_min_messages;
