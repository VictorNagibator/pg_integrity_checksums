# pg_integrity_checksums - Multi-Level Data Integrity Extension for PostgreSQL

[![CI](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/ci.yml/badge.svg)](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/ci.yml)
[![Quality](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/quality.yml/badge.svg)](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/quality.yml)
[![CodeQL](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/codeql.yml/badge.svg)](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/codeql.yml)
[![codecov](https://codecov.io/gh/VictorNagibator/pg_integrity_checksums/branch/main/graph/badge.svg)](https://codecov.io/gh/VictorNagibator/pg_integrity_checksums)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/VictorNagibator/pg_integrity_checksums/badge)](https://scorecard.dev/viewer/?uri=github.com/VictorNagibator/pg_integrity_checksums)

[![Release](https://img.shields.io/github/v/release/VictorNagibator/pg_integrity_checksums?filter=v*&label=extension)](https://github.com/VictorNagibator/pg_integrity_checksums/releases/latest)
[![CLI release](https://img.shields.io/github/v/release/VictorNagibator/pg_integrity_checksums?filter=cli-v*&label=cli)](https://github.com/VictorNagibator/pg_integrity_checksums/releases?q=cli-v)
[![PGXN](https://img.shields.io/badge/PGXN-pg__integrity__checksums-0f5d8f)](https://pgxn.org/dist/pg_integrity_checksums/)
[![PGEXT.CLOUD](https://img.shields.io/badge/PGEXT.CLOUD-packaged-6f42c1)](https://pgext.cloud/ext/pg_integrity_checksums)
[![Docker image](https://img.shields.io/badge/ghcr.io-pg__integrity__checksums-blue?logo=docker)](https://github.com/VictorNagibator/pg_integrity_checksums/pkgs/container/pg_integrity_checksums)
[![PostgreSQL 12-19](https://img.shields.io/badge/PostgreSQL-12--19-336791?logo=postgresql&logoColor=white)](https://www.postgresql.org/)
[![Docs](https://img.shields.io/badge/docs-online-blue)](https://victornagibator.github.io/pg_integrity_checksums/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

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

When two copies of a table differ, the logical checksum can be split by key
bucket (or, on a physical standby, by block range) to find the rows that
differ; see [Finding differing rows](docs/functions.md#finding-differing-rows).

A generic API with an explicit algorithm (FNV-1a 32/64-bit, CRC-32C or
XXH3-64) returns raw `bytea` digests, for a wider or faster checksum.  It is described in the
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

Install it with any of these ([details](docs/installation.md)):

```bash
pgxn install pg_integrity_checksums                     # PGXN
sudo apt install postgresql-16-pg-integrity-checksums   # PIGSTY APT repository
docker run -e POSTGRES_PASSWORD=secret \
  ghcr.io/victornagibator/pg_integrity_checksums:pg16   # image with the extension

git clone https://github.com/VictorNagibator/pg_integrity_checksums.git
cd pg_integrity_checksums && make && sudo make install  # from source (PGXS)
```

The companion CLI captures baselines and verifies a database against them,
with text, JSON and JUnit reports for CI.  Download a
[release binary](https://github.com/VictorNagibator/pg_integrity_checksums/releases?q=cli-v)
or use the `ghcr.io/victornagibator/pg_integrity_checksums-cli` image (see
[CLI installation](docs/cli.md#installation)), then:

```bash
pg_integrity_checksums capture --db "$DATABASE_URL" --output baseline.json
pg_integrity_checksums verify --db "$DATABASE_URL" --baseline baseline.json
```

The CLI uses the 32-bit v1 baseline format by default.  Add
`--hash fnv64` to `capture` when you explicitly need a 64-bit v2 baseline.

## Documentation

- [Documentation site](https://victornagibator.github.io/pg_integrity_checksums/)
- [Getting started](docs/getting-started.md)
- [Installation](docs/installation.md)
- [Upgrading](docs/upgrading.md)
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
as are v1 baselines.  The optional generic API adds explicit `fnv32`/`fnv64`/`crc32c`/`xxh3`
digests and v2 baselines; see [Functions and algorithms](docs/functions.md)
and [Concepts](docs/concepts.md) for selection guidance.

## Support

- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)
- [License](LICENSE)
