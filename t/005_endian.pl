# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Fixed logical checksums for values whose canonical encoding contains
# multirange counts.  The same values must be produced on little-endian and
# big-endian systems.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

my $node = PostgreSQL::Test::Cluster->new('checksum_endian');
$node->init;
$node->start;

my $server_version = $node->safe_psql('postgres', 'SHOW server_version_num');
if ($server_version < 140000)
{
    $node->stop;
    plan skip_all => 'multiranges require PostgreSQL 14 or newer';
}

$node->safe_psql('postgres',
    'CREATE EXTENSION pg_integrity_checksums');

$node->safe_psql('postgres', q{
CREATE TABLE endian_mr (id integer, a integer[], m int4multirange);
INSERT INTO endian_mr VALUES
    (1, ARRAY[1, 2], int4range(1, 3)::int4multirange),
    (2, '[0:2]={4,NULL,6}', '{[10,20),[30,40)}'::int4multirange);
});

is($node->safe_psql('postgres',
       "SELECT pg_table_logical_checksum('endian_mr'::regclass)"),
   '2795709843', 'multirange table logical checksum is endian-stable');

is($node->safe_psql('postgres',
       "SELECT pg_tuple_logical_checksum('endian_mr'::regclass, '(0,1)'::tid)"),
   '2120354645', 'multirange tuple logical checksum is endian-stable');

is($node->safe_psql('postgres',
       "SELECT pg_cell_checksum('endian_mr'::regclass, '(0,1)'::tid, 2)"),
   '-1952248318', 'multirange cell logical checksum is endian-stable');

$node->stop;

done_testing();
