# PostgreSQL integrity checks you can run online

**pg_integrity_checksums** is a PostgreSQL extension and CLI for checking data
integrity at **page, cell, tuple, table, index and database** level.

It provides logical checksums that stay stable across `VACUUM`, `REINDEX`, OID
changes and physical row movement, plus physical checksums for on-disk
corruption within one cluster.

[Get started](getting-started.md){ .md-button .md-button--primary }
[Concepts](concepts.md){ .md-button }
[CLI reference](cli.md){ .md-button }

## What you can check

| Level | Physical | Logical | Example use |
|---|---:|---:|---|
| Page | yes | — | Single-page corruption |
| Cell | — | yes | One value in one row |
| Tuple | yes | yes | Physical or logical row fingerprint |
| Table | yes | yes | Data change detection, restore checks |
| Index | yes | yes | Missing index entries vs corrupt pages |
| Database | yes | yes | Whole-database fingerprint |

Page checksums are PostgreSQL’s built-in 16-bit page checksum.  Use
`pg_page_checksum_status` to compare a page with its stored checksum.  The
other levels use FNV-1a 32-bit by default and optionally expose 64-bit
digests through the generic API.

## First checksum

~~~sql
CREATE EXTENSION pg_integrity_checksums;

SELECT pg_table_logical_checksum('public.orders'::regclass);
~~~

The default functions return ordinary SQL integers.  A 64-bit digest is
available when needed:

~~~sql
SELECT encode(
    pg_table_logical_checksum('public.orders'::regclass, 'fnv64'),
    'hex'
);
~~~

See [Concepts](concepts.md#which-api-should-i-use) for when to choose 32-bit
or 64-bit.

## Why teams use it

- **Logical comparison** across a restore, replica, migration or release.
- **Physical detection** of corrupted pages, tuples, tables and indexes.
- **No primary key required** for logical table and database checksums.
- **Partition-aware** table and index checksums.
- **Parallel scans** through `pg_integrity_checksums.workers`.
- **CI-friendly baselines** from the companion CLI, with text, JSON and JUnit
  reports.

## Choose your path

| I want to... | Read |
|---|---|
| Install and run my first checksum | [Getting started](getting-started.md) |
| Upgrade an existing installation | [Upgrading](upgrading.md) |
| Understand logical vs physical and 32-bit vs 64-bit | [Concepts](concepts.md) |
| Look up a function | [Function reference](functions.md) |
| Use baselines in CI | [CLI reference](cli.md) |
| Understand performance and correctness limits | [Operations](operations.md) |
| Debug an error | [Troubleshooting](troubleshooting.md) |
| Understand the internals | [Architecture](architecture.md) |

## What it is not

- FNV-1a is **not cryptographic**; it is not tamper evidence.
- Physical checksums are tied to one cluster and database.
- Logical checksums describe content, not physical layout.
- It does not replace backups, `pg_checksums` or `amcheck`.

## Documentation

- [Getting started](getting-started.md)
- [Concepts](concepts.md)
- [Installation](installation.md)
- [Upgrading](upgrading.md)
- [Function reference](functions.md)
- [CLI](cli.md)
- [Operations](operations.md)
- [Troubleshooting](troubleshooting.md)
- [Architecture](architecture.md)
- [Testing and releases](testing.md)

Source and issue tracker:
[github.com/VictorNagibator/pg_integrity_checksums](https://github.com/VictorNagibator/pg_integrity_checksums).
