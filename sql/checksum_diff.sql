-- Bucket, row and block range checksums: locating the rows that differ
-- between two copies of a table.

SET extra_float_digits = 0;

CREATE TABLE diff_a (id integer PRIMARY KEY, v text, d numeric);
INSERT INTO diff_a SELECT g, 'v' || g, g / 4.0 FROM generate_series(1, 2000) g;

-- Same rows in another physical order, then three differences.
CREATE TABLE diff_b (id integer PRIMARY KEY, v text, d numeric);
INSERT INTO diff_b SELECT * FROM diff_a ORDER BY id DESC;
UPDATE diff_b SET v = 'changed' WHERE id = 1234;
DELETE FROM diff_b WHERE id = 77;
INSERT INTO diff_b VALUES (5000, 'extra', 1);

-- One bucket is the table checksum, for both algorithms.
SELECT bucket, row_count,
       checksum = pg_table_logical_checksum('diff_a', 'fnv64') AS eq_table
FROM pg_table_bucket_checksums('diff_a', 1);
SELECT checksum = pg_table_logical_checksum('diff_a', 'fnv32') AS eq_table32,
       length(checksum) AS len
FROM pg_table_bucket_checksums('diff_a', 1, NULL, 'fnv32');

-- Every bucket is returned, empty ones included, and every row is counted.
SELECT count(*) AS buckets, sum(row_count) AS rows,
       min(bucket) AS first, max(bucket) AS last
FROM pg_table_bucket_checksums('diff_a', 64);
SELECT count(*) FILTER (WHERE row_count = 0) AS empty,
       count(DISTINCT checksum) FILTER (WHERE row_count = 0) AS empty_checksums
FROM pg_table_bucket_checksums('diff_a', 4096);

-- Bucket assignment and checksums are portable values.
SELECT bucket, row_count, encode(checksum, 'hex') AS checksum
FROM pg_table_bucket_checksums('diff_a', 8) ORDER BY bucket;

-- Bucket b of n is the union of buckets b*k .. b*k+k-1 of n*k.
SELECT bool_and(c.row_count = f.rows) AS nested
FROM pg_table_bucket_checksums('diff_a', 8) c
JOIN (SELECT bucket / 16 AS bucket, sum(row_count) AS rows
      FROM pg_table_bucket_checksums('diff_a', 128) GROUP BY 1) f
  USING (bucket);

-- Physical order does not matter: identical tables have identical buckets.
CREATE TABLE diff_c AS SELECT * FROM diff_a ORDER BY random();
ALTER TABLE diff_c ADD PRIMARY KEY (id);
SELECT count(*) AS differing
FROM pg_table_bucket_checksums('diff_a', 64) x
JOIN pg_table_bucket_checksums('diff_c', 64) y USING (bucket)
WHERE x.checksum <> y.checksum OR x.row_count <> y.row_count;

-- The changed, missing and extra rows are in differing buckets.
CREATE TEMP TABLE diff_buckets AS
SELECT x.bucket, x.row_count AS rows_a, y.row_count AS rows_b
FROM pg_table_bucket_checksums('diff_a', 64) x
JOIN pg_table_bucket_checksums('diff_b', 64) y USING (bucket)
WHERE x.checksum <> y.checksum;
SELECT * FROM diff_buckets ORDER BY bucket;

-- Comparing the rows of those buckets by key hash finds exactly the three
-- differences.
WITH sel AS (SELECT array_agg(bucket) AS b FROM diff_buckets),
ra AS (SELECT r.* FROM sel, pg_table_row_checksums('diff_a', 64, sel.b) r),
rb AS (SELECT r.* FROM sel, pg_table_row_checksums('diff_b', 64, sel.b) r)
SELECT coalesce(ra.key, rb.key) AS key,
       CASE WHEN ra.key IS NULL THEN 'only in diff_b'
            WHEN rb.key IS NULL THEN 'only in diff_a'
            ELSE 'changed' END AS difference
FROM ra FULL JOIN rb USING (key_hash)
WHERE ra.row_checksum IS DISTINCT FROM rb.row_checksum
ORDER BY 1;

-- Row checksums are the logical tuple checksums; key hashes are portable.
SELECT count(*) AS rows,
       count(*) FILTER (WHERE row_checksum <>
                        pg_tuple_logical_checksum('diff_a', ctid, 'fnv64'))
           AS mismatched,
       count(*) FILTER (WHERE tableoid <> 'diff_a'::regclass) AS other_table
FROM pg_table_row_checksums('diff_a');
SELECT count(*) FILTER (WHERE row_checksum <>
                        pg_tuple_logical_checksum('diff_a', ctid, 'fnv32'))
           AS mismatched32
FROM pg_table_row_checksums('diff_a', 1, NULL, NULL, 'fnv32');
SELECT bucket, encode(key_hash, 'hex') AS key_hash, key,
       encode(row_checksum, 'hex') AS row_checksum
FROM pg_table_row_checksums('diff_a', 8)
WHERE key IN ('(1)', '(2)', '(1000)') ORDER BY key;

