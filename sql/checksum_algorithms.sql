-- The crc32c and xxh3 algorithms.
--
-- Known-answer vectors come from outside this extension: the CRC-32C check
-- value of '123456789' is e3069283, and XXH3-64 of the empty input is
-- 2d06800538d394c2 (xxHash's own test vectors).  The other vectors were
-- computed with an independent bitwise CRC-32C and with xxHash 0.8.3.

-- Known answers.
SELECT s,
       encode(pg_text_checksum(s, 0, 'crc32c'), 'hex') AS crc32c,
       encode(pg_text_checksum(s, 0, 'xxh3'), 'hex') AS xxh3,
       encode(pg_text_checksum(s, 42, 'xxh3'), 'hex') AS xxh3_seed42
FROM unnest(ARRAY['', 'a', '123456789', 'hello world']) AS s;

-- Inputs longer than XXH3's short and mid-size paths (> 240 bytes) and
-- longer than CRC-32C's word-at-a-time loop.
SELECT encode(pg_data_checksum(b, 0, 'crc32c'), 'hex') AS long_crc32c,
       encode(pg_data_checksum(b, 0, 'xxh3'), 'hex') AS long_xxh3,
       encode(pg_data_checksum(b, 42, 'xxh3'), 'hex') AS long_xxh3_seed42
FROM (SELECT string_agg(set_byte('\x00'::bytea, 0, (i * 7) % 256), ''::bytea
                        ORDER BY i) AS b
      FROM generate_series(0, 4999) AS i) AS long_input;

-- For crc32c the seed is a previous CRC: passing one continues it, so the
-- CRC of a concatenation can be computed in parts.  Seed 0 is a fresh CRC.
SELECT pg_data_checksum('\x20776f726c64'::bytea,
                        pg_checksum_bigint(pg_data_checksum('\x68656c6c6f'::bytea, 0, 'crc32c'), true),
                        'crc32c')
       = pg_data_checksum('\x68656c6c6f20776f726c64'::bytea, 0, 'crc32c') AS crc32c_continues;
SELECT encode(pg_data_checksum('abc'::bytea, 42, 'crc32c'), 'hex') AS crc32c_seed42;

-- Digest lengths and the bigint conversion.
SELECT length(pg_data_checksum('x'::bytea, 0, 'crc32c')) AS crc32c_len,
       length(pg_data_checksum('x'::bytea, 0, 'xxh3')) AS xxh3_len,
       pg_checksum_bigint(pg_text_checksum('123456789', 0, 'crc32c')) AS crc32c_signed,
       pg_checksum_bigint(pg_text_checksum('123456789', 0, 'crc32c'), true) AS crc32c_unsigned,
       pg_checksum_bigint(pg_text_checksum('', 0, 'xxh3')) AS xxh3_bigint;
SELECT pg_data_checksum(NULL::bytea, 0, 'xxh3') IS NULL AS data_null,
       pg_text_checksum(NULL::text, 0, 'crc32c') IS NULL AS text_null;

-- Algorithm names are exact.
SELECT pg_text_checksum('x', 0, 'XXH3');
SELECT pg_text_checksum('x', 0, 'crc32');
SELECT pg_text_checksum('x', 0, 'xxh64');

-- Logical checksums are canonical, so their values are fixed: they do not
-- depend on the OID, the platform or the physical layout.  Pin them.
CREATE TYPE algo_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TABLE algo_t (
    id integer PRIMARY KEY,
    n numeric,
    a integer[],
    r int4range,
    m algo_mood,
    tz timetz,
    iv interval,
    t text
);
INSERT INTO algo_t VALUES
    (1, 1.2300, ARRAY[1, 2], int4range(1, 10), 'ok', '12:00+05:30',
     '1 year 2 days 00:00:03', 'hello'),
    (2, 'NaN', ARRAY[NULL, 3], 'empty', 'sad', '00:00-14', '-1 days', NULL),
    (3, 0.0, '[0:1]={7,8}', '[1,5]', 'happy', NULL, '0', ''),
    (4, -5, '{}', NULL, NULL, '23:59:59.5+00', NULL, repeat('toast me ', 5000));
CREATE INDEX algo_t_expr ON algo_t ((lower(t)), n);

SELECT a.algo,
       encode(pg_table_logical_checksum('algo_t'::regclass, a.algo), 'hex') AS table_logical,
       encode(pg_index_logical_checksum('algo_t_expr'::regclass, a.algo), 'hex') AS index_logical,
       encode(pg_tuple_logical_checksum('algo_t'::regclass, '(0,1)', a.algo), 'hex') AS tuple_logical,
       encode(pg_cell_checksum('algo_t'::regclass, '(0,1)', 6, a.algo), 'hex') AS cell_timetz
FROM (VALUES ('crc32c'), ('xxh3')) AS a(algo);

-- Sequences: definition and state, hashed as one canonical buffer.
CREATE SEQUENCE algo_seq AS bigint START 5 INCREMENT 3 MAXVALUE 1000 CYCLE;
SELECT nextval('algo_seq'), nextval('algo_seq');
SELECT a.algo,
       encode(pg_sequence_logical_checksum('algo_seq'::regclass, a.algo), 'hex') AS sequence_logical
FROM (VALUES ('crc32c'), ('xxh3')) AS a(algo);
SELECT setval('algo_seq', 11, false);
SELECT a.algo,
       encode(pg_sequence_logical_checksum('algo_seq'::regclass, a.algo), 'hex') AS sequence_not_called
FROM (VALUES ('crc32c'), ('xxh3')) AS a(algo);
DROP SEQUENCE algo_seq;

-- Each algorithm gives its own values; their widths are 4 and 8 bytes.
SELECT count(DISTINCT pg_table_logical_checksum('algo_t'::regclass, a.algo)) AS distinct_values,
       string_agg(a.algo || '=' || length(pg_table_logical_checksum('algo_t'::regclass, a.algo)),
                  ', ' ORDER BY a.algo) AS lengths
FROM (VALUES ('fnv32'), ('fnv64'), ('crc32c'), ('xxh3')) AS a(algo);

-- Physical checksums depend on the cluster; check their shape and that they
-- follow the data.
SELECT a.algo,
       length(pg_table_physical_checksum('algo_t'::regclass, false, a.algo)) AS table_len,
       length(pg_table_physical_checksum('algo_t'::regclass, true, a.algo)) AS table_header_len,
       length(pg_index_physical_checksum('algo_t_pkey'::regclass, a.algo)) AS index_len,
       length(pg_tuple_physical_checksum('algo_t'::regclass, '(0,1)', false, a.algo)) AS tuple_len,
       length(pg_tuple_physical_checksum('algo_t'::regclass, '(0,1)', true, a.algo)) AS tuple_header_len
FROM (VALUES ('crc32c'), ('xxh3')) AS a(algo);

CREATE TEMP TABLE algo_before AS
SELECT a.algo,
       pg_table_physical_checksum('algo_t'::regclass, false, a.algo) AS tp,
       pg_index_physical_checksum('algo_t_pkey'::regclass, a.algo) AS ip,
       pg_table_logical_checksum('algo_t'::regclass, a.algo) AS tl
FROM (VALUES ('crc32c'), ('xxh3')) AS a(algo);
UPDATE algo_t SET t = 'changed' WHERE id = 3;
SELECT b.algo,
       pg_table_physical_checksum('algo_t'::regclass, false, b.algo) <> b.tp AS table_physical_changed,
       pg_table_logical_checksum('algo_t'::regclass, b.algo) <> b.tl AS table_logical_changed
FROM algo_before AS b ORDER BY b.algo;
UPDATE algo_t SET t = '' WHERE id = 3;
SELECT b.algo,
       pg_table_logical_checksum('algo_t'::regclass, b.algo) = b.tl AS table_logical_restored
FROM algo_before AS b ORDER BY b.algo;

-- Logical checksums do not depend on partitioning.
CREATE TABLE algo_plain (id integer, v text);
CREATE TABLE algo_part (id integer, v text) PARTITION BY RANGE (id);
CREATE TABLE algo_part_1 PARTITION OF algo_part FOR VALUES FROM (0) TO (100);
CREATE TABLE algo_part_2 PARTITION OF algo_part FOR VALUES FROM (100) TO (200);
INSERT INTO algo_plain SELECT i, 'p' || i FROM generate_series(1, 199) AS i;
INSERT INTO algo_part SELECT i, 'p' || i FROM generate_series(199, 1, -1) AS i;
CREATE INDEX algo_part_idx ON algo_part (id);
SELECT a.algo,
       pg_table_logical_checksum('algo_plain'::regclass, a.algo) =
       pg_table_logical_checksum('algo_part'::regclass, a.algo) AS partition_independent,
       length(pg_index_physical_checksum('algo_part_idx'::regclass, a.algo)) AS part_index_len
FROM (VALUES ('crc32c'), ('xxh3')) AS a(algo);

-- One bucket holds every row: its checksum is the table checksum.
SELECT a.algo,
       (SELECT checksum FROM pg_table_bucket_checksums('algo_t', 1, NULL, a.algo)) =
       pg_table_logical_checksum('algo_t'::regclass, a.algo) AS one_bucket_is_table
FROM (VALUES ('crc32c'), ('xxh3')) AS a(algo);

-- Serial and parallel scans agree, at every level.
CREATE TABLE algo_big AS
SELECT i AS id, repeat('x', 200) AS body FROM generate_series(1, 20000) AS i;
CREATE INDEX algo_big_idx ON algo_big (id);
CREATE INDEX algo_big_expr ON algo_big ((id % 7), body);

SET pg_integrity_checksums.workers = 0;
CREATE TEMP TABLE algo_serial AS
SELECT a.algo,
       pg_table_physical_checksum('algo_big'::regclass, false, a.algo) AS tp,
       pg_table_logical_checksum('algo_big'::regclass, a.algo) AS tl,
       pg_index_physical_checksum('algo_big_idx'::regclass, a.algo) AS ip,
       pg_index_logical_checksum('algo_big_expr'::regclass, a.algo) AS il,
       pg_database_physical_checksum(false, false, a.algo) AS dp,
       pg_database_logical_checksum(false, true, a.algo) AS dl
FROM (VALUES ('crc32c'), ('xxh3')) AS a(algo);
SET pg_integrity_checksums.workers = 2;
SELECT s.algo,
       pg_table_physical_checksum('algo_big'::regclass, false, s.algo) = s.tp AS table_physical,
       pg_table_logical_checksum('algo_big'::regclass, s.algo) = s.tl AS table_logical,
       pg_index_physical_checksum('algo_big_idx'::regclass, s.algo) = s.ip AS index_physical,
       pg_index_logical_checksum('algo_big_expr'::regclass, s.algo) = s.il AS index_logical,
       pg_database_physical_checksum(false, false, s.algo) = s.dp AS database_physical,
       pg_database_logical_checksum(false, true, s.algo) = s.dl AS database_logical
FROM algo_serial AS s ORDER BY s.algo;
SET pg_integrity_checksums.workers = 0;

-- The per-relation database functions report what the relation functions
-- return.
SELECT a.algo,
       bool_and(d.checksum = CASE WHEN d.relkind = 'i'
                                  THEN pg_index_logical_checksum(d.relid, a.algo)
                                  ELSE pg_table_logical_checksum(d.relid, a.algo) END)
         AS logical_rows_match,
       count(*) > 0 AS has_rows
FROM (VALUES ('crc32c'), ('xxh3')) AS a(algo),
     pg_database_logical_checksums(algorithm => a.algo) AS d
WHERE d.status = 'ok' AND d.schema_name = 'public'
GROUP BY a.algo ORDER BY a.algo;
SELECT a.algo,
       bool_and(d.checksum = CASE WHEN d.relkind = 'i'
                                  THEN pg_index_physical_checksum(d.relid, a.algo)
                                  ELSE pg_table_physical_checksum(d.relid, false, a.algo) END)
         AS physical_rows_match
FROM (VALUES ('crc32c'), ('xxh3')) AS a(algo),
     pg_database_physical_checksums(algorithm => a.algo) AS d
WHERE d.status = 'ok' AND d.schema_name = 'public'
GROUP BY a.algo ORDER BY a.algo;

DROP TABLE algo_t, algo_plain, algo_part, algo_big;
DROP TYPE algo_mood;
