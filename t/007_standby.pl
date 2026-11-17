# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Verify logical checksums on a streaming standby after replay.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

# The TAP backup helper invokes pg_basebackup through the Windows shell; the
# current CI image does not provide a usable streaming backup path there.
plan skip_all => 'streaming standby TAP is not supported on Windows'
  if $^O eq 'MSWin32' || $^O eq 'cygwin';

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

# Rows live at the same TIDs on a physical standby, so block range checksums
# are comparable; leave dead tuples behind so visibility matters.
$primary->safe_psql('postgres',
    "UPDATE standby_items SET payload = payload || '_u' WHERE id % 7 = 0;");
$primary->safe_psql('postgres', 'DELETE FROM standby_items WHERE id % 11 = 0;');
$primary_lsn = $primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
$standby->poll_query_until('postgres',
    "SELECT '$primary_lsn'::pg_lsn <= pg_last_wal_replay_lsn()");

my $ranges_sql = q{
    SELECT string_agg(start_block || ':' || row_count || ':' ||
                      encode(checksum, 'hex'), ',' ORDER BY start_block)
      FROM pg_table_block_checksums('standby_items'::regclass, 1)
    UNION ALL
    SELECT string_agg(bucket || ':' || row_count || ':' ||
                      encode(checksum, 'hex'), ',' ORDER BY bucket)
      FROM pg_table_bucket_checksums('standby_items'::regclass, 16)
};

my $primary_ranges = $primary->safe_psql('postgres', $ranges_sql);
my $standby_ranges = $standby->safe_psql('postgres', $ranges_sql);

isnt($primary_ranges, '', 'block and bucket checksums are not empty');
is($standby_ranges, $primary_ranges,
    'block range and bucket checksums match on standby');

$standby->stop;
$primary->stop;

done_testing();
