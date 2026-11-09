# Getting started

This is the shortest path from a clean PostgreSQL installation to a useful
checksum.  It takes about five minutes.

If you only remember one rule: **logical checksums answer “is the data the
same?”; physical checksums answer “is this exact cluster’s storage the same?”**

## 1. Install the extension

Choose one of the methods in [Installation](installation.md).  For a local
PostgreSQL development server:

~~~bash
git clone https://github.com/VictorNagibator/pg_integrity_checksums.git
cd pg_integrity_checksums
make
sudo make install
~~~

Then create the extension in the database where you want to use it:

~~~sql
CREATE EXTENSION pg_integrity_checksums;

SELECT extversion
FROM pg_extension
WHERE extname = 'pg_integrity_checksums';
~~~

The extension is installed **per database**, not once per cluster.

## 2. Checksum a table

Create a small table:

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

Compute a 64-bit logical checksum:

~~~sql
SELECT encode(
    pg_table_logical_checksum('demo_items'::regclass, 'fnv64'),
    'hex'
) AS logical_checksum;
~~~

A digest is raw bytes.  The `encode(..., 'hex')` is only for display; store or
compare the `bytea` value itself.

If you need a SQL number instead:

~~~sql
SELECT pg_checksum_bigint(
    pg_table_logical_checksum('demo_items'::regclass, 'fnv64')
) AS logical_checksum_bigint;
~~~

The result is stable across `VACUUM`, `REINDEX`, table rewrites and physical
row movement.  It changes when the logical row content changes.

## 3. Checksum an index

~~~sql
SELECT encode(
    pg_index_logical_checksum('demo_items_pkey'::regclass, 'fnv64'),
    'hex'
);
~~~

A logical index checksum describes the entries the table data implies.  Use a
physical index checksum when you want to inspect the actual index pages:

~~~sql
SELECT encode(
    pg_index_physical_checksum('demo_items_pkey'::regclass, 'fnv64'),
    'hex'
);
~~~

## 4. Capture and verify a CLI baseline

The CLI stores checksums as versioned JSON:

~~~bash
cd cli
go build ./cmd/pg_integrity_checksums

pg_integrity_checksums capture \
  --db "$DATABASE_URL" \
  --schema public \
  --hash fnv64 \
  --output baseline.json

pg_integrity_checksums verify \
  --db "$DATABASE_URL" \
  --baseline baseline.json
~~~

`--hash fnv64` writes a v2 baseline.  The default `capture` without `--hash`
keeps writing the compatible v1 FNV-1a 32-bit format.

A clean verify exits with code `0` and prints `no changes`.  The exit-code
contract is documented in the [CLI reference](cli.md#exit-codes).

## 5. Choose the right checksum

| Question | Use |
|---|---|
| Did the logical data change? | `pg_table_logical_checksum` / CLI logical baseline |
| Did this cluster’s storage change at a known location? | `pg_tuple_physical_checksum` |
| Is an index missing or corrupt? | `pg_index_physical_checksum` |
| Is the whole database logically unchanged? | `pg_database_logical_checksum` (superuser) |
| Do I need a 64-bit, non-cryptographic digest? | Generic `fnv64` overloads |
| Do I need compatibility with existing 2.x values? | Original numeric `fnv32` functions |

## 6. Read next

- [Concepts](concepts.md) — logical vs physical, levels, 32-bit vs 64-bit,
  baselines and limits.
- [Function reference](functions.md) — every function and algorithm.
- [CLI](cli.md) — capture, verify, diff, update, reports and CI usage.
- [Operations](operations.md) — correctness, performance and limitations.
