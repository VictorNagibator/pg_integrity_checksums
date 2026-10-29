# Property tests for canonical logical encodings.
use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('property');
$node->init;
$node->start;

$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_integrity_checksums;
    CREATE TYPE prop_mood AS ENUM ('sad', 'ok', 'happy');
    CREATE TYPE prop_pair AS (a integer, b text);
});

# The same numeric value with different display scales must hash identically.
$node->safe_psql('postgres', q{
    CREATE TABLE prop_num_a (v numeric);
    CREATE TABLE prop_num_b (v numeric);
    INSERT INTO prop_num_a
        SELECT round(((g * 37) % 10000)::numeric / 100, 2)
        FROM generate_series(1, 100) g;
    INSERT INTO prop_num_b
        SELECT (round(((g * 37) % 10000)::numeric / 100, 2))::numeric(12,4)
        FROM generate_series(1, 100) g;
});
is(
    $node->safe_psql('postgres',
        "SELECT pg_table_logical_checksum('prop_num_a'::regclass) = " .
        "pg_table_logical_checksum('prop_num_b'::regclass)"),
    't',
    'numeric scale is canonical');

# Array literal forms with the same dimensions and elements.
$node->safe_psql('postgres', q{
    CREATE TABLE prop_arr_a (v integer[]);
    CREATE TABLE prop_arr_b (v integer[]);
    INSERT INTO prop_arr_a
        SELECT ARRAY[g, g + 1, NULL] FROM generate_series(1, 50) g;
    INSERT INTO prop_arr_b
        SELECT ('{' || g || ',' || (g + 1) || ',NULL}')::integer[]
        FROM generate_series(1, 50) g;
});
is(
    $node->safe_psql('postgres',
        "SELECT pg_table_logical_checksum('prop_arr_a'::regclass) = " .
        "pg_table_logical_checksum('prop_arr_b'::regclass)"),
    't',
    'array literals are canonical');

# Range constructor and range literal must hash identically.
$node->safe_psql('postgres', q{
    CREATE TABLE prop_range_a (v int4range);
    CREATE TABLE prop_range_b (v int4range);
    INSERT INTO prop_range_a SELECT int4range(g, g + 10) FROM generate_series(1, 50) g;
    INSERT INTO prop_range_b
        SELECT ('[' || g || ',' || (g + 10) || ')')::int4range
        FROM generate_series(1, 50) g;
});
is(
    $node->safe_psql('postgres',
        "SELECT pg_table_logical_checksum('prop_range_a'::regclass) = " .
        "pg_table_logical_checksum('prop_range_b'::regclass)"),
    't',
    'range literals are canonical');

# Composite constructor and composite literal forms.
$node->safe_psql('postgres', q{
    CREATE TABLE prop_comp_a (v prop_pair);
    CREATE TABLE prop_comp_b (v prop_pair);
    INSERT INTO prop_comp_a
        SELECT ROW(g, 'value_' || g)::prop_pair FROM generate_series(1, 50) g;
    INSERT INTO prop_comp_b
        SELECT ('(' || g || ',value_' || g || ')')::prop_pair
        FROM generate_series(1, 50) g;
});
is(
    $node->safe_psql('postgres',
        "SELECT pg_table_logical_checksum('prop_comp_a'::regclass) = " .
        "pg_table_logical_checksum('prop_comp_b'::regclass)"),
    't',
    'composite literals are canonical');

# Enum values are canonical within the same enum type.
$node->safe_psql('postgres', q{
    CREATE TABLE prop_enum_a (v prop_mood);
    CREATE TABLE prop_enum_b (v prop_mood);
    INSERT INTO prop_enum_a SELECT 'ok'::prop_mood FROM generate_series(1, 20);
    INSERT INTO prop_enum_b SELECT 'ok'::prop_mood FROM generate_series(1, 20);
});
is(
    $node->safe_psql('postgres',
        "SELECT pg_table_logical_checksum('prop_enum_a'::regclass) = " .
        "pg_table_logical_checksum('prop_enum_b'::regclass)"),
    't',
    'enum values are canonical');

# Random values across every supported in-tree representation must not crash,
# and repeated logical scans must be stable.
$node->safe_psql('postgres', q{
    CREATE TABLE prop_fuzz (
        id integer,
        n numeric,
        a integer[],
        r int4range,
        p prop_pair,
        m prop_mood,
        t text
    );
    INSERT INTO prop_fuzz
    SELECT g,
           CASE WHEN g % 17 = 0 THEN 'NaN'::numeric
                ELSE (random() * 1000000)::numeric(20,6) END,
           ARRAY[(random() * 100)::integer,
                 NULL,
                 (random() * 100)::integer],
           int4range(1, 1 + (random() * 1000)::integer),
           ROW((random() * 1000)::integer,
               repeat('v', (random() * 40)::integer))::prop_pair,
           (ARRAY['sad','ok','happy'])[1 + (random() * 2)::integer]::prop_mood,
           repeat('x', (random() * 200)::integer)
    FROM generate_series(1, 100) g;
    CREATE INDEX prop_fuzz_idx ON prop_fuzz (id, t);
});

my $first = $node->safe_psql('postgres',
    "SELECT pg_table_logical_checksum('prop_fuzz'::regclass)");
my $second = $node->safe_psql('postgres',
    "SELECT pg_table_logical_checksum('prop_fuzz'::regclass)");
is($first, $second, 'random table checksum is stable');
isnt($first, '', 'random table checksum is not null');

my $idx_first = $node->safe_psql('postgres',
    "SELECT pg_index_logical_checksum('prop_fuzz_idx'::regclass)");
my $idx_second = $node->safe_psql('postgres',
    "SELECT pg_index_logical_checksum('prop_fuzz_idx'::regclass)");
is($idx_first, $idx_second, 'random index checksum is stable');
isnt($idx_first, '', 'random index checksum is not null');

my $tuple = $node->safe_psql('postgres',
    "SELECT pg_tuple_logical_checksum('prop_fuzz'::regclass, ctid) " .
    "FROM prop_fuzz ORDER BY id LIMIT 1");
isnt($tuple, '', 'random tuple checksum is not null');

my $cell = $node->safe_psql('postgres',
    "SELECT pg_cell_checksum('prop_fuzz'::regclass, ctid, 2) " .
    "FROM prop_fuzz ORDER BY id LIMIT 1");
isnt($cell, '', 'random cell checksum is not null');

done_testing();
