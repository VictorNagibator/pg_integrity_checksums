# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Verify logical checksums on a streaming standby after replay.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

my $primary = PostgreSQL::Test::Cluster->new('standby_primary');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION pg_integrity_checksums;');
$primary->safe_psql('postgres',
    'CREATE TABLE standby_items (id integer PRIMARY KEY, payload text);');
$primary->safe_psql('postgres',
    "INSERT INTO standby_items SELECT g, 'payload_' || g FROM generate_series(1, 200) g;");
$primary->safe_psql('postgres',
    'CREATE INDEX standby_items_payload_idx ON standby_items (payload);');

my $backup_name = 'standby_backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('standby_replica');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->start;

my $primary_lsn = $primary->safe_psql('postgres',
    'SELECT pg_current_wal_lsn()');
$standby->poll_query_until('postgres',
    "SELECT '$primary_lsn'::pg_lsn <= pg_last_wal_replay_lsn()");

my $checksum_sql = q{
    SELECT pg_table_logical_checksum('standby_items'::regclass),
           pg_index_logical_checksum('standby_items_pkey'::regclass),
           pg_index_logical_checksum('standby_items_payload_idx'::regclass),
           pg_database_logical_checksum(false, true)
};

my $primary_checksums = $primary->safe_psql('postgres', $checksum_sql);
my $standby_checksums = $standby->safe_psql('postgres', $checksum_sql);

is($standby_checksums, $primary_checksums,
    'logical table, index and database checksums match on standby');

$standby->stop;
$primary->stop;

done_testing();
