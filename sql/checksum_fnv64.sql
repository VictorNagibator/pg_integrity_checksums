-- Generic checksum API and FNV-1a 64-bit primitives.

SELECT encode(pg_data_checksum('hello'::bytea, 0, 'fnv32'), 'hex') = '4f9f2cab' AS fnv32_ok;

SELECT encode(pg_data_checksum('hello'::bytea, 0, 'fnv64'), 'hex') = 'a430d84680aabd0b' AS fnv64_ok;

SELECT pg_checksum_bigint(pg_data_checksum('hello'::bytea, 0, 'fnv64')) = -6615550055289275125 AS bigint_ok;

SELECT pg_checksum_bigint(pg_data_checksum('hello'::bytea, 0, 'fnv32')) = pg_data_checksum('hello'::bytea, 0) AS legacy_ok;

SELECT length(pg_data_checksum('hello'::bytea, 0, 'fnv32')) = 4 AND length(pg_data_checksum('hello'::bytea, 0, 'fnv64')) = 8 AS lengths_ok;

SELECT encode(pg_text_checksum('hello', 0, 'fnv64'), 'hex') = encode(pg_data_checksum('hello'::bytea, 0, 'fnv64'), 'hex') AS text_ok;

-- Logical table/index digests are selectable and stay compatible with the
-- legacy 32-bit wrappers.
CREATE TABLE fnv64_a (id integer PRIMARY KEY, value text);
INSERT INTO fnv64_a SELECT i, 'v' || i FROM generate_series(1, 20) i;
CREATE TABLE fnv64_b (id integer PRIMARY KEY, value text);
INSERT INTO fnv64_b SELECT i, 'v' || i FROM generate_series(20, 1, -1) i;

SELECT length(pg_table_logical_checksum('fnv64_a'::regclass, 'fnv64')) = 8 AS table64_len;
SELECT pg_table_logical_checksum('fnv64_a'::regclass, 'fnv64') = pg_table_logical_checksum('fnv64_b'::regclass, 'fnv64') AS table64_order_ok;
SELECT length(pg_index_logical_checksum('fnv64_a_pkey'::regclass, 'fnv64')) = 8 AS index64_len;
SELECT length(pg_table_physical_checksum('fnv64_a'::regclass, false, 'fnv64')) = 8 AS table_physical64_len;
SELECT pg_checksum_bigint(pg_table_physical_checksum('fnv64_a'::regclass, false, 'fnv32')) = pg_table_physical_checksum('fnv64_a'::regclass, false) AS table_physical32_legacy_ok;
SELECT length(pg_index_physical_checksum('fnv64_a_pkey'::regclass, 'fnv64')) = 8 AS index_physical64_len;
SELECT pg_checksum_bigint(pg_index_physical_checksum('fnv64_a_pkey'::regclass, 'fnv32')) = pg_index_physical_checksum('fnv64_a_pkey'::regclass) AS index_physical32_legacy_ok;
SELECT length(pg_database_physical_checksum(false, false, 'fnv64')) = 8 AS database_physical64_len;
SELECT pg_checksum_bigint(pg_database_physical_checksum(false, false, 'fnv32')) = pg_database_physical_checksum(false, false) AS database_physical32_legacy_ok;
SELECT pg_index_logical_checksum('fnv64_a_pkey'::regclass, 'fnv64') = pg_index_logical_checksum('fnv64_b_pkey'::regclass, 'fnv64') AS index64_order_ok;
SELECT pg_checksum_bigint(pg_table_logical_checksum('fnv64_a'::regclass, 'fnv32')) = pg_table_logical_checksum('fnv64_a'::regclass) AS table32_legacy_ok;
SELECT pg_checksum_bigint(pg_index_logical_checksum('fnv64_a_pkey'::regclass, 'fnv32')) = pg_index_logical_checksum('fnv64_a_pkey'::regclass) AS index32_legacy_ok;
SELECT length(pg_cell_checksum('fnv64_a'::regclass, '(0,1)', 2, 'fnv64')) = 8 AS cell64_len;
SELECT pg_checksum_bigint(pg_cell_checksum('fnv64_a'::regclass, '(0,1)', 2, 'fnv32')) = pg_cell_checksum('fnv64_a'::regclass, '(0,1)', 2) AS cell32_legacy_ok;
SELECT length(pg_tuple_logical_checksum('fnv64_a'::regclass, '(0,1)', 'fnv64')) = 8 AS tuple64_len;
SELECT length(pg_tuple_physical_checksum('fnv64_a'::regclass, '(0,1)', false, 'fnv64')) = 8 AS tuple_physical64_len;
SELECT pg_checksum_bigint(pg_tuple_physical_checksum('fnv64_a'::regclass, '(0,1)', false, 'fnv32')) = pg_tuple_physical_checksum('fnv64_a'::regclass, '(0,1)', false) AS tuple_physical32_legacy_ok;
SELECT pg_checksum_bigint(pg_tuple_logical_checksum('fnv64_a'::regclass, '(0,1)', 'fnv32')) = pg_tuple_logical_checksum('fnv64_a'::regclass, '(0,1)') AS tuple32_legacy_ok;
SELECT length(pg_database_logical_checksum(false, true, 'fnv64')) = 8 AS database64_len;
SELECT pg_checksum_bigint(pg_database_logical_checksum(false, true, 'fnv32'), true) = pg_database_logical_checksum(false, true) AS database32_legacy_ok;
DROP TABLE fnv64_a, fnv64_b;
