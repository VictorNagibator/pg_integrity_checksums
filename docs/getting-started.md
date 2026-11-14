# Getting started

This guide covers the basic extension workflow: installing the extension,
computing table and index checksums, and capturing a first CLI baseline.
It assumes a working PostgreSQL 12+ server and a database where you can
create the extension.

## 1. Install and enable the extension

Install the extension with one of the methods in [Installation](installation.md),
then enable it in the target database:

~~~sql
CREATE EXTENSION pg_integrity_checksums;

SELECT extversion
FROM pg_extension
WHERE extname = 'pg_integrity_checksums';
~~~

The extension is installed per database, not once per cluster.

## 2. Compute a table checksum

Create a small table for the example:

~~~sql
CREATE TABLE demo_items (
    id    integer PRIMARY KEY,
    name  text NOT NULL
);

INSERT INTO demo_items VALUES
    (1, 'alpha'),
    (2, 'beta'),
    (3, 'gamma');
~~~

Compute a logical checksum with the default 32-bit API:

~~~sql
SELECT pg_table_logical_checksum('demo_items'::regclass);
~~~

The result is a SQL integer (`bigint` for table checksums).  Logical
checksums are stable across `VACUUM`, `REINDEX`, table rewrites and physical
row movement, and change when the logical row content changes.  They do not
require a primary key.

A physical checksum of the same table returns a `bigint`:

~~~sql
SELECT pg_table_physical_checksum('demo_items'::regclass, false);
~~~

Physical checksums depend on tuple locations, storage layout and the relation
identity.  They are intended for one cluster and change after maintenance that
rewrites storage.

## 3. Compute an index checksum

The default logical index checksum describes the entries implied by the table
data:

~~~sql
SELECT pg_index_logical_checksum('demo_items_pkey'::regclass);
~~~

A physical index checksum inspects the index storage itself:

~~~sql
SELECT pg_index_physical_checksum('demo_items_pkey'::regclass);
~~~

Use the physical form to detect missing or corrupt index pages; the logical
form describes expected content and is stable across `REINDEX`.

## 4. Optional 64-bit digests

The 32-bit API is the default and is sufficient for most integrity checks.
The generic API provides an explicit algorithm and a wider 64-bit digest when
a longer fingerprint is useful:

~~~sql
SELECT encode(
    pg_table_logical_checksum('demo_items'::regclass, 'fnv64'),
    'hex'
) AS logical_checksum_64;
~~~

Generic functions return `bytea`; `encode(..., 'hex')` is only for display.
Use `pg_checksum_bigint(...)` when a SQL `bigint` is required:

~~~sql
SELECT pg_checksum_bigint(
    pg_table_logical_checksum('demo_items'::regclass, 'fnv64')
);
~~~

| Requirement | Recommended API |
|---|---|
| Simple integer result; existing v1 baselines | Default 32-bit functions |
| Explicit algorithm or wider digest | Generic `fnv32`/`fnv64` overloads |
| Page checksum | `pg_page_checksum` (32-bit only) |

FNV-1a is not a cryptographic hash; 64-bit digests do not make it tamper
evident.  See [Concepts](concepts.md#which-api-should-i-use) for the complete
selection guidance.

## 5. Capture and verify a CLI baseline

The CLI captures the selected objects into a versioned JSON baseline.  Install
it from a release binary, the container image or with `go install`; see
[CLI installation](cli.md#installation).  The default capture uses the 32-bit
v1 format:

~~~bash
pg_integrity_checksums capture \
  --db "$DATABASE_URL" \
  --schema public \
  --output baseline.json

pg_integrity_checksums verify \
  --db "$DATABASE_URL" \
  --baseline baseline.json
~~~

To capture a 64-bit v2 baseline instead, add `--hash fnv64` to `capture`.
`verify` and `update` inherit the algorithm from the baseline they read.

A clean verify exits with code `0` and prints `no changes`.  The complete
exit-code contract is in the [CLI reference](cli.md#exit-codes).

## 6. Next steps

- [Concepts](concepts.md) — logical vs physical checksums, API selection,
  aggregation, baselines and limitations.
- [Function reference](functions.md) — signatures, return types and
  algorithm details.
- [CLI](cli.md) — filters, reports, diff, update and CI integration.
- [Operations](operations.md) — performance, permissions and production
  constraints.
