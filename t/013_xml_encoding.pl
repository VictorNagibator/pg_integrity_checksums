# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Logical checksums hash text in the server encoding, whatever the session's
# client_encoding is.  xml_send() also writes an XML declaration naming the
# client encoding, unless that is UTF8 or SQL_ASCII.  In a LATIN1 database a
# session with client_encoding LATIN1 used to call it directly and hashed
# '<?xml version="1.0" encoding="LATIN1"?>...', while every other session
# hashed the value without that declaration.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

my $node = PostgreSQL::Test::Cluster->new('xml_encoding');
$node->init;
$node->start;

my ($ret, $stdout, $stderr) =
  $node->psql('postgres', q{SELECT '<a/>'::xml});
if ($ret != 0)
{
    $node->stop;
    plan skip_all => 'the server was built without libxml';
}

$node->safe_psql('postgres', q{
    CREATE DATABASE latin1 ENCODING 'LATIN1' LC_COLLATE 'C' LC_CTYPE 'C'
        TEMPLATE template0;
});
$node->safe_psql('latin1', q{
    CREATE EXTENSION pg_integrity_checksums;
    CREATE TABLE x (id integer PRIMARY KEY, v xml);
    INSERT INTO x VALUES (1, '<?xml version="1.0"?><a>1</a>'), (2, '<b/>');
    CREATE INDEX x_v_idx ON x ((v::text));
});

my $checksums = q{
    SELECT pg_table_logical_checksum('x') || ' ' ||
           encode(pg_table_logical_checksum('x', 'fnv64'), 'hex') || ' ' ||
           pg_tuple_logical_checksum('x', '(0,1)') || ' ' ||
           pg_cell_checksum('x', '(0,1)', 2) || ' ' ||
           pg_database_logical_checksum();
};

my %result;
foreach my $encoding ('LATIN1', 'UTF8', 'SQL_ASCII')
{
    $result{$encoding} = $node->safe_psql('latin1',
        "SET client_encoding = '$encoding'; $checksums");
}

is($result{LATIN1}, $result{UTF8},
    'client_encoding equal to the server encoding does not change xml checksums');
is($result{SQL_ASCII}, $result{UTF8},
    'client_encoding SQL_ASCII does not change xml checksums');

$node->stop;

done_testing();
