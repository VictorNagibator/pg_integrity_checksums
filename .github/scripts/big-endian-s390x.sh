#!/bin/sh
# Big-endian logical-checksum test, run inside an s390x Alpine container.
# PostgreSQL itself is installed from Alpine packages; only the extension is
# compiled, so this test does not spend tens of minutes building PostgreSQL.
set -e

PGDATA=/tmp/pgdata

rm -rf "$PGDATA"
PGBIN="$(pg_config --bindir)"
export PATH="$PGBIN:$PATH"

"$PGBIN/initdb" -D "$PGDATA" -U pg -A trust --no-sync
"$PGBIN/pg_ctl" -D "$PGDATA" -l "$PGDATA/server.log" -o '-p 55444' -w start
trap '"$PGBIN/pg_ctl" -D "$PGDATA" -m immediate stop || true' EXIT

"$PGBIN/psql" -h /tmp -p 55444 -U pg -d postgres -v ON_ERROR_STOP=1 <<'SQL'
CREATE EXTENSION pg_integrity_checksums;

CREATE TYPE endian_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TYPE endian_pair AS (a integer, b text);

CREATE TABLE endian_t (
    id integer,
    n numeric,
    a integer[],
    r int4range,
    m endian_mood,
    p endian_pair,
    t text
);

INSERT INTO endian_t VALUES
    (1, 1.2300, ARRAY[1, 2], int4range(1, 10), 'ok', ROW(1, 'x'), 'hello'),
    (2, 'NaN', ARRAY[NULL, 3], 'empty', 'sad', ROW(NULL, ''), NULL),
    (3, 0.0, '[0:1]={7,8}', '[1,5]', 'happy', ROW(2, NULL), '');

CREATE TABLE endian_mr (id integer, a integer[], m int4multirange);
INSERT INTO endian_mr VALUES
    (1, ARRAY[1, 2], int4range(1, 3)::int4multirange),
    (2, '[0:2]={4,NULL,6}', '{[10,20),[30,40)}'::int4multirange);

DO $$
DECLARE
    table_value bigint;
    int_value integer;
BEGIN
    SELECT pg_table_logical_checksum('endian_t'::regclass) INTO table_value;
    IF table_value <> 161531096 THEN
        RAISE EXCEPTION 'endian_t table checksum changed: %', table_value;
    END IF;
    SELECT pg_tuple_logical_checksum('endian_t'::regclass, '(0,1)'::tid)
      INTO int_value;
    IF int_value <> -585984793 THEN
        RAISE EXCEPTION 'endian_t tuple checksum changed: %', int_value;
    END IF;
    SELECT pg_cell_checksum('endian_t'::regclass, '(0,1)'::tid, 1)
      INTO int_value;
    IF int_value <> -46178465 THEN
        RAISE EXCEPTION 'endian_t cell checksum changed: %', int_value;
    END IF;

    SELECT pg_table_logical_checksum('endian_mr'::regclass) INTO table_value;
    IF table_value <> 2795709843 THEN
        RAISE EXCEPTION 'endian_mr table checksum changed: %', table_value;
    END IF;
    SELECT pg_tuple_logical_checksum('endian_mr'::regclass, '(0,1)'::tid)
      INTO int_value;
    IF int_value <> 2120354645 THEN
        RAISE EXCEPTION 'endian_mr tuple checksum changed: %', int_value;
    END IF;
    SELECT pg_cell_checksum('endian_mr'::regclass, '(0,1)'::tid, 2)
      INTO int_value;
    IF int_value <> -1952248318 THEN
        RAISE EXCEPTION 'endian_mr cell checksum changed: %', int_value;
    END IF;

    RAISE NOTICE 'big-endian logical checksums OK';
END
$$;

DROP TABLE endian_mr;
DROP TABLE endian_t;
DROP TYPE endian_pair;
DROP TYPE endian_mood;
SQL
