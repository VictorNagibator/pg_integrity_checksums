# pg_integrity_checksums - Multi-Level Data Integrity Extension for PostgreSQL

[![CI](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/ci.yml/badge.svg)](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/ci.yml)
[![CodeQL](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/codeql.yml/badge.svg)](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/codeql.yml)
[![Quality](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/quality.yml/badge.svg)](https://github.com/VictorNagibator/pg_integrity_checksums/actions/workflows/quality.yml)
[![Docker image](https://img.shields.io/badge/ghcr.io-pg__integrity__checksums-blue?logo=docker)](https://github.com/VictorNagibator/pg_integrity_checksums/pkgs/container/pg_integrity_checksums)
[![PostgreSQL 12-19](https://img.shields.io/badge/postgres-12--19-336791)](https://www.postgresql.org/)
[![PGEXT.CLOUD](https://img.shields.io/badge/PGEXT.CLOUD-packaged-6f42c1)](https://pgext.cloud/ext/pg_integrity_checksums)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![codecov CLI](https://codecov.io/gh/VictorNagibator/pg_integrity_checksums/branch/main/graph/badge.svg?flag=cli)](https://codecov.io/gh/VictorNagibator/pg_integrity_checksums)
[![XERJ Corpus Hub contributor](https://hub.xerj.org/contributors/VictorNagibator/badge.svg)](https://hub.xerj.org/contributors/VictorNagibator)

## Overview

pg_integrity_checksums provides page, cell, tuple, table, index and database
checksums.  Page checksums are physical; cell checksums are logical; the other
levels support both physical and logical modes.

Logical checksums are canonical across endianness, VACUUM/REINDEX/physical
movement and database OIDs.  Physical checksums are for on-disk corruption in
one specific cluster.

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

## Documentation

- [Architecture](docs/architecture.md)
- [Installation](docs/installation.md)
- [Functions and algorithms](docs/functions.md)
- [CLI contract](cli/README.md)
- [Operations](docs/operations.md)
- [Testing and releases](docs/testing.md)
- [Troubleshooting](docs/troubleshooting.md)
- [CLI changelog](cli/Changes)
- [Extension changelog](Changes)
- [Migration from 1.x](MIGRATION.md)

## Support

- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)
- [License](LICENSE)
