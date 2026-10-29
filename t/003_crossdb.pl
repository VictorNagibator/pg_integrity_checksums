# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Cross-database portability tests.  The same schema and data are created in
# two databases.  Type, relation and index OIDs differ, but logical checksums
# must match; physical checksums must not.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

my $node = PostgreSQL::Test::Cluster->new('checksum_crossdb');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE DATABASE cross_a TEMPLATE template0');
$node->safe_psql('postgres', 'CREATE DATABASE cross_b TEMPLATE template0');

foreach my $db ('cross_a', 'cross_b')
{
    $node->safe_psql($db, 'CREATE EXTENSION pg_integrity_checksums;');
}

my $setup = q{
CREATE TYPE cross_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TYPE cross_pair AS (a integer, b text);
CREATE DOMAIN cross_positive AS integer CHECK (VALUE > 0);

CREATE TABLE cross_t (
    id integer,
    n numeric,
    f float8,
    txt text,
    js jsonb,
    b bytea,
    arr integer[],
    pair cross_pair,
    mood cross_mood,
    dom cross_positive,
    rng int4range,
    u uuid,
    net inet,
    bits bit(8)
);

INSERT INTO cross_t VALUES
    (1, 1.0, 'NaN', 'one', '{"a": 1}', '\x0102',
     ARRAY[1, 2, NULL], ROW(1, 'x')::cross_pair, 'ok', 5, '[1,3)',
     '11111111-1111-1111-1111-111111111111', '192.168.0.1', B'10101010'),
    (2, 1.00, '-0'::float8, 'two', '{"b": [true, null]}', '\x00',
     ARRAY[[1,2],[3,4]], ROW(2, 'y')::cross_pair, 'happy', 7, '(1,4]',
     '22222222-2222-2222-2222-222222222222', '2001:db8::1', B'00000001'),
    (3, 'NaN'::numeric, 1.5, 'three', '{}', NULL,
     ARRAY[]::integer[], NULL, 'sad', 9, 'empty',
     '33333333-3333-3333-3333-333333333333', '10.0.0.0/8', B'11111111');

CREATE TABLE cross_nopk (id integer, v text);
INSERT INTO cross_nopk VALUES (1, 'duplicate'), (1, 'duplicate');

CREATE TABLE cross_empty (id integer);

CREATE TABLE cross_toast (id integer, v text);
INSERT INTO cross_toast
SELECT gs, repeat(md5(gs::text), 100)
FROM generate_series(1, 30) gs;

CREATE TABLE cross_part (id integer, region text) PARTITION BY LIST (region);
CREATE TABLE cross_part_a PARTITION OF cross_part FOR VALUES IN ('a');
CREATE TABLE cross_part_b PARTITION OF cross_part FOR VALUES IN ('b');
INSERT INTO cross_part VALUES (1, 'a'), (2, 'b'), (3, 'a');
CREATE INDEX cross_part_idx ON cross_part (id);

CREATE MATERIALIZED VIEW cross_mv AS SELECT id, n FROM cross_t;

CREATE INDEX cross_btree ON cross_t (txt);
CREATE INDEX cross_hash ON cross_t USING hash (txt);
CREATE INDEX cross_gin ON cross_t USING gin (js);
CREATE INDEX cross_gist ON cross_t USING gist (rng);
CREATE INDEX cross_spgist ON cross_t USING spgist (txt);
CREATE INDEX cross_brin ON cross_t USING brin (id);
CREATE INDEX cross_expr ON cross_t ((id + 1));
CREATE INDEX cross_partial ON cross_t (txt) WHERE id > 1;
CREATE INDEX cross_include ON cross_t (id) INCLUDE (txt);
};

$node->safe_psql('cross_a', $setup);
$node->safe_psql('cross_b', $setup);

my $server_version = $node->safe_psql('postgres', 'SHOW server_version_num');
if ($server_version >= 140000)
{
    foreach my $db ('cross_a', 'cross_b')
    {
        $node->safe_psql($db, 'ALTER TABLE cross_t ADD COLUMN mr int4multirange');
        $node->safe_psql($db,
            "UPDATE cross_t SET mr = int4range(id, id + 2)::int4multirange");
    }
}

sub logical_pair
{
    my ($sql) = @_;
    return ($node->safe_psql('cross_a', $sql),
            $node->safe_psql('cross_b', $sql));
}

# Table logical checksums are independent of relation OIDs.
foreach my $table ('cross_t', 'cross_nopk', 'cross_empty', 'cross_toast',
                   'cross_part', 'cross_mv')
{
    my ($a, $b) = logical_pair(
        "SELECT pg_table_logical_checksum('$table'::regclass)");
    is($a, $b, "table logical checksum matches across databases: $table");
}

# Index logical checksums are independent of index OIDs.
foreach my $index ('cross_btree', 'cross_hash', 'cross_gin', 'cross_gist',
                   'cross_spgist', 'cross_brin', 'cross_expr',
                   'cross_partial', 'cross_include', 'cross_part_idx')
{
    my ($a, $b) = logical_pair(
        "SELECT pg_index_logical_checksum('$index'::regclass)");
    is($a, $b, "index logical checksum matches across databases: $index");
}

# Database logical checksums include user tables and, optionally, indexes.
foreach my $args ('false, false', 'false, true')
{
    my ($a, $b) = logical_pair(
        "SELECT pg_database_logical_checksum($args)");
    is($a, $b, "database logical checksum matches across databases ($args)");
}

# Tuple and cell logical checksums are independent of physical location.
my ($tuple_a, $tuple_b) = logical_pair(
    "SELECT pg_tuple_logical_checksum('cross_t'::regclass, ctid)
       FROM cross_t WHERE id = 1");
is($tuple_a, $tuple_b, 'tuple logical checksum matches across databases');

my ($cell_a, $cell_b) = logical_pair(
    "SELECT pg_cell_checksum('cross_t'::regclass, ctid, 4)
       FROM cross_t WHERE id = 1");
is($cell_a, $cell_b, 'cell logical checksum matches across databases');

# Physical checksums include relation/storage identity and must differ.
my $phys_table_a = $node->safe_psql('cross_a',
    "SELECT pg_table_physical_checksum('cross_t'::regclass, false)");
my $phys_table_b = $node->safe_psql('cross_b',
    "SELECT pg_table_physical_checksum('cross_t'::regclass, false)");
isnt($phys_table_a, $phys_table_b,
     'table physical checksum differs across databases');

my $phys_db_a = $node->safe_psql('cross_a',
    'SELECT pg_database_physical_checksum(false, false)');
my $phys_db_b = $node->safe_psql('cross_b',
    'SELECT pg_database_physical_checksum(false, false)');
isnt($phys_db_a, $phys_db_b,
     'database physical checksum differs across databases');

# System-catalog logical checksums must also be available.  They are not
# compared across databases because catalog rows contain database-local OIDs.
foreach my $db ('cross_a', 'cross_b')
{
    my $catalog = $node->safe_psql($db,
        "SELECT pg_table_logical_checksum('pg_statistic'::regclass) IS NOT NULL");
    is($catalog, 't', "system catalog logical checksum works in $db");

    my $with_system = $node->safe_psql($db,
        'SELECT pg_database_logical_checksum(true, true) IS NOT NULL');
    is($with_system, 't', "database logical checksum with system catalogs works in $db");
}

$node->safe_psql('postgres', 'DROP DATABASE cross_a');
$node->safe_psql('postgres', 'DROP DATABASE cross_b');
$node->stop;

done_testing();
