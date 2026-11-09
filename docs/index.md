# PostgreSQL integrity checks you can run online

**pg_integrity_checksums** is a PostgreSQL extension for verifying data
integrity at **page, cell, tuple, table, index and database** level. It adds
logical checksums that stay stable across `VACUUM`, `REINDEX`, OID changes and
endianness, plus a CLI for baselines and CI.

[Install](installation.md){ .md-button .md-button--primary }
[CLI reference](cli.md){ .md-button }
[GitHub](https://github.com/VictorNagibator/pg_integrity_checksums){ .md-button }

## Why

`pg_checksums` verifies page checksums offline. `amcheck` verifies B-tree
structure. Both are useful, but neither gives you a repeatable logical
checksum of a table that you can compare after a restore, across a replica, or
between releases.

`pg_integrity_checksums` fills that gap:

- logical checksums are independent of heap TIDs, relation OIDs and type OIDs;
- partitions and partitioned indexes are aggregated transparently;
- parallel scans are controlled by `pg_integrity_checksums.workers`;
- physical and logical variants are available at most levels;
- the CLI stores baselines as versioned JSON and returns CI-friendly exit codes.

## Quick start

### Extension

```sql
CREATE EXTENSION pg_integrity_checksums;

SELECT pg_table_logical_checksum('public.orders'::regclass);
SELECT pg_index_logical_checksum('public.orders_pkey'::regclass);

-- Generic 64-bit digest
SELECT encode(pg_table_logical_checksum('public.orders'::regclass, 'fnv64'), 'hex');
```

### CLI

```bash
cd cli
go build ./cmd/pg_integrity_checksums

pg_integrity_checksums capture \
  --dsn "$DATABASE_URL" \
  --output baseline.json

pg_integrity_checksums verify \
  --dsn "$DATABASE_URL" \
  --baseline baseline.json
```

See the [installation guide](installation.md) for packages, PGXN, Docker and
source builds.

## What it is not

- FNV-1a 32-bit and 64-bit are fast, but neither is **cryptographic** and
  neither is a substitute for a cryptographic hash.
- Physical checksums are tied to one cluster and database.
- Logical checksums describe data content, not the physical layout.
- The extension does not replace backups, `pg_checksums` or `amcheck`.
  It complements them.

Read [limitations](operations.md) and the [function reference](functions.md)
before relying on checksums for a compliance process.

## Documentation

- [Installation](installation.md)
- [Architecture](architecture.md)
- [Functions and algorithms](functions.md)
- [CLI](cli.md)
- [Operations](operations.md)
- [Testing and releases](testing.md)
- [Troubleshooting](troubleshooting.md)

Project source and issue tracker:
[github.com/VictorNagibator/pg_integrity_checksums](https://github.com/VictorNagibator/pg_integrity_checksums).
