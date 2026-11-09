-- Additional FNV-1a 64-bit coverage: vectors, empty input, unsigned
-- conversion, NULLs, partition-independent logical checksums, TOAST and
-- parallel execution.

SELECT encode(pg_data_checksum(''::bytea, 0, 'fnv32'), 'hex') = '811c9dc5' AS empty_fnv32;
SELECT encode(pg_data_checksum(''::bytea, 0, 'fnv64'), 'hex') = 'cbf29ce484222325' AS empty_fnv64;
SELECT encode(pg_data_checksum('a'::bytea, 0, 'fnv32'), 'hex') = 'e40c292c' AS a_fnv32;
SELECT encode(pg_data_checksum('a'::bytea, 0, 'fnv64'), 'hex') = 'af63dc4c8601ec8c' AS a_fnv64;
SELECT pg_data_checksum('hello'::bytea, 0, 'fnv32') <> pg_data_checksum('hello'::bytea, 0, 'fnv64') AS algorithms_differ;
SELECT pg_data_checksum('hello'::bytea, 1, 'fnv64') <> pg_data_checksum('hello'::bytea, 0, 'fnv64') AS seed_changes_fnv64;
SELECT pg_data_checksum(NULL::bytea, 0, 'fnv64') IS NULL AS data_null;
SELECT pg_text_checksum(NULL::text, 0, 'fnv64') IS NULL AS text_null;

-- A 32-bit digest with the high bit set is signed by default and unsigned
-- when requested explicitly.
SELECT pg_checksum_bigint(pg_data_checksum(''::bytea, 0, 'fnv32')) = -2128831035 AS fnv32_signed;
SELECT pg_checksum_bigint(pg_data_checksum(''::bytea, 0, 'fnv32'), true) = 2166136261 AS fnv32_unsigned;

-- Cell/tuple NULL and invalid-TID behavior.
CREATE TABLE fnv64_nulls (a integer, b integer);
INSERT INTO fnv64_nulls VALUES (1, NULL);
SELECT length(pg_cell_checksum('fnv64_nulls'::regclass, '(0,1)', 2, 'fnv64')) = 8 AS cell_null_digest;
SELECT length(pg_tuple_logical_checksum('fnv64_nulls'::regclass, '(0,1)', 'fnv64')) = 8 AS tuple_with_null64;
SELECT pg_cell_checksum('fnv64_nulls'::regclass, '(0,1)', 9, 'fnv64') IS NULL AS cell_bad_attnum;
SELECT pg_tuple_logical_checksum('fnv64_nulls'::regclass, '(1000000,1)', 'fnv64') IS NULL AS tuple_bad_tid;

-- Empty tables still have a defined 64-bit digest.
CREATE TABLE fnv64_empty (a integer);
SELECT length(pg_table_logical_checksum('fnv64_empty'::regclass, 'fnv64')) = 8 AS empty_table_logical64;
SELECT length(pg_table_physical_checksum('fnv64_empty'::regclass, false, 'fnv64')) = 8 AS empty_table_physical64;

-- Logical table checksums describe rows, not the partitioning layout.
CREATE TABLE fnv64_plain (id integer, v text);
CREATE TABLE fnv64_part (id integer, v text) PARTITION BY RANGE (id);
CREATE TABLE fnv64_part_1 PARTITION OF fnv64_part FOR VALUES FROM (0) TO (100);
CREATE TABLE fnv64_part_2 PARTITION OF fnv64_part FOR VALUES FROM (100) TO (200);
INSERT INTO fnv64_plain SELECT i, 'p' || i FROM generate_series(1, 199) i;
INSERT INTO fnv64_part SELECT i, 'p' || i FROM generate_series(199, 1, -1) i;
SELECT pg_table_logical_checksum('fnv64_plain'::regclass, 'fnv64') =
       pg_table_logical_checksum('fnv64_part'::regclass, 'fnv64') AS partition_logical64_ok;

-- TOAST data is covered through the owning table.
CREATE TABLE fnv64_toast (id integer, body text);
INSERT INTO fnv64_toast VALUES (1, repeat('x', 100000));
SELECT length(pg_table_logical_checksum('fnv64_toast'::regclass, 'fnv64')) = 8 AS toast_logical64;
SELECT length(pg_table_physical_checksum('fnv64_toast'::regclass, false, 'fnv64')) = 8 AS toast_physical64;

-- A multi-block relation exercises more than one parallel chunk.
CREATE TABLE fnv64_big AS
SELECT i AS id, repeat('x', 200) AS body FROM generate_series(1, 20000) i;
CREATE INDEX fnv64_big_idx ON fnv64_big (id);

SET pg_integrity_checksums.workers = 0;
SELECT pg_table_physical_checksum('fnv64_big'::regclass, false, 'fnv64') AS tp64_big_serial,
       pg_table_logical_checksum('fnv64_big'::regclass, 'fnv64') AS tl64_big_serial,
       pg_index_physical_checksum('fnv64_big_idx'::regclass, 'fnv64') AS ip64_big_serial,
       pg_index_logical_checksum('fnv64_big_idx'::regclass, 'fnv64') AS il64_big_serial,
       pg_database_physical_checksum(false, false, 'fnv64') AS dp64_big_serial,
       pg_database_logical_checksum(false, true, 'fnv64') AS dl64_big_serial \gset
SET pg_integrity_checksums.workers = 2;
SELECT pg_table_physical_checksum('fnv64_big'::regclass, false, 'fnv64') = :'tp64_big_serial'::bytea AS table_physical_parallel64,
       pg_table_logical_checksum('fnv64_big'::regclass, 'fnv64') = :'tl64_big_serial'::bytea AS table_logical_parallel64,
       pg_index_physical_checksum('fnv64_big_idx'::regclass, 'fnv64') = :'ip64_big_serial'::bytea AS index_physical_parallel64,
       pg_index_logical_checksum('fnv64_big_idx'::regclass, 'fnv64') = :'il64_big_serial'::bytea AS index_logical_parallel64,
       pg_database_physical_checksum(false, false, 'fnv64') = :'dp64_big_serial'::bytea AS database_physical_parallel64,
       pg_database_logical_checksum(false, true, 'fnv64') = :'dl64_big_serial'::bytea AS database_logical_parallel64;
SET pg_integrity_checksums.workers = 0;

DROP TABLE fnv64_big, fnv64_toast, fnv64_plain, fnv64_part, fnv64_empty, fnv64_nulls;
