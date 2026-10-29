-- Edge cases: zero-column tables, dropped columns, invalid indexes,
-- materialized views and relations without heap tuple storage.

-- Zero-column and all-dropped-column tables still have a logical row value.
CREATE TABLE test_errors_zero ();
INSERT INTO test_errors_zero DEFAULT VALUES;
SELECT pg_table_logical_checksum('test_errors_zero'::regclass) IS NOT NULL
       AS zero_column_table_logical,
       pg_table_physical_checksum('test_errors_zero'::regclass, false) IS NOT NULL
       AS zero_column_table_physical,
       pg_cell_checksum('test_errors_zero'::regclass, '(0,1)'::tid, 1) IS NULL
       AS zero_column_cell_null;

CREATE TABLE test_errors_dropped (a integer, b text);
INSERT INTO test_errors_dropped VALUES (1, 'x');
ALTER TABLE test_errors_dropped DROP COLUMN a;
SELECT pg_table_logical_checksum('test_errors_dropped'::regclass) IS NOT NULL
       AS all_dropped_column_logical,
       pg_table_physical_checksum('test_errors_dropped'::regclass, false) IS NOT NULL
       AS all_dropped_column_physical;

-- A materialized view is a relation with heap storage and can be checksummed.
CREATE MATERIALIZED VIEW test_errors_mv AS
SELECT gs AS id, 'mv_' || gs AS v FROM generate_series(1, 20) gs;
SELECT pg_table_logical_checksum('test_errors_mv'::regclass) IS NOT NULL
       AS matview_logical,
       pg_table_physical_checksum('test_errors_mv'::regclass, false) IS NOT NULL
       AS matview_physical;

-- A sequence and an index have storage, so page checksums are valid.
CREATE SEQUENCE test_errors_seq;
SELECT nextval('test_errors_seq');
CREATE TABLE test_errors_page_t (id integer, v text);
INSERT INTO test_errors_page_t VALUES (1, 'x');
CREATE INDEX test_errors_page_idx ON test_errors_page_t (v);
SELECT
    pg_page_checksum('test_errors_seq'::regclass, 0) IS NOT NULL
        AS sequence_page_ok,
    pg_page_checksum('test_errors_page_idx'::regclass, 0) IS NOT NULL
        AS index_page_ok;

-- Invalid indexes have no logical content.
CREATE INDEX test_errors_invalid_idx ON test_errors_page_t (v);
UPDATE pg_index SET indisvalid = false
WHERE indexrelid = 'test_errors_invalid_idx'::regclass;
SELECT pg_index_logical_checksum('test_errors_invalid_idx'::regclass) IS NULL
       AS invalid_index_logical_null;

-- Strict database scans reject an invalid index; non-strict scans skip it.
SET pg_integrity_checksums.strict = on;
DO $$
DECLARE
    rejected boolean;
BEGIN
    rejected := false;
    BEGIN
        PERFORM pg_database_logical_checksum(false, true);
    EXCEPTION WHEN feature_not_supported THEN
        rejected := true;
    END;
    IF NOT rejected THEN
        RAISE EXCEPTION 'strict database scan accepted an invalid index';
    END IF;
END;
$$;
RESET pg_integrity_checksums.strict;

SET client_min_messages = error;
SELECT pg_database_logical_checksum(false, true) IS NOT NULL
       AS invalid_index_skipped;
RESET client_min_messages;

UPDATE pg_index SET indisvalid = true
WHERE indexrelid = 'test_errors_invalid_idx'::regclass;

-- An empty partitioned index still has a defined empty-content value.
CREATE TABLE test_errors_part (id integer, region text) PARTITION BY LIST (region);
CREATE INDEX test_errors_part_idx ON test_errors_part (region);
SELECT pg_index_logical_checksum('test_errors_part_idx'::regclass) IS NOT NULL
       AS empty_partitioned_index_logical;

-- Functions reject relations without heap tuple storage.
DO $$
DECLARE
    rejected boolean;
BEGIN
    rejected := false;
    BEGIN
        PERFORM pg_cell_checksum('test_errors_page_idx'::regclass, '(0,1)'::tid, 1);
    EXCEPTION WHEN OTHERS THEN
        rejected := true;
    END;
    IF NOT rejected THEN
        RAISE EXCEPTION 'cell checksum accepted an index';
    END IF;

    rejected := false;
    BEGIN
        PERFORM pg_table_logical_checksum('test_errors_page_idx'::regclass);
    EXCEPTION WHEN OTHERS THEN
        rejected := true;
    END;
    IF NOT rejected THEN
        RAISE EXCEPTION 'table logical checksum accepted an index';
    END IF;
END;
$$;

-- A dropped column is not a readable value; its cell checksum is NULL.
SELECT pg_cell_checksum('test_errors_dropped'::regclass, '(0,1)'::tid, 1) IS NULL
       AS dropped_column_cell_null;

-- Cleanup.
DROP TABLE test_errors_zero;
DROP TABLE test_errors_dropped;
DROP MATERIALIZED VIEW test_errors_mv;
DROP SEQUENCE test_errors_seq;
DROP INDEX test_errors_invalid_idx;
DROP TABLE test_errors_page_t;
DROP TABLE test_errors_part;
