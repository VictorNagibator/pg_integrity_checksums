# Copyright (c) 2026, PostgreSQL Global Development Group
#
# In a SERIALIZABLE transaction a checksum is a read of the whole relation and
# must take SIREAD locks like a sequential scan does.  Otherwise a write skew
# through a checksum goes undetected: each transaction reads what the other
# one writes, and both commit.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

my $pg_config_version = qx{pg_config --version};
my ($pg_major) = $pg_config_version =~ /(\d+)/;
# PostgreSQL::Test::BackgroundPsql appeared in PostgreSQL 16.
plan skip_all => 'needs PostgreSQL 16 or later for BackgroundPsql'
  if !defined($pg_major) || $pg_major < 16;

my $node = PostgreSQL::Test::Cluster->new('serializable');
$node->init;
$node->start;
$node->safe_psql('postgres', q{
    CREATE EXTENSION pg_integrity_checksums;
    CREATE TABLE ssi_a (id integer);
    CREATE TABLE ssi_b (id integer);
    INSERT INTO ssi_a VALUES (1);
    INSERT INTO ssi_b VALUES (1);
});

# Run a write skew in which transaction 1 reads ssi_a with "$read" and
# transaction 2 reads ssi_b with a plain query; each then writes what the
# other one read (transaction 2 with "$write", by default an insert into
# ssi_a).  Return whether a serialization failure was raised.
sub write_skew_fails
{
    my ($read, $write) = @_;
    $write //= 'INSERT INTO ssi_a VALUES (2)';
    my $s1 = $node->background_psql('postgres', on_error_stop => 0);
    my $s2 = $node->background_psql('postgres', on_error_stop => 0);

    $s1->query('BEGIN ISOLATION LEVEL SERIALIZABLE');
    $s2->query('BEGIN ISOLATION LEVEL SERIALIZABLE');
    $s1->query($read);
    $s2->query('SELECT count(*) FROM ssi_b');
    $s1->query('INSERT INTO ssi_b VALUES (2)');
    $s2->query($write);
    $s1->query('COMMIT');
    $s2->query('COMMIT');

    my $errors = $s1->{stderr} . $s2->{stderr};
    $s1->quit;
    $s2->quit;
    $node->safe_psql('postgres',
        'DELETE FROM ssi_a WHERE id = 2; DELETE FROM ssi_b WHERE id = 2; VACUUM ssi_a;');
    return $errors =~ /could not serialize access/ ? 1 : 0;
}

# The plain query shows the scenario does produce a conflict.
is(write_skew_fails('SELECT count(*) FROM ssi_a'), 1,
    'write skew through a plain read fails');
is(write_skew_fails("SELECT pg_table_logical_checksum('ssi_a')"), 1,
    'write skew through a table logical checksum fails');
is(write_skew_fails("SELECT pg_table_physical_checksum('ssi_a', false)"), 1,
    'write skew through a table physical checksum fails');
# A tuple checksum reads one row: like a read by TID, it conflicts with a
# write of that row, not with an insert of another one.
is( write_skew_fails(
        "SELECT pg_tuple_logical_checksum('ssi_a', '(0,1)')",
        'UPDATE ssi_a SET id = 1 WHERE id = 1'),
    1,
    'write skew through a tuple checksum fails');

$node->stop;

done_testing();
