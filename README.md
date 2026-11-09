# pg_integrity_checksums - Multi-Level Data Integrity Extension for PostgreSQL

[![CI](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/ci.yml/badge.svg)](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/ci.yml)
[![CodeQL](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/codeql.yml/badge.svg)](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/codeql.yml)
[![Quality](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/quality.yml/badge.svg)](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/quality.yml)
[![Docker image](https://img.shields.io/badge/ghcr.io-pg__integrity__checksums-blue?logo=docker)](https://github.com/VictorNagibator/pg_integrity_checksums/pkgs/container/pg_integrity_checksums)
[![PostgreSQL 12-19](https://img.shields.io/badge/postgres-12--19-336791)](https://www.postgresql.org/)
[![PGEXT.CLOUD](https://img.shields.io/badge/PGEXT.CLOUD-packaged-6f42c1)](https://pgext.cloud/ext/pg_integrity_checksums)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Docs](https://img.shields.io/badge/docs-online-blue)](https://victornagibator.github.io/pg_integrity_checksums/)
[![XERJ Corpus Hub contributor](https://hub.xerj.org/contributors/VictorNagibator/badge.svg)](https://hub.xerj.org/contributors/VictorNagibator)

## Overview

pg_integrity_checksums provides page, cell, tuple, table, index and database
checksums.  Page checksums are physical; cell checksums are logical; the other
levels support both physical and logical modes.

Logical checksums are canonical across endianness, VACUUM/REINDEX/physical
movement and database OIDs.  Physical checksums are for on-disk corruption in
one specific cluster.

The current API uses FNV-1a 64-bit and returns raw `bytea` digests:

```sql
SELECT encode(pg_table_logical_checksum('public.orders'::regclass, 'fnv64'), 'hex');
SELECT pg_checksum_bigint(pg_table_logical_checksum('public.orders'::regclass, 'fnv64'));
```

Page checksums remain PostgreSQL's built-in 32-bit page checksum.

## Quick start

```sql
CREATE EXTENSION pg_integrity_checksums;
SELECT encode(pg_table_logical_checksum('public.orders'::regclass, 'fnv64'), 'hex');
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
pg_integrity_checksums capture --db "$DATABASE_URL" --hash fnv64 --output baseline.json
pg_integrity_checksums verify --db "$DATABASE_URL" --baseline baseline.json
```

`--hash fnv64` creates a v2 baseline.  Without it, `capture` keeps writing
the compatible v1 FNV-1a 32-bit format.

## Documentation

- [Documentation site](https://victornagibator.github.io/pg_integrity_checksums/)
- [Architecture](docs/architecture.md)
- [Installation](docs/installation.md)
- [Functions and algorithms](docs/functions.md)
- [CLI documentation](docs/cli.md)
- [Operations](docs/operations.md)
- [Testing and releases](docs/testing.md)
- [Troubleshooting](docs/troubleshooting.md)
- [CLI changelog](cli/Changes)
- [Extension changelog](Changes)
- [Migration from 1.x](MIGRATION.md)

## Compatibility

The original numeric 32-bit SQL functions and v1 baselines remain supported
for existing users.  New code should use the algorithm argument and the
`bytea` API described in [Functions and algorithms](docs/functions.md).

## Support

- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)
- [License](LICENSE)
