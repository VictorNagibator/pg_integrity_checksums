# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Database-level checksums are strict by default: a relation that cannot be
# checksummed (here a table with a corrupt page, and an unlogged table on a
# standby) aborts the call instead of silently dropping out of the result.
# With pg_integrity_checksums.strict = off the relation is skipped with a
# warning.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

# ---------------------------------------------------------------------------
# A corrupt heap page
# ---------------------------------------------------------------------------
my $node = PostgreSQL::Test::Cluster->new('strict_corrupt');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_integrity_checksums;');
$node->safe_psql('postgres', q{
    CREATE TABLE healthy_t AS SELECT g AS id FROM generate_series(1, 500) g;
    CREATE TABLE corrupt_t AS SELECT g AS id FROM generate_series(1, 500) g;
});

my $relpath = $node->safe_psql('postgres',
    "SELECT pg_relation_filepath('corrupt_t')");
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop;

# Overwrite pd_lower, pd_upper, pd_special and the page size/version of block 0
# with garbage so the page header is no longer sane.
my $file = $node->data_dir . '/' . $relpath;
open(my $fh, '+<', $file) or die "could not open $file: $!";
binmode($fh);
seek($fh, 12, 0) or die "seek failed: $!";
print $fh ("\xff" x 8);
close($fh);

$node->start;

my @calls = (
    [ 'logical 32-bit',  'pg_database_logical_checksum(false, true)' ],
    [ 'physical 32-bit', 'pg_database_physical_checksum(false, false)' ],
    [ 'logical 64-bit',  q{pg_database_logical_checksum(false, true, 'fnv64')} ],
    [ 'physical 64-bit', q{pg_database_physical_checksum(false, false, 'fnv64')} ],
);

for my $call (@calls)
{
    my ($label, $expr) = @$call;
    my ($stdout, $stderr);

    # Default: strict, the corrupt table makes the call fail.
    my $ret = $node->psql('postgres', "SELECT $expr",
        stdout => \$stdout, stderr => \$stderr);
    isnt($ret, 0, "$label: strict by default, corrupt page fails the call");
    like($stderr, qr/corrupt|invalid page|cannot be checksummed/,
        "$label: the error names the problem");
    like($stderr,
        qr/while computing the database checksum of relation "corrupt_t"/,
        "$label: the error names the relation");

    # Strict mode off: the table is skipped with a warning.
    $stdout = $stderr = '';
    $ret = $node->psql('postgres',
        "SET pg_integrity_checksums.strict = off; SELECT $expr",
        stdout => \$stdout, stderr => \$stderr);
    is($ret, 0, "$label: strict = off completes");
    like($stderr, qr/skipping relation \d+ while computing database checksum/,
        "$label: strict = off warns about the skipped relation");
    like($stderr, qr/database checksum skipped one or more relations/,
        "$label: strict = off emits the summary warning");
}

# The non-strict result is the checksum of everything except the corrupt table.
my $skipped = $node->safe_psql('postgres',
    "SET pg_integrity_checksums.strict = off; "
      . "SET client_min_messages = error; "
      . "SELECT pg_database_logical_checksum(false, true)");
$node->safe_psql('postgres', 'DROP TABLE corrupt_t');
my $without = $node->safe_psql('postgres',
    'SELECT pg_database_logical_checksum(false, true)');
is($skipped, $without,
    'non-strict result equals the checksum without the corrupt table');

# The setting can be made persistent for a role or database.
$node->safe_psql('postgres',
    'ALTER DATABASE postgres SET pg_integrity_checksums.strict = off');
is($node->safe_psql('postgres', 'SHOW pg_integrity_checksums.strict'),
    'off', 'ALTER DATABASE ... SET changes the default for new sessions');

$node->stop;

# ---------------------------------------------------------------------------
# An unlogged table on a standby
# ---------------------------------------------------------------------------
SKIP:
{
    skip 'streaming standby TAP is not supported on Windows', 5
      if $^O eq 'MSWin32' || $^O eq 'cygwin';

    my $primary = PostgreSQL::Test::Cluster->new('strict_primary');
    $primary->init(allows_streaming => 1);
    $primary->start;
    $primary->safe_psql('postgres', q{
        CREATE EXTENSION pg_integrity_checksums;
        CREATE TABLE logged_t AS SELECT g AS id FROM generate_series(1, 200) g;
        CREATE UNLOGGED TABLE unlogged_t AS
            SELECT g AS id FROM generate_series(1, 200) g;
    });

    $primary->backup('strict_backup');
    my $standby = PostgreSQL::Test::Cluster->new('strict_standby');
    $standby->init_from_backup($primary, 'strict_backup', has_streaming => 1);
    $standby->start;

    my $lsn = $primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
    $standby->poll_query_until('postgres',
        "SELECT '$lsn'::pg_lsn <= pg_last_wal_replay_lsn()");

    my ($stdout, $stderr);
    my $ret = $standby->psql('postgres',
        'SELECT pg_database_logical_checksum(false, true)',
        stdout => \$stdout, stderr => \$stderr);
    isnt($ret, 0, 'standby: strict by default, unlogged table fails the call');
    like($stderr,
        qr/while computing the database checksum of relation "unlogged_t"/,
        'standby: the error names the unlogged table');

    $stdout = $stderr = '';
    $ret = $standby->psql('postgres',
        'SET pg_integrity_checksums.strict = off; '
          . 'SELECT pg_database_logical_checksum(false, true)',
        stdout => \$stdout, stderr => \$stderr);
    is($ret, 0, 'standby: strict = off completes');
    like($stderr, qr/skipping relation \d+ while computing database checksum/,
        'standby: strict = off warns about the unlogged table');

    # Without the unlogged table, primary and standby agree again.
    $primary->safe_psql('postgres', 'DROP TABLE unlogged_t');
    $lsn = $primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
    $standby->poll_query_until('postgres',
        "SELECT '$lsn'::pg_lsn <= pg_last_wal_replay_lsn()");
    is($standby->safe_psql('postgres',
            'SELECT pg_database_logical_checksum(false, true)'),
        $primary->safe_psql('postgres',
            'SELECT pg_database_logical_checksum(false, true)'),
        'standby: strict checksum matches the primary without unlogged tables');

    $standby->stop;
    $primary->stop;
}

done_testing();
