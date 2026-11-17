-- Parallel checksum tests (table, index, and database levels)
-- Verifies that parallel computation produces identical, deterministic results.

CREATE TABLE test_parallel (
    id integer PRIMARY KEY,
    group_id integer NOT NULL,
    data text NOT NULL,
    padding char(200) DEFAULT 'x'
);

INSERT INTO test_parallel (id, group_id, data)
SELECT gs, gs % 10, 'data_' || gs FROM generate_series(1, 5000) gs;

CREATE INDEX idx_test_parallel_group ON test_parallel (group_id);
CREATE INDEX idx_test_parallel_data ON test_parallel (data);
CREATE INDEX idx_test_parallel_hash ON test_parallel USING hash (data);

-- Test A: parallel table physical checksum equals serial
DO $$
DECLARE
    serial_val bigint;
    parallel_val bigint;
BEGIN
    PERFORM set_config('pg_integrity_checksums.workers', '0', true);
    serial_val := pg_table_physical_checksum('test_parallel'::regclass, false);
    PERFORM set_config('pg_integrity_checksums.workers', '4', true);
    parallel_val := pg_table_physical_checksum('test_parallel'::regclass, false);
    IF serial_val IS NULL OR serial_val != parallel_val THEN
        RAISE EXCEPTION 'Test A failed: parallel table physical checksum differs from serial';
    END IF;
END;
$$;

-- Test B: parallel table logical checksum equals serial
DO $$
DECLARE
    serial_val bigint;
    parallel_val bigint;
BEGIN
    PERFORM set_config('pg_integrity_checksums.workers', '0', true);
    serial_val := pg_table_logical_checksum('test_parallel'::regclass);
    PERFORM set_config('pg_integrity_checksums.workers', '4', true);
    parallel_val := pg_table_logical_checksum('test_parallel'::regclass);
    IF serial_val IS NULL OR serial_val != parallel_val THEN
        RAISE EXCEPTION 'Test B failed: parallel table logical checksum differs from serial';
    END IF;
END;
$$;

-- Test C: parallel index physical checksum equals serial
DO $$
DECLARE
    serial_val integer;
    parallel_val integer;
BEGIN
    PERFORM set_config('pg_integrity_checksums.workers', '0', true);
    serial_val := pg_index_physical_checksum('idx_test_parallel_group'::regclass);
    PERFORM set_config('pg_integrity_checksums.workers', '4', true);
    parallel_val := pg_index_physical_checksum('idx_test_parallel_group'::regclass);
    IF serial_val IS NULL OR serial_val != parallel_val THEN
        RAISE EXCEPTION 'Test C failed: parallel index physical checksum differs from serial';
    END IF;
END;
$$;

-- Test D: parallel index logical checksum equals serial
DO $$
DECLARE
    serial_val integer;
    parallel_val integer;
BEGIN
    PERFORM set_config('pg_integrity_checksums.workers', '0', true);
    serial_val := pg_index_logical_checksum('idx_test_parallel_group'::regclass);
    PERFORM set_config('pg_integrity_checksums.workers', '4', true);
    parallel_val := pg_index_logical_checksum('idx_test_parallel_group'::regclass);
    IF serial_val IS NULL OR serial_val != parallel_val THEN
        RAISE EXCEPTION 'Test D failed: parallel index logical checksum differs from serial';
    END IF;
END;
$$;

-- Test D2: parallel Hash index logical checksum equals serial.  Hash index
-- descriptors store int4 hash codes, while FormIndexDatum returns the
-- original text value; this covers the original-input-type path in workers.
DO $$
DECLARE
    serial_val integer;
    parallel_val integer;
BEGIN
    PERFORM set_config('pg_integrity_checksums.workers', '0', true);
    serial_val := pg_index_logical_checksum('idx_test_parallel_hash'::regclass);
    PERFORM set_config('pg_integrity_checksums.workers', '4', true);
    parallel_val := pg_index_logical_checksum('idx_test_parallel_hash'::regclass);
    IF serial_val IS NULL OR serial_val != parallel_val THEN
        RAISE EXCEPTION 'Test D2 failed: parallel hash index logical checksum differs from serial';
    END IF;
END;
$$;

-- Test E: parallel database physical checksum equals serial
DO $$
DECLARE
    serial_val bigint;
    parallel_val bigint;
BEGIN
    PERFORM set_config('pg_integrity_checksums.workers', '0', true);
    serial_val := pg_database_physical_checksum(false, false);
    PERFORM set_config('pg_integrity_checksums.workers', '4', true);
    parallel_val := pg_database_physical_checksum(false, false);
    IF serial_val IS NULL OR serial_val != parallel_val THEN
        RAISE EXCEPTION 'Test E failed: parallel database physical checksum differs from serial';
    END IF;
END;
$$;

-- Test F: parallel database logical checksum equals serial
DO $$
DECLARE
    serial_val bigint;
    parallel_val bigint;
BEGIN
    PERFORM set_config('pg_integrity_checksums.workers', '0', true);
    serial_val := pg_database_logical_checksum(false, false);
    PERFORM set_config('pg_integrity_checksums.workers', '4', true);
    parallel_val := pg_database_logical_checksum(false, false);
    IF serial_val IS NULL OR serial_val != parallel_val THEN
        RAISE EXCEPTION 'Test F failed: parallel database logical checksum differs from serial';
    END IF;
