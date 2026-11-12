# pg_integrity_checksums - Multi-Level Data Integrity Extension for PostgreSQL

[![CI](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/ci.yml/badge.svg)](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/ci.yml)
[![CodeQL](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/codeql.yml/badge.svg)](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/codeql.yml)
[![Quality](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/quality.yml/badge.svg)](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/quality.yml)
[![Docker image](https://img.shields.io/badge/ghcr.io-pg__integrity__checksums-blue?logo=docker)](https://github.com/VictorNagibator/pg_integrity_checksums/pkgs/container/pg_integrity_checksums)
[![PostgreSQL 12-19](https://img.shields.io/badge/postgres-12--19-336791)](https://www.postgresql.org/)
[![PGEXT.CLOUD](https://img.shields.io/badge/PGEXT.CLOUD-packaged-6f42c1)](https://pgext.cloud/ext/pg_integrity_checksums)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Docs](https://img.shields.io/badge/docs-online-blue)](https://victornagibator.github.io/pg_integrity_checksums/)

## Overview

pg_integrity_checksums provides cell, tuple, table, index and database
checksums.  Cell checksums are logical; tuple, table, index and database
checksums support both physical and logical modes.

Logical checksums are canonical across endianness, VACUUM/REINDEX/physical
movement and database OIDs.  Physical checksums are for on-disk corruption in
one specific cluster.

The default SQL API uses FNV-1a 32-bit and returns ordinary SQL integers:

```sql
SELECT pg_table_logical_checksum('public.orders'::regclass);
SELECT pg_table_physical_checksum('public.orders'::regclass, false);
```

A generic API with explicit FNV-1a 32-bit or 64-bit digests is available when a
wider checksum or a raw `bytea` value is required.  It is described in the
[function reference](docs/functions.md).

## Errors and skipped relations

Database-level checksums are **strict by default** (since 2.5): if a relation
cannot be checksummed (an invalid index, a corrupt page, an unlogged table on a
standby), the call fails and names the relation, instead of returning a value
that silently leaves it out.  `SET pg_integrity_checksums.strict = off` skips
such relations with a warning; do not use that value for monitoring.  See
[Strict mode](docs/operations.md#strict-mode).

## Quick start

```sql
CREATE EXTENSION pg_integrity_checksums;
SELECT pg_table_logical_checksum('public.orders'::regclass);
```

Install from packages, Docker, PGXN or build with PGXS:

```bash
git clone https://github.com/VictorNagibator/pg_integrity_checksums.git
cd pg_integrity_checksums
make
sudo make install
```

The CLI lives in [cli/](cli/README.md):

```bash
cd cli
go build ./cmd/pg_integrity_checksums
pg_integrity_checksums capture --db "$DATABASE_URL" --output baseline.json
pg_integrity_checksums verify --db "$DATABASE_URL" --baseline baseline.json
```

The CLI uses the 32-bit v1 baseline format by default.  Add
`--hash fnv64` to `capture` when you explicitly need a 64-bit v2 baseline.

## Documentation

- [Documentation site](https://victornagibator.github.io/pg_integrity_checksums/)
- [Getting started](docs/getting-started.md)
- [Installation](docs/installation.md)
- [Concepts](docs/concepts.md)
- [Function reference](docs/functions.md)
- [Architecture](docs/architecture.md)
- [CLI documentation](docs/cli.md)
- [Operations](docs/operations.md)
- [Testing and releases](docs/testing.md)
- [Troubleshooting](docs/troubleshooting.md)
- [CLI changelog](cli/Changes)
- [Extension changelog](Changes)
- [Migration from 1.x](MIGRATION.md)

## Compatibility

The 32-bit numeric SQL functions are the default and remain fully supported,
as are v1 baselines.  The optional generic API adds explicit `fnv32`/`fnv64`
digests and v2 baselines; see [Functions and algorithms](docs/functions.md)
and [Concepts](docs/concepts.md) for selection guidance.

## Support

- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)
- [License](LICENSE)
