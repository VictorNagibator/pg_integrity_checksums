# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A logical index checksum reads the index's table.  PostgreSQL locks a table
# before its indexes (DROP INDEX, DROP TABLE), so the checksum must do the
# same: it used to open the index first and then wait for the table, and a
# concurrent DROP waiting for the index made the deadlock detector cancel one
# of them, often the user's DROP.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;
use Time::HiRes qw(usleep);

my $pg_config_version = qx{pg_config --version};
my ($pg_major) = $pg_config_version =~ /(\d+)/;
# PostgreSQL::Test::BackgroundPsql appeared in PostgreSQL 16.
plan skip_all => 'needs PostgreSQL 16 or later for BackgroundPsql'
  if !defined($pg_major) || $pg_major < 16;

my $node = PostgreSQL::Test::Cluster->new('lock_order');
$node->init;
# Detect a deadlock quickly instead of after the default second.
$node->append_conf('postgresql.conf', 'deadlock_timeout = 100ms');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_integrity_checksums');

# Lock $lock_table in one session, start "$checksum" in another (it has to
# wait for that lock), then run "$ddl" in the first session and commit.
# Return the errors of both sessions.
sub checksum_during_ddl
{
    my ($setup, $lock_table, $checksum, $ddl) = @_;

    $node->safe_psql('postgres', $setup);

    my $s1 = $node->background_psql('postgres', on_error_stop => 0);
    my $s2 = $node->background_psql('postgres', on_error_stop => 0);

    $s1->query_safe('BEGIN');
    $s1->query_safe("LOCK TABLE $lock_table IN ACCESS EXCLUSIVE MODE");

    $s2->query_until(qr/started/, "\\echo started\n$checksum;\n");

    # Wait until the checksum waits for the lock.  Not poll_query_until():
    # under MSYS Perl on Windows its output keeps a trailing \r and never
    # equals the expected value.
    my $waiting = 0;
    foreach my $i (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
    {
        my $out = $node->safe_psql('postgres',
            'SELECT count(*) > 0 FROM pg_locks WHERE NOT granted');
        if ($out =~ /^t\s*$/)
        {
            $waiting = 1;
            last;
        }
        usleep(100_000);
    }
    die 'the checksum did not wait for the lock' unless $waiting;

    # Each query clears the stderr of the previous one: collect it as we go.
    my $errors = '';
    $s1->query($ddl);
    $errors .= $s1->{stderr};
    $s1->query('COMMIT');
    $errors .= $s1->{stderr};

    # Wait for the checksum to finish; its error, if any, arrives meanwhile.
    $s2->query('SELECT 1');
    $errors .= $s2->{stderr};

    $s1->quit;
    $s2->quit;
    return $errors;
}

my $errors = checksum_during_ddl(
    q{CREATE TABLE lo_t (a int);
      INSERT INTO lo_t SELECT generate_series(1, 100);
      CREATE INDEX lo_t_a ON lo_t (a);},
    'lo_t',
    "SELECT pg_index_logical_checksum('lo_t_a')",
    'DROP INDEX lo_t_a');
unlike($errors, qr/deadlock detected/,
    'index logical checksum does not deadlock with DROP INDEX');
is($node->safe_psql('postgres',
        "SELECT count(*) FROM pg_class WHERE relname = 'lo_t_a'"),
    '0', 'DROP INDEX committed');

# A partitioned index: its leaf indexes used to be locked before the
# partitions.  LOCK TABLE on the parent locks the partitions too.
$errors = checksum_during_ddl(
    q{CREATE TABLE lo_p (a int) PARTITION BY RANGE (a);
      CREATE TABLE lo_p1 PARTITION OF lo_p FOR VALUES FROM (0) TO (100);
      CREATE TABLE lo_p2 PARTITION OF lo_p FOR VALUES FROM (100) TO (200);
      INSERT INTO lo_p SELECT generate_series(0, 199);
      CREATE INDEX lo_p_a ON lo_p (a);},
    'lo_p',
    "SELECT pg_index_logical_checksum('lo_p_a')",
    'DROP TABLE lo_p2');
unlike($errors, qr/deadlock detected/,
    'partitioned index logical checksum does not deadlock with DROP TABLE of a partition');
is($node->safe_psql('postgres',
        "SELECT count(*) FROM pg_class WHERE relname = 'lo_p2'"),
    '0', 'DROP TABLE of the partition committed');

# A database scan computes an index on its own (not in one pass with its
# table) when it is the only relation asked for.
$errors = checksum_during_ddl(
    q{CREATE TABLE lo_d (a int);
      INSERT INTO lo_d SELECT generate_series(1, 100);
      CREATE INDEX lo_d_a ON lo_d (a);},
    'lo_d',
    "SELECT * FROM pg_database_logical_checksums(false, true, ARRAY['lo_d_a'::regclass])",
    'DROP INDEX lo_d_a');
unlike($errors, qr/deadlock detected/,
    'database logical checksum of an index does not deadlock with DROP INDEX');
is($node->safe_psql('postgres',
        "SELECT count(*) FROM pg_class WHERE relname = 'lo_d_a'"),
    '0', 'DROP INDEX committed during the database scan');

$node->stop;

done_testing();