-- The bucket of a row is the bucket that counts it.
SELECT bool_and(r.cnt = b.row_count) AS consistent
FROM pg_table_bucket_checksums('diff_a', 32) b
LEFT JOIN (SELECT bucket, count(*) AS cnt
           FROM pg_table_row_checksums('diff_a', 32) GROUP BY 1) r
  USING (bucket)
WHERE b.row_count > 0;

-- only_buckets filters; duplicates are harmless.
SELECT count(DISTINCT bucket) AS buckets, count(*) = (
         SELECT sum(row_count) FROM pg_table_bucket_checksums('diff_a', 16)
         WHERE bucket IN (3, 5)) AS rows_match
FROM pg_table_row_checksums('diff_a', 16, '{3,5,5}');
SELECT count(*) AS no_rows FROM pg_table_row_checksums('diff_a', 16, '{}');

-- Explicit keys, in key order.  The whole row is the key with an empty
-- array, or when there is no primary key.
SELECT key FROM pg_table_row_checksums('diff_a', 1, NULL, '{v,id}')
WHERE key LIKE '(v7,%' ORDER BY key;
SELECT key FROM pg_table_row_checksums('diff_a', 1, NULL, '{}')
WHERE key LIKE '(7,%';
CREATE TABLE diff_nopk AS SELECT * FROM diff_a;
SELECT (SELECT array_agg(checksum ORDER BY bucket)
        FROM pg_table_bucket_checksums('diff_nopk', 16)) =
       (SELECT array_agg(checksum ORDER BY bucket)
        FROM pg_table_bucket_checksums('diff_a', 16, '{}')) AS whole_row_default;
SELECT (SELECT array_agg(checksum ORDER BY bucket)
        FROM pg_table_bucket_checksums('diff_nopk', 16, '{id}')) =
       (SELECT array_agg(checksum ORDER BY bucket)
        FROM pg_table_bucket_checksums('diff_a', 16)) AS explicit_pk;
-- A composite primary key.
CREATE TABLE diff_pk2 (a integer, b text, c integer, PRIMARY KEY (b, a));
INSERT INTO diff_pk2 VALUES (1, 'x', 10), (2, 'y', 20);
SELECT key FROM pg_table_row_checksums('diff_pk2') ORDER BY key;

-- The key text is what ROW(...)::text prints, quoting included.
CREATE TABLE diff_quote (id integer, t text, d date, a integer[]);
INSERT INTO diff_quote VALUES
    (1, 'plain', '2026-01-02', '{1,2}'), (2, '', NULL, NULL),
    (3, 'a b', '2026-01-02', '{}'), (4, 'x"y\z', NULL, '{3}'),
    (5, '(,)', NULL, NULL), (NULL, NULL, NULL, NULL);
SET DateStyle = 'ISO, YMD';
SELECT count(*) AS rows,
       count(*) FILTER (WHERE r.key = ROW(q.id, q.t, q.d, q.a)::text) AS same
FROM pg_table_row_checksums('diff_quote', 1, NULL, '{}') r
JOIN diff_quote q ON q.ctid = r.ctid;
SELECT key FROM pg_table_row_checksums('diff_quote', 1, NULL, '{t,id}')
ORDER BY key COLLATE "C";
RESET DateStyle;
DROP TABLE diff_quote;

-- NULL key values and duplicate rows are kept apart.
CREATE TABLE diff_dup (k integer, v text);
INSERT INTO diff_dup VALUES (NULL, 'a'), (NULL, 'a'), (1, NULL), (1, 'b');
SELECT key, count(*), count(DISTINCT key_hash) AS hashes
FROM pg_table_row_checksums('diff_dup', 4, NULL, '{k}')
GROUP BY key ORDER BY key;
SELECT sum(row_count) AS rows
FROM pg_table_bucket_checksums('diff_dup', 4, '{k,v}');

-- Dropped columns are not part of the key or the row.
CREATE TABLE diff_drop (x integer, id integer PRIMARY KEY, v text);
INSERT INTO diff_drop SELECT g, g, 'v' || g FROM generate_series(1, 2000) g;
ALTER TABLE diff_drop DROP COLUMN x;
CREATE TABLE diff_nodrop (id integer PRIMARY KEY, v text);
INSERT INTO diff_nodrop SELECT g, 'v' || g FROM generate_series(1, 2000) g;
SELECT (SELECT array_agg(checksum ORDER BY bucket)
        FROM pg_table_bucket_checksums('diff_drop', 16)) =
       (SELECT array_agg(checksum ORDER BY bucket)
        FROM pg_table_bucket_checksums('diff_nodrop', 16)) AS dropped_ignored;

-- Partitioned tables: leaf rows are hashed in the parent's column order,
-- even for a partition that declares its columns in another order.
CREATE TABLE diff_part (id integer, v text, PRIMARY KEY (id))
    PARTITION BY RANGE (id);
CREATE TABLE diff_part1 PARTITION OF diff_part FOR VALUES FROM (0) TO (1000);
CREATE TABLE diff_part2 (v text, id integer NOT NULL);
ALTER TABLE diff_part ATTACH PARTITION diff_part2 FOR VALUES FROM (1000) TO (3000);
INSERT INTO diff_part SELECT id, v FROM diff_nodrop;
SELECT (SELECT array_agg(checksum ORDER BY bucket)
        FROM pg_table_bucket_checksums('diff_part', 16)) =
       (SELECT array_agg(checksum ORDER BY bucket)
        FROM pg_table_bucket_checksums('diff_nodrop', 16)) AS partitioned;
