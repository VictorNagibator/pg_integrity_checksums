# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Physical index and TOAST checksums must survive a server restart when
# nothing changed.  With data checksums on, PostgreSQL stores pd_checksum
# only in the copy of a page it writes out, so the value in shared buffers is
# stale until the page is evicted and read back.  Hashing raw buffer pages
# therefore gave a different physical checksum after a restart.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

my $node = PostgreSQL::Test::Cluster->new('physical_restart');
$node->init(extra => ['--data-checksums']);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_integrity_checksums;');
$node->safe_psql('postgres', q{
    CREATE TABLE restart_t (id integer PRIMARY KEY, p point, v text)
        WITH (autovacuum_enabled = off);
    ALTER TABLE restart_t ALTER COLUMN v SET STORAGE EXTERNAL;
    CREATE INDEX restart_t_hash ON restart_t USING hash (id);
    CREATE INDEX restart_t_gist ON restart_t USING gist (p);
    INSERT INTO restart_t
        SELECT g, point(g, g), repeat(md5(g::text), 100)
        FROM generate_series(1, 5000) g;
    VACUUM restart_t;
    CHECKPOINT;
    DELETE FROM restart_t WHERE id % 3 = 0;
    VACUUM restart_t;
    CHECKPOINT;
});

my $query = q{
    SELECT pg_index_physical_checksum('restart_t_pkey') || ' ' ||
           pg_index_physical_checksum('restart_t_hash') || ' ' ||
           pg_index_physical_checksum('restart_t_gist') || ' ' ||
           encode(pg_index_physical_checksum('restart_t_pkey', 'fnv64'), 'hex') || ' ' ||
           pg_table_physical_checksum(
               (SELECT reltoastrelid FROM pg_class WHERE relname = 'restart_t'))
};

my $before = $node->safe_psql('postgres', $query);

$node->restart;

my $after = $node->safe_psql('postgres', $query);
is($after, $before,
    'physical index and TOAST checksums are unchanged by a restart');

$node->safe_psql('postgres',
    "INSERT INTO restart_t VALUES (100000, point(-1, -1), 'x')");
my $changed = $node->safe_psql('postgres', $query);
isnt($changed, $before, 'a real change still changes physical checksums');

$node->stop;

done_testing();
