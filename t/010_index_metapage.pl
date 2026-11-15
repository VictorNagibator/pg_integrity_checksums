# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Physical index checksums mask the LP_DEAD hints index scans set on line
# pointers.  Only pages that hold index tuples have line pointers: a B-tree
# metapage keeps its metadata between the page header and pd_lower, where the
# masking used to read it as line pointers and clear "LP_DEAD" bits in it, so
# two different metapages could give the same checksum.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

my $node = PostgreSQL::Test::Cluster->new('index_metapage');
my $pg_config_version = qx{pg_config --version};
my ($pg_major) = $pg_config_version =~ /(\d+)/;
my @extra = ();
# The test writes to the data file directly; no page checksum to fix up.
push @extra, '--no-data-checksums' if defined($pg_major) && $pg_major >= 18;
$node->init(extra => \@extra);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_integrity_checksums;');
$node->safe_psql('postgres', q{
    CREATE TABLE meta_t (id integer) WITH (autovacuum_enabled = off);
    INSERT INTO meta_t SELECT generate_series(1, 10);
    CREATE INDEX meta_t_idx ON meta_t (id);
});
my $path = $node->data_dir . '/'
  . $node->safe_psql('postgres', "SELECT pg_relation_filepath('meta_t_idx')");

# Write a 32-bit value at offset 36 of block 0 (btm_level of the metapage,
# which pd_lower covers).  Bits 15-16 are where a line pointer keeps its
# flags on both little- and big-endian machines, and bit 0 and bit 17 make
# its length non-zero on both.
sub set_meta_word
{
    my ($value) = @_;
    $node->stop;
    open(my $fh, '+<', $path) or die "open $path: $!";
    binmode $fh;
    sysseek($fh, 36, 0) or die "seek: $!";
    syswrite($fh, pack('L', $value)) == 4 or die "write: $!";
    close($fh);
    $node->start;
    return $node->safe_psql('postgres',
        q{SELECT pg_index_physical_checksum('meta_t_idx') || ' ' ||
                 encode(pg_index_physical_checksum('meta_t_idx', 'fnv64'), 'hex')});
}

my $looks_dead = set_meta_word(0x00038001);      # flags = LP_DEAD
my $looks_normal = set_meta_word(0x00028001);    # flags = LP_NORMAL

isnt($looks_dead, $looks_normal,
    'metapage bits that look like LP_DEAD are not masked');

$node->stop;

done_testing();