SELECT tableoid, count(*), min(key) AS some_key
FROM pg_table_row_checksums('diff_part', 1, NULL, '{}')
GROUP BY tableoid ORDER BY tableoid::text;

-- TOASTed values.
CREATE TABLE diff_toast (id integer PRIMARY KEY, big text);
ALTER TABLE diff_toast ALTER COLUMN big SET STORAGE EXTERNAL;
INSERT INTO diff_toast VALUES (1, repeat('abcdefgh', 100000)), (2, NULL);
SELECT key = '(1,' || repeat('abcdefgh', 100000) || ')' AS whole_row_key,
       row_checksum = pg_tuple_logical_checksum('diff_toast', ctid, 'fnv64')
           AS row_ok
FROM pg_table_row_checksums('diff_toast', 1, NULL, '{}') WHERE key LIKE '(1,%';

-- Only visible rows count.
BEGIN;
DELETE FROM diff_a WHERE id <= 100;
SELECT sum(row_count) AS rows_in_transaction
FROM pg_table_bucket_checksums('diff_a', 8);
ROLLBACK;

-- Block ranges.  A range that covers the table is the table checksum.
SELECT start_block, row_count,
       checksum = pg_table_logical_checksum('diff_a', 'fnv64') AS eq_table
FROM pg_table_block_checksums('diff_a', 100000);
SELECT count(*) = (pg_relation_size('diff_a') / current_setting('block_size')::int)
           AS one_per_block,
       sum(row_count) AS rows,
       bool_and(end_block = start_block + 1) AS ends
FROM pg_table_block_checksums('diff_a');
SELECT bool_and(end_block - start_block = 3) AS width,
       min(start_block) AS first
FROM pg_table_block_checksums('diff_a', 3, 'fnv32');
CREATE TABLE diff_empty (id integer);
SELECT count(*) AS no_ranges FROM pg_table_block_checksums('diff_empty');
SELECT * FROM pg_table_bucket_checksums('diff_empty', 2);

-- NULL relation: no rows, like a STRICT function.
SELECT count(*) FROM pg_table_bucket_checksums(NULL, 4);
SELECT count(*) FROM pg_table_row_checksums(NULL);
SELECT count(*) FROM pg_table_block_checksums(NULL);

-- Errors.
SELECT * FROM pg_table_bucket_checksums('diff_a', 0);
SELECT * FROM pg_table_bucket_checksums('diff_a', 1048577);
SELECT * FROM pg_table_bucket_checksums('diff_a', NULL);
SELECT * FROM pg_table_bucket_checksums('diff_a', 4, '{nope}');
SELECT * FROM pg_table_bucket_checksums('diff_a', 4, '{id,id}');
SELECT * FROM pg_table_bucket_checksums('diff_a', 4, '{id,NULL}');
SELECT * FROM pg_table_bucket_checksums('diff_a', 4, '{{id}}');
SELECT * FROM pg_table_bucket_checksums('diff_a', 4, NULL, 'md5');
SELECT * FROM pg_table_bucket_checksums('diff_a', 4, NULL, NULL);
SELECT * FROM pg_table_row_checksums('diff_a', 4, '{4}');
SELECT * FROM pg_table_row_checksums('diff_a', 4, '{-1}');
SELECT * FROM pg_table_row_checksums('diff_a', 4, '{1,NULL}');
SELECT * FROM pg_table_block_checksums('diff_a', 0);
SELECT * FROM pg_table_block_checksums('diff_part');
CREATE VIEW diff_view AS SELECT 1 AS x;
SELECT * FROM pg_table_bucket_checksums('diff_view', 2);
SELECT * FROM pg_table_block_checksums('diff_a_pkey');

-- Privileges and row-level security are enforced.
CREATE ROLE regress_diff_reader;
SET ROLE regress_diff_reader;
SELECT * FROM pg_table_bucket_checksums('diff_a', 2);
SELECT * FROM pg_table_row_checksums('diff_a');
SELECT * FROM pg_table_block_checksums('diff_a');
RESET ROLE;
GRANT SELECT ON diff_a TO regress_diff_reader;
ALTER TABLE diff_a ENABLE ROW LEVEL SECURITY;
SET ROLE regress_diff_reader;
SELECT * FROM pg_table_bucket_checksums('diff_a', 2);
RESET ROLE;
ALTER TABLE diff_a DISABLE ROW LEVEL SECURITY;
SET ROLE regress_diff_reader;
SELECT sum(row_count) AS rows FROM pg_table_bucket_checksums('diff_a', 2);
RESET ROLE;

DROP TABLE diff_a, diff_b, diff_c, diff_nopk, diff_pk2, diff_dup, diff_drop,
    diff_nodrop, diff_part, diff_toast, diff_empty;
DROP VIEW diff_view;
DROP ROLE regress_diff_reader;
