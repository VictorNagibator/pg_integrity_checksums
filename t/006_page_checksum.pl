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
$node_on->safe_psql('postgres',
    "CREATE TABLE rel_status AS SELECT g AS id, repeat('r', 200) AS payload FROM generate_series(1, 2000) g;"
);
# VACUUM creates the free space map and visibility map forks.
$node_on->safe_psql('postgres', 'VACUUM rel_status;');
$node_on->safe_psql('postgres', 'CHECKPOINT;');

check_status($node_on, 'ok');

is( $node_on->safe_psql(
        'postgres',
        "SELECT count(*) FROM pg_relation_page_checksum_status('rel_status')"),
    '0',
    'intact relation has no mismatching pages');
is( $node_on->safe_psql(
        'postgres',
        "SELECT string_agg(DISTINCT fork || ':' || status, ',' ORDER BY fork || ':' || status)
         FROM pg_relation_page_checksum_status('rel_status', include_ok => true)"),
    'fsm:ok,main:ok,vm:ok',
    'every fork of an intact relation verifies');

my $value = $node_on->safe_psql('postgres',
    "SELECT pg_page_checksum('page_status'::regclass, 0)");
cmp_ok($value, '>=', 0, 'page checksum is non-negative');
cmp_ok($value, '<=', 65535, 'page checksum is 16-bit');

# A corrupt page must still be reported as a mismatch.  The status function
# reads the block again when the checksum does not match (a read can overlap
# with a concurrent write); real corruption fails every read.
my $relpath = $node_on->safe_psql('postgres',
    "SELECT pg_relation_filepath('page_status')");
my $rel_relpath = $node_on->safe_psql('postgres',
    "SELECT pg_relation_filepath('rel_status')");
$node_on->stop;

my $file = $node_on->data_dir . '/' . $relpath;
open(my $fh, '+<', $file) or die "open $file: $!";
binmode $fh;
# Overwrite bytes in the middle of block 0, away from the page header and the
# line pointers; the page stays structurally valid.
sysseek($fh, 4000, 0) or die "seek: $!";
syswrite($fh, "\xde\xad\xbe\xef" x 4) == 16 or die "write: $!";
close($fh);

# Corrupt block 2 of the main fork and block 0 of the visibility map of a
# second relation; the relation-level check must name exactly those blocks.
foreach my $target ([ '', 2 * 8192 + 4000 ], [ '_vm', 4000 ])
{
    my ($suffix, $offset) = @$target;
    my $target_file = $node_on->data_dir . '/' . $rel_relpath . $suffix;
    open(my $rfh, '+<', $target_file) or die "open $target_file: $!";
    binmode $rfh;
    sysseek($rfh, $offset, 0) or die "seek: $!";
    syswrite($rfh, "\xde\xad\xbe\xef" x 4) == 16 or die "write: $!";
    close($rfh);
}

$node_on->start;
check_status($node_on, 'mismatch');
is( $node_on->safe_psql(
        'postgres',
        "SELECT string_agg(fork || ':' || blkno || ':' || status, ',' ORDER BY fork, blkno)
         FROM pg_relation_page_checksum_status('rel_status')"),
    'main:2:mismatch,vm:0:mismatch',
    'relation check reports the corrupt blocks of every fork');
is( $node_on->safe_psql(
        'postgres',
        "SELECT string_agg(fork || ':' || blkno, ',')
         FROM pg_relation_page_checksum_status('rel_status', 'main')"),
    'main:2',
    'fork filter limits the check to one fork');
is( $node_on->safe_psql(
        'postgres',
        "SELECT stored_checksum <> calculated_checksum
         FROM pg_relation_page_checksum_status('rel_status', 'main')"),
    't',
    'mismatch row carries both checksums');
$node_on->stop;

# A corrupt header with pd_upper = 0 is not a new page: only an all-zero
# block is.  PageIsNew() alone looks at pd_upper and reported "new".
open($fh, '+<', $file) or die "open $file: $!";
binmode $fh;
sysseek($fh, 14, 0) or die "seek: $!";
syswrite($fh, "\0\0") == 2 or die "write: $!";
close($fh);

$node_on->start;
check_status($node_on, 'mismatch');
$node_on->stop;

my $node_off = PostgreSQL::Test::Cluster->new('page_checksum_off');
my $pg_config_version = qx{pg_config --version};
my ($pg_major) = $pg_config_version =~ /(\d+)/;
my @off_extra = ();
push @off_extra, '--no-data-checksums' if defined($pg_major) && $pg_major >= 18;
$node_off->init(extra => \@off_extra);
$node_off->start;
$node_off->safe_psql('postgres', 'CREATE EXTENSION pg_integrity_checksums;');
$node_off->safe_psql('postgres',
    'CREATE TABLE page_status (id integer PRIMARY KEY);');
$node_off->safe_psql('postgres', 'INSERT INTO page_status VALUES (1);');

check_status($node_off, 'checksums_disabled');

my ($ret, $stdout, $stderr) = $node_off->psql('postgres',
    "SELECT * FROM pg_relation_page_checksum_status('page_status')");
isnt($ret, 0, 'relation check fails without data checksums');
like(
    $stderr,
    qr/data checksums are not enabled in this cluster/,
    'relation check explains that data checksums are disabled');
$node_off->stop;

done_testing();
