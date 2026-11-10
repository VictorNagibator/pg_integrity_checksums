# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Verify pg_page_checksum_status() with data checksums enabled and disabled.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

sub check_status
{
    my ($node, $expected) = @_;

    my $status = $node->safe_psql('postgres',
        "SELECT pg_page_checksum_status('page_status'::regclass, 0)");
    is($status, $expected, "page checksum status is $expected");
}

my $node_on = PostgreSQL::Test::Cluster->new('page_checksum_on');
$node_on->init(extra => ['--data-checksums']);
$node_on->start;
$node_on->safe_psql('postgres', 'CREATE EXTENSION pg_integrity_checksums;');
$node_on->safe_psql('postgres',
    'CREATE TABLE page_status (id integer PRIMARY KEY);');
$node_on->safe_psql('postgres', 'INSERT INTO page_status VALUES (1), (2);');
$node_on->safe_psql('postgres', 'CHECKPOINT;');

check_status($node_on, 'ok');

my $value = $node_on->safe_psql('postgres',
    "SELECT pg_page_checksum('page_status'::regclass, 0)");
cmp_ok($value, '>=', 0, 'page checksum is non-negative');
cmp_ok($value, '<=', 65535, 'page checksum is 16-bit');

$node_on->stop;

my $node_off = PostgreSQL::Test::Cluster->new('page_checksum_off');
my @off_extra = ();
push @off_extra, '--no-data-checksums' if $node_off->pg_version->major >= 18;
$node_off->init(extra => \@off_extra);
$node_off->start;
$node_off->safe_psql('postgres', 'CREATE EXTENSION pg_integrity_checksums;');
$node_off->safe_psql('postgres',
    'CREATE TABLE page_status (id integer PRIMARY KEY);');
$node_off->safe_psql('postgres', 'INSERT INTO page_status VALUES (1);');

check_status($node_off, 'checksums_disabled');
$node_off->stop;

done_testing();
