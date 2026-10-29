-- Fixed logical baselines for the same schema/data on PostgreSQL 12-19.
-- If a canonical encoding change is intentional, regenerate these values and
-- update expected/checksum_crossversion.out.

CREATE TYPE xv_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TYPE xv_pair AS (a integer, b text);

CREATE TABLE xv_t (
    id integer,
    n numeric,
    a integer[],
    r int4range,
    m xv_mood,
    p xv_pair,
    t text
);

INSERT INTO xv_t VALUES
    (1, 1.2300, ARRAY[1, 2], int4range(1, 10), 'ok', ROW(1, 'x'), 'hello'),
    (2, 'NaN', ARRAY[NULL, 3], 'empty', 'sad', ROW(NULL, ''), NULL),
    (3, 0.0, '[0:1]={7,8}', '[1,5]', 'happy', ROW(2, NULL), '');

CREATE INDEX xv_idx ON xv_t (id, t);

SELECT pg_table_logical_checksum('xv_t'::regclass) = 161531096
       AS crossversion_table;
SELECT pg_index_logical_checksum('xv_idx'::regclass) = -1867570346
       AS crossversion_index;
SELECT pg_tuple_logical_checksum('xv_t'::regclass, '(0,1)'::tid) = -585984793
       AS crossversion_tuple;
SELECT pg_cell_checksum('xv_t'::regclass, '(0,1)'::tid, 1) = -46178465
       AS crossversion_cell;

DROP TABLE xv_t;
DROP TYPE xv_pair;
DROP TYPE xv_mood;
