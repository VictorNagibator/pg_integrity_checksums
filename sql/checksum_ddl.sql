-- DDL stability tests: physical layout changes must not change logical
-- checksums, while schema/content changes that alter row content must.

CREATE TABLE test_ddl_t (id integer, v text);
INSERT INTO test_ddl_t
SELECT gs, 'value_' || gs FROM generate_series(1, 200) gs;
CREATE INDEX test_ddl_idx ON test_ddl_t (v);

SELECT pg_table_logical_checksum('test_ddl_t'::regclass) AS base_logical,
       pg_index_logical_checksum('test_ddl_idx'::regclass) AS base_index_logical,
       pg_database_logical_checksum(false, false) AS base_db_tables,
       pg_database_logical_checksum(false, true) AS base_db_indexes
\gset

-- Renaming a table or column does not change row content.
ALTER TABLE test_ddl_t RENAME TO test_ddl_renamed;
ALTER TABLE test_ddl_renamed RENAME COLUMN v TO value_text;
SELECT pg_table_logical_checksum('test_ddl_renamed'::regclass) = :base_logical
       AS table_rename_logical_stable,
       pg_index_logical_checksum('test_ddl_idx'::regclass) = :base_index_logical
       AS index_rename_logical_stable;

ALTER INDEX test_ddl_idx RENAME TO test_ddl_idx_renamed;
SELECT pg_index_logical_checksum('test_ddl_idx_renamed'::regclass) = :base_index_logical
       AS index_rename_only_logical_stable;

-- Adding a nullable column changes the logical row format; dropping it
-- restores the original logical content because dropped columns are skipped.
ALTER TABLE test_ddl_renamed ADD COLUMN extra integer;
SELECT pg_table_logical_checksum('test_ddl_renamed'::regclass) <> :base_logical
       AS add_column_changes_logical;
ALTER TABLE test_ddl_renamed DROP COLUMN extra;
SELECT pg_table_logical_checksum('test_ddl_renamed'::regclass) = :base_logical
       AS drop_column_restores_logical;

-- A type change changes the type tag and therefore the logical checksum.
ALTER TABLE test_ddl_renamed ALTER COLUMN value_text TYPE varchar(100);
SELECT pg_table_logical_checksum('test_ddl_renamed'::regclass) <> :base_logical
       AS type_change_changes_logical;
SELECT pg_table_logical_checksum('test_ddl_renamed'::regclass) AS table_after_type,
       pg_database_logical_checksum(false, false) AS db_tables_after_type,
       pg_database_logical_checksum(false, true) AS db_indexes_after_type
\gset

-- Creating and dropping an index does not change table content.  Database
-- logical checksums change only when indexes are included.
CREATE INDEX test_ddl_second_idx ON test_ddl_renamed (id);
SELECT pg_table_logical_checksum('test_ddl_renamed'::regclass) = :table_after_type
       AS table_logical_after_type_change,
       pg_database_logical_checksum(false, false) = :db_tables_after_type
       AS db_tables_unaffected_by_index,
       pg_database_logical_checksum(false, true) <> :db_indexes_after_type
       AS db_indexes_affected_by_index;
DROP INDEX test_ddl_second_idx;
SELECT pg_database_logical_checksum(false, true) = :db_indexes_after_type
       AS db_indexes_restored_after_drop;

-- Non-rewriting storage options do not change logical checksums.
ALTER TABLE test_ddl_renamed SET (fillfactor = 70);
SELECT pg_table_logical_checksum('test_ddl_renamed'::regclass) = :table_after_type
       AS logical_after_fillfactor;

-- TRUNCATE removes all visible rows and changes the logical checksum.
TRUNCATE test_ddl_renamed;
SELECT pg_table_logical_checksum('test_ddl_renamed'::regclass) <> :table_after_type
       AS truncate_changes_logical;

-- Adding an enum value does not change existing enum data.
CREATE TYPE test_ddl_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TABLE test_ddl_enum (v test_ddl_mood);
INSERT INTO test_ddl_enum VALUES ('ok');
SELECT pg_table_logical_checksum('test_ddl_enum'::regclass) AS enum_before
\gset
ALTER TYPE test_ddl_mood ADD VALUE 'new_value';
SELECT pg_table_logical_checksum('test_ddl_enum'::regclass) = :enum_before
       AS enum_add_value_logical_stable;

-- Cleanup.
DROP TABLE test_ddl_renamed;
DROP TABLE test_ddl_enum;
DROP TYPE test_ddl_mood;