END;
$$;

-- Test G: parallel results are deterministic across repeated runs
DO $$
DECLARE
    first_val bigint;
    second_val bigint;
BEGIN
    PERFORM set_config('pg_integrity_checksums.workers', '4', true);
    first_val := pg_table_physical_checksum('test_parallel'::regclass, false);
    second_val := pg_table_physical_checksum('test_parallel'::regclass, false);
    IF first_val != second_val THEN
        RAISE EXCEPTION 'Test G failed: parallel checksum is not deterministic';
    END IF;
END;
$$;

-- Test H: parallel physical and logical checksums are different
DO $$
DECLARE
    physical_val bigint;
    logical_val bigint;
BEGIN
    PERFORM set_config('pg_integrity_checksums.workers', '4', true);
    physical_val := pg_table_physical_checksum('test_parallel'::regclass, false);
    logical_val := pg_table_logical_checksum('test_parallel'::regclass);
    IF physical_val = logical_val THEN
        RAISE EXCEPTION 'Test H failed: parallel physical and logical checksums should differ';
    END IF;
END;
$$;

-- Test I: parallel checksum changes after data modification
DO $$
DECLARE
    before_val bigint;
    after_val bigint;
BEGIN
    PERFORM set_config('pg_integrity_checksums.workers', '4', true);
    before_val := pg_table_logical_checksum('test_parallel'::regclass);

    UPDATE test_parallel SET data = 'modified' WHERE id = 1;
    after_val := pg_table_logical_checksum('test_parallel'::regclass);
    UPDATE test_parallel SET data = 'data_1' WHERE id = 1;

    IF before_val = after_val THEN
        RAISE EXCEPTION 'Test I failed: parallel checksum should change after data modification';
    END IF;
END;
$$;

-- Test J: parallel bucket checksums equal serial ones, for both algorithms,
-- the primary key, chosen key columns and the whole row, and for a
-- partitioned table with a partition whose columns are in another order.
CREATE TABLE test_parallel_part (id integer, data text) PARTITION BY RANGE (id);
CREATE TABLE test_parallel_part_1 PARTITION OF test_parallel_part
    FOR VALUES FROM (0) TO (2500);
CREATE TABLE test_parallel_part_2 (data text, id integer);
ALTER TABLE test_parallel_part ATTACH PARTITION test_parallel_part_2
    FOR VALUES FROM (2500) TO (10000);
INSERT INTO test_parallel_part
    SELECT i, repeat('p' || i, 40) FROM generate_series(1, 5000) i;

CREATE FUNCTION test_parallel_buckets() RETURNS TABLE(
    rel text, algo text, keys text, bucket integer, row_count bigint,
    checksum bytea)
LANGUAGE sql AS $$
    SELECT r, a, coalesce(k.k::text, 'pk'), b.*
    FROM unnest(ARRAY['test_parallel', 'test_parallel_part']) r,
         unnest(ARRAY['fnv32', 'fnv64']) a,
         (VALUES (NULL::text[]), ('{data}'), ('{}')) k(k),
         pg_table_bucket_checksums(r::regclass, 37, k.k, a) b
$$;

SET pg_integrity_checksums.workers = 0;
CREATE TEMP TABLE test_parallel_buckets_serial AS
    SELECT * FROM test_parallel_buckets();
SET pg_integrity_checksums.workers = 4;
CREATE TEMP TABLE test_parallel_buckets_par AS
    SELECT * FROM test_parallel_buckets();
RESET pg_integrity_checksums.workers;
SELECT count(*) AS bucket_rows,
       count(*) FILTER (WHERE s IS DISTINCT FROM p) AS differing,
       sum(s.row_count) AS serial_rows
FROM test_parallel_buckets_serial s
FULL JOIN test_parallel_buckets_par p USING (rel, algo, keys, bucket);
DROP TABLE test_parallel_buckets_serial, test_parallel_buckets_par;
DROP FUNCTION test_parallel_buckets();
DROP TABLE test_parallel_part;

-- Temporary relations cannot be read by parallel workers: they are scanned
-- serially, whatever the worker setting.
CREATE TEMP TABLE test_parallel_temp AS
    SELECT i AS id, 'data_' || i AS data FROM generate_series(1, 5000) i;
CREATE INDEX test_parallel_temp_idx ON test_parallel_temp (id);
SET pg_integrity_checksums.workers = 2;
SELECT pg_table_physical_checksum('test_parallel_temp') IS NOT NULL AS temp_table_physical;
SELECT pg_table_logical_checksum('test_parallel_temp') IS NOT NULL AS temp_table_logical;
SELECT pg_index_physical_checksum('test_parallel_temp_idx') IS NOT NULL AS temp_index_physical;
SELECT pg_index_logical_checksum('test_parallel_temp_idx') IS NOT NULL AS temp_index_logical;
SELECT sum(row_count) AS temp_bucket_rows
FROM pg_table_bucket_checksums('test_parallel_temp', 8);
RESET pg_integrity_checksums.workers;
DROP TABLE test_parallel_temp;

-- Clean up
DROP INDEX idx_test_parallel_group;
DROP INDEX idx_test_parallel_data;
DROP TABLE test_parallel;
