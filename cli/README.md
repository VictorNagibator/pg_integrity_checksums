# pg_integrity_checksums CLI

Companion command-line tool for the `pg_integrity_checksums` PostgreSQL
extension.  The extension provides checksum functions; this tool adds
baselines, verification, diffs, filters and CI-friendly exit codes.

The CLI is released independently from the extension.  It talks to the
database through SQL and does not install or modify extension objects.

## Status

Contract and initial implementation are versioned separately from the
extension.  The baseline format is **v1**.

## Commands

```text
pg_integrity_checksums capture   --db DSN --output FILE [options]
pg_integrity_checksums verify    --db DSN --baseline FILE [options]
pg_integrity_checksums diff      OLD.json NEW.json [options]
pg_integrity_checksums show      FILE [options]
pg_integrity_checksums update    --db DSN --baseline FILE --yes [options]
pg_integrity_checksums doctor    --db DSN [options]
pg_integrity_checksums version
```

### `capture`

Discovers objects in scope, computes checksums in one repeatable-read
read-only transaction, and writes a versioned JSON baseline.

```bash
pg_integrity_checksums capture \
  --db "$DATABASE_URL" \
  --output baseline.json \
  --schema public
```

Options:

| Flag | Default | Meaning |
|---|---|---|
| `--db` | `DATABASE_URL`/PG env | PostgreSQL connection string |
| `--output` | `baseline.json` | Output file; `-` means stdout |
| `--kind` | `logical` | `logical` or `physical` |
| `--schema` | all non-system | Schemas to include; repeatable |
| `--exclude-schema` | | Schemas to exclude; repeatable |
| `--table` | all | `schema.table` to include; repeatable |
| `--exclude-table` | | `schema.table` to exclude; repeatable |
| `--no-indexes` | false | Do not discover indexes |
| `--include-system` | false | Include system schemas |
| `--workers` | 0 | Extension parallel workers (GUC) |
| `--timeout` | 0 | `statement_timeout`, e.g. `5m` |
| `--strict` | false | Fail if an object cannot be checksummed |
| `--quiet` | false | No progress on stderr |

`--kind physical` is opt-in.  A physical baseline is tied to one cluster;
it records `system_identifier` and cannot be used against another cluster.

### `verify`

Re-discovers the objects using the scope recorded in the baseline and compares
their current checksums with the baseline.

```bash
pg_integrity_checksums verify \
  --db "$DATABASE_URL" \
  --baseline baseline.json
```

Options:

| Flag | Default | Meaning |
|---|---|---|
| `--format` | `text` | `text` or `json` |
| `--fail-on` | `any` | `changed`, `new`, `missing`, `any`, `none` |
| `--show-ok` | false | Include unchanged objects in text output |
| `--allow-version-mismatch` | false | Allow a different PostgreSQL major |
| `--workers` | 0 | Extension parallel workers |
| `--timeout` | 0 | `statement_timeout` |
| `--quiet` | false | No progress on stderr |

### `diff`

Compares two baseline files without connecting to PostgreSQL.

```bash
pg_integrity_checksums diff before.json after.json
```

Flags and the two files may appear in any order.  Exit code is `1` when
changes are found and `0` when the object maps match.  `--no-fail` makes a
change-only diff exit 0.

### `show`

Prints baseline metadata and a compact object summary.  With `--format json`
it prints the baseline document as JSON.  `--objects` lists every object,
including skipped ones.

Flags and the file may appear in any order, for example:

```bash
pg_integrity_checksums show baseline.json --objects
```

### `update`

Captures a new baseline using the old baseline's scope.  It prints the diff and
requires `--yes` before replacing the file.  There is no automatic update on
DML or DDL.  The file is replaced atomically.

```bash
pg_integrity_checksums update \
  --db "$DATABASE_URL" \
  --baseline baseline.json \
  --yes

# Preview the diff without writing
pg_integrity_checksums update \
  --db "$DATABASE_URL" \
  --baseline baseline.json \
  --dry-run
```

### `doctor`

Checks that the extension is installed, reports versions, the current
database, and the number of objects that would be captured.  `--timeout`
sets `statement_timeout` on the diagnostic connection.

## Exit codes

| Code | Meaning |
|---:|---|
| 0 | Success / no blocking differences |
| 1 | Differences found (according to `--fail-on`), or diff found changes |
| 2 | Operational error: connection, SQL, permissions, corrupt page |
| 3 | Invalid baseline: format, JSON, kind or incompatible version |
| 4 | `update`/destructive operation refused without `--yes` |

## Baseline format v1

The canonical format identifier is:

```text
pg_integrity_checksums.baseline/v1
```

