# pg_integrity_checksums CLI

[![coverage](https://codecov.io/gh/VictorNagibator/pg_integrity_checksums/branch/main/graph/badge.svg)](https://codecov.io/gh/VictorNagibator/pg_integrity_checksums)

The CLI is the companion command-line tool for the
`pg_integrity_checksums` PostgreSQL extension. It captures versioned
baselines, verifies checksums and produces CI-friendly text, JSON and JUnit
reports.

The full CLI contract lives in the documentation site:

- [CLI documentation](https://victornagibator.github.io/pg_integrity_checksums/cli/)
- [CLI documentation source](../docs/cli.md)
- [Release notes](Changes)

## Build and test

```bash
cd cli
go test ./...
go build ./cmd/pg_integrity_checksums
```

The integration suite is opt-in and needs a PostgreSQL database with the
extension installed:

```bash
PGICS_TEST_DSN="postgresql:///app?host=/tmp" go test ./internal/cli -run TestIntegration -v
```
