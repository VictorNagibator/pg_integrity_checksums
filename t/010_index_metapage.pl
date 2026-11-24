# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Physical index checksums mask the LP_DEAD hints index scans set on line
# pointers.  Only pages that hold index tuples have line pointers: a B-tree
# metapage keeps its metadata between the page header and pd_lower, where the
# masking used to read it as line pointers and clear "LP_DEAD" bits in it, so
# two different metapages could give the same checksum.
#
# The space between pd_lower and pd_upper is cleared before hashing, because
# it is unused on most pages.  A BRIN range map page keeps its array of TIDs
# there, and so does every metapage written before PostgreSQL 11 (which did
# not set pd_lower; pg_upgrade keeps them): clearing it hid every change, and
# every corruption, of that data from the physical checksum.

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

my $blcksz = $node->safe_psql('postgres', "SELECT current_setting('block_size')");

# Write raw bytes into a relation file and return the index's checksums.
sub write_and_checksum
{
    my ($file, $offset, $bytes, $index) = @_;
    $node->stop;
    open(my $fh, '+<', $file) or die "open $file: $!";
    binmode $fh;
    sysseek($fh, $offset, 0) or die "seek: $!";
    syswrite($fh, $bytes) == length($bytes) or die "write: $!";
    close($fh);
    $node->start;
    return $node->safe_psql('postgres',
        qq{SELECT pg_index_physical_checksum('$index') || ' ' ||
                  encode(pg_index_physical_checksum('$index', 'fnv64'), 'hex')});
}

# A metapage whose pd_lower was never set, as PostgreSQL 10 and older wrote
# it: its metadata lies in the "unused" space.
write_and_checksum($path, 12, pack('S', 24), 'meta_t_idx');
my $old_meta_a = set_meta_word(0x00000001);
my $old_meta_b = set_meta_word(0x00000002);
isnt($old_meta_a, $old_meta_b,
    'metadata of a metapage without pd_lower is part of the checksum');

# Block 1 of a BRIN index is its range map: one TID per range of heap pages,
# right after the page header, with pd_lower left at the header size.
$node->safe_psql('postgres', q{
    CREATE INDEX meta_t_brin ON meta_t USING brin (id)
        WITH (pages_per_range = 1);
});
my $brin_path = $node->data_dir . '/'
  . $node->safe_psql('postgres', "SELECT pg_relation_filepath('meta_t_brin')");
my $revmap_tid = $blcksz + 24;
my $revmap_ok = write_and_checksum($brin_path, $revmap_tid + 4, pack('S', 1),
    'meta_t_brin');
my $revmap_bad = write_and_checksum($brin_path, $revmap_tid + 4, pack('S', 2),
    'meta_t_brin');
isnt($revmap_ok, $revmap_bad, 'a changed BRIN range map changes the checksum');
is(write_and_checksum($brin_path, $revmap_tid + 4, pack('S', 1), 'meta_t_brin'),
    $revmap_ok, 'the restored BRIN range map gives the original checksum');

$node->stop;

done_testing();