Example:

```json
{
  "format": "pg_integrity_checksums.baseline/v1",
  "baseline_id": "sha256:...",
  "created_at": "2026-11-06T16:18:40Z",
  "tool_version": "0.1.0",
  "kind": "logical",
  "extension_version": "2.1.0",
  "postgres_version": 160014,
  "database": "app",
  "scope": {
    "schemas": ["public"],
    "exclude_schemas": [],
    "tables": [],
    "exclude_tables": [],
    "include_system": false,
    "include_indexes": true
  },
  "objects": [
    {
      "kind": "table",
      "schema": "public",
      "name": "orders",
      "status": "ok",
      "checksum": "161531096"
    },
    {
      "kind": "index",
      "schema": "public",
      "name": "orders_pkey",
      "table_schema": "public",
      "table_name": "orders",
      "status": "ok",
      "checksum": "-1867570346"
    }
  ]
}
```

Rules:

- Checksums are **JSON strings**, because SQL `bigint` values do not fit
  safely into JavaScript numbers.
- `kind` is `logical` or `physical`.
- Object `kind` is `table` or `index`.
- OIDs are never stored; identity is schema-qualified names.
- Objects are sorted by `kind`, `schema`, `name`.
- `status` is `ok` or `skipped`; skipped objects may carry `reason`.
- `baseline_id` is `sha256:` plus the SHA-256 of the canonical JSON encoding
  of `kind`, `extension_version`, `postgres_version`, `database` and the
  sorted object list.
- Physical baselines additionally contain `system_identifier` (string) and
  `database_oid` (number).
- Unknown fields are rejected only by schema validation tooling; the CLI
  ignores unknown fields when reading, for forward compatibility.

The full JSON Schema is in `docs/baseline-v1.schema.json`.

## Semantics

### Snapshot consistency

Capture and verify run in one transaction:

```sql
BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY;
```

All objects in a baseline therefore describe the same database snapshot.
Different tables are never read from separate snapshots or separate
connections.  Parallelism is provided by the extension's
`pg_integrity_checksums.workers` GUC inside that transaction.

### Object scope

By default the CLI discovers:

- ordinary tables (`r`), partitioned tables (`p`) and materialized views (`m`);
- top-level relations only: partition leaves are not duplicated;
- indexes (`i`) and partitioned indexes (`I`) when indexes are enabled;
- non-system schemas (`pg_catalog`, `information_schema`, `pg_toast` and
  temporary schemas are excluded).

A partitioned table/index is checksummed through its parent, which aggregates
its leaves.  `--include-system` includes system schemas.  Foreign tables,
views, sequences and TOAST relations are not captured as separate objects.

### Logical vs physical

`logical` is the default:

- stable across VACUUM/REINDEX/physical movement;
- suitable for migrations and cross-database comparison;
- excludes relation OIDs, heap TIDs and type OIDs.

`physical` is opt-in:

- depends on storage, page locations, LSNs and architecture;
- only valid for the same cluster and storage state;
- the baseline records `system_identifier` and is rejected on another cluster.

### No automatic baseline updates

The CLI does not install triggers, start background workers or update
baselines after DML.  `update` is explicit and requires `--yes`.  This
prevents a corrupted database from silently replacing a known-good baseline.

### Permissions

Capture requires the same privileges as the checksum functions: `SELECT` on
each object and superuser for database-level checks.  RLS-protected relations
are rejected by the extension and are reported as errors unless skipped.

## Examples

```bash
# Capture public schema, logical
pg_integrity_checksums capture --db "$DATABASE_URL" \
  --schema public --output prod-baseline.json

# CI verification
pg_integrity_checksums verify --db "$DATABASE_URL" \
  --baseline prod-baseline.json --format json --fail-on changed

# Compare two snapshots from different releases
pg_integrity_checksums diff before.json after.json

# Physical snapshot of one cluster, after maintenance
pg_integrity_checksums capture --db "$DATABASE_URL" \
  --kind physical --output physical.json
```

## Versioning

- Baseline format version: `v1`.
- CLI version: independent `0.x.y` / `1.x.y` semver.
- Extension compatibility is checked at runtime and recorded in the baseline.
- New optional fields may be added without a format bump; breaking changes
  require a new `format` identifier.

## Building and testing

```bash
cd cli
go test ./...
go vet ./...
go build ./cmd/pg_integrity_checksums
```

The integration test is opt-in and needs an installed extension:

```bash
PGICS_TEST_DSN="postgresql:///app?host=/tmp"   go test ./internal/cli -run TestIntegration -v
```

It creates and drops a temporary schema, so use a test database.
