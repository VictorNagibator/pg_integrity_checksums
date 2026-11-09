# pg_integrity_checksums CLI

[![coverage](https://codecov.io/gh/VictorNagibator/pg_integrity_checksums/branch/main/graph/badge.svg)](https://codecov.io/gh/VictorNagibator/pg_integrity_checksums)

Companion command-line tool for the `pg_integrity_checksums` PostgreSQL
extension.  The extension provides checksum functions; this tool adds
baselines, verification, diffs, filters and CI-friendly exit codes.

The CLI is released independently from the extension.  It talks to the
database through SQL and does not install or modify extension objects.  Release
notes live in [Changes](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/cli/Changes) and the release workflow reads the section for
the pushed `cli-v*` tag automatically.

## Quick start

The CLI requires the extension in the target database.  Version 0.4 works
best with extension 2.2, which provides the `fnv64` API.

~~~bash
# From the repository (Go 1.25+)
cd cli
go build ./cmd/pg_integrity_checksums

# Capture a 64-bit logical baseline
./pg_integrity_checksums capture \
  --db "$DATABASE_URL" \
  --schema public \
  --hash fnv64 \
  --output baseline.json

# Verify the database against it
./pg_integrity_checksums verify \
  --db "$DATABASE_URL" \
  --baseline baseline.json
~~~

A clean verify prints `no changes` and exits `0`.  See
[Getting started](getting-started.md) for the extension side and the
[command reference](#commands) below for every flag.

## Status

The CLI is currently **0.4.0**.  It is versioned separately from the
extension.  It writes **v1** (FNV-1a 32-bit) by default and **v2** with
`capture --hash fnv64`.

## Commands

```text
pg_integrity_checksums init      [options]
pg_integrity_checksums config    validate|show [options]
pg_integrity_checksums capture   [--config FILE] [--profile NAME] [options]
pg_integrity_checksums verify    [--config FILE] [--profile NAME] [options]
pg_integrity_checksums diff      OLD.json NEW.json [options]
pg_integrity_checksums show      FILE [options]
pg_integrity_checksums update    [--config FILE] [--profile NAME] [options]
pg_integrity_checksums doctor    [--config FILE] [--profile NAME] [options]
pg_integrity_checksums completion bash|zsh|fish|powershell
pg_integrity_checksums version
```

### `init`

Creates a starter `.pgics.yaml` without connecting to PostgreSQL:

```bash
pg_integrity_checksums init \
  --profile prod \
  --dsn env:DATABASE_URL \
  --baseline baselines/prod.json \
  --schema public
```

The command refuses to overwrite an existing file unless `--force` is used.
Relative `baseline` paths are written into the profile; directories are
created as needed.

### `config`

```bash
pg_integrity_checksums config validate
pg_integrity_checksums config validate --config .pgics.yaml --profile prod
pg_integrity_checksums config show --profile prod
```

`validate` checks the YAML, profiles, durations, DSN environment references and
known format/kind/fail-on values. `show` prints the effective profile with the
DSN password masked.

### Configuration profiles

`.pgics.yaml` profiles provide defaults for repeated commands:

```yaml
default: prod

profiles:
  prod:
    dsn: env:DATABASE_URL
    schema: [public]
    exclude_table: [audit.*]
    baseline: baselines/prod.json
    output: baselines/prod.json
    workers: 4
    hash: fnv64
    timeout: 5m
    connect_timeout: 10s
    application_name: pg_integrity_checksums
    fail_on: changed
```

```bash
pg_integrity_checksums verify --profile prod
PGICS_PROFILE=prod pg_integrity_checksums verify
```

Precedence: command-line flags, environment, profile, built-in defaults.
`PGICS_CONFIG` and `--config` select the file.  Relative `baseline` and
`output` paths resolve against the config file directory.  `env:NAME` DSNs
read a connection string from the environment.

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
| `--dsn` | | Alias for `--db` |
| `--config` | `.pgics.yaml` | Config file |
| `--profile` | config default | Profile name |
| `--connect-timeout` | 15s | Connection timeout |
| `--application-name` | | Value for `pg_stat_activity.application_name` |
| `--output` | `baseline.json` | Output file; `-` means stdout; `.gz` is compressed |
| `--kind` | `logical` | `logical` or `physical` |
| `--hash` | `fnv32` | `fnv32` (v1) or `fnv64` (v2) |
| `--schema` | all non-system | Schemas to include; globs allowed |
| `--exclude-schema` | | Schemas to exclude; globs allowed |
| `--table` | all | `schema.table` or table glob to include |
| `--exclude-table` | | `schema.table` or table glob to exclude |
| `--no-indexes` | false | Do not discover indexes |
| `--include-system` | false | Include system schemas |
| `--workers` | 0 | Extension parallel workers (GUC) |
| `--timeout` | 0 | `statement_timeout`, e.g. `5m` |
| `--strict` | false | Fail if an object cannot be checksummed |
| `--quiet` | false | No progress on stderr |

`--kind physical` is opt-in.  A physical baseline is tied to one cluster;
it records `system_identifier` and cannot be used against another cluster.

`--hash fnv64` writes a **v2** baseline.  The default `--hash fnv32` keeps
writing the historical **v1** format.  `verify` and `update` inherit the
hash algorithm from the baseline they read; there is no separate verify flag.

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
| `--db` | `DATABASE_URL`/PG env | PostgreSQL connection string |
| `--dsn` | | Alias for `--db` |
| `--config` | `.pgics.yaml` | Config file |
| `--profile` | config default | Profile name |
| `--connect-timeout` | 15s | Connection timeout |
| `--application-name` | | Value for `pg_stat_activity.application_name` |
| `--format` | `text` | `text`, `json` or `junit` |
| `--output` | `-` | Report file; `-` means stdout |
| `--fail-on` | `any` | `changed`, `schema-changed`, `new`, `missing`, `any`, `none` |
| `--show-ok` | false | Include unchanged objects in text output |
| `--allow-version-mismatch` | false | Allow a different PostgreSQL major |
| `--changed-since` | false | Reuse unchanged tables/indexes (logical only, heuristic) |
| `--workers` | 0 | Extension parallel workers |
| `--timeout` | 0 | `statement_timeout` |
| `--quiet` | false | No progress on stderr |

### `diff`

Compares two baseline files without connecting to PostgreSQL.

```bash
pg_integrity_checksums diff before.json after.json
```

Flags and the two files may appear in any order.  `--format` accepts `text`,
`json` or `junit`.  `--output` writes a report file.  Exit code is `1` when
changes are found and `0` when the object maps match.  `--no-fail` makes a
change-only diff exit 0.

### `show`

Prints baseline metadata and a compact object summary.  With `--format json`
it prints the baseline document as JSON.  `--objects` lists every object,
including skipped ones.  `--output` writes the report to a file.

Flags and the file may appear in any order, for example:

```bash
pg_integrity_checksums show baseline.json --objects
```

### `update`

Captures a new baseline using the old baseline's scope.  It prints the diff and
requires `--yes` before replacing the file.  There is no automatic update on
DML or DDL.  The file is replaced atomically.  `--format` accepts `text`,
`json` and `junit`; `--output` writes the diff report to a file.  `--config`,
`--profile`, `--dsn`, `--connect-timeout` and `--application-name` work as in
`capture`.

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
database, and the number of objects that would be captured.  `--verbose`
lists every object with its preflight status; `--strict` exits 1 when a
problem is found.  `--format json` and `--output` are supported.  The preflight
checks at least SELECT privileges and row-level security.  `--timeout` sets
`statement_timeout` on the diagnostic connection.

### `completion`

Prints a shell completion script:

```bash
pg_integrity_checksums completion bash > /etc/bash_completion.d/pg_integrity_checksums
pg_integrity_checksums completion zsh
pg_integrity_checksums completion fish
pg_integrity_checksums completion powershell
```

### Docker

```bash
docker build -t pgics-cli ./cli
docker run --rm -e DATABASE_URL -v "$PWD:/work" pgics-cli verify --baseline /work/baseline.json
```

### GitHub Action

The composite action at `cli/action.yml` builds the CLI and runs verify:

```yaml
- uses: VictorNagibator/pg_integrity_checksums/cli@cli-v0.2.0
  with:
    baseline: baselines/prod.json
    fail_on: changed
```

## Exit codes

| Code | Meaning |
|---:|---|
| 0 | Success / no blocking differences |
| 1 | Differences found (according to `--fail-on`), or diff found changes |
| 2 | Operational error: connection, SQL, permissions, corrupt page |
| 3 | Invalid baseline: format, JSON, kind or incompatible version |
| 4 | `update`/destructive operation refused without `--yes` |

## Baseline formats

### v1 — FNV-1a 32-bit

The historical format identifier is:

```text
pg_integrity_checksums.baseline/v1
```

v1 has no `hash` field and always uses the legacy numeric FNV-1a 32-bit
wrappers.  The CLI continues to read and write it by default.

Example:

```json
{
  "format": "pg_integrity_checksums.baseline/v1",
  "baseline_id": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "created_at": "2026-11-06T16:18:40Z",
  "tool_version": "0.2.0-dev",
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
      "checksum": "161531096",
      "schema_hash": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "stats": {
        "relfilenode": 16399,
        "inserted": 100,
        "updated": 2,
        "deleted": 1
      }
    },
    {
      "kind": "index",
      "schema": "public",
      "name": "orders_pkey",
      "table_schema": "public",
      "table_name": "orders",
      "status": "ok",
      "checksum": "-1867570346",
      "schema_hash": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    }
  ]
}
```

### v2 — explicit checksum algorithm

v2 adds a required `hash` field and stores the algorithm explicitly:

```text
pg_integrity_checksums.baseline/v2
```

```json
{
  "format": "pg_integrity_checksums.baseline/v2",
  "hash": "fnv64",
  "kind": "logical",
  "extension_version": "2.2.0"
}
```

The supported values are `fnv32` and `fnv64`.  The CLI writes `fnv64` v2
baselines when `capture --hash fnv64` is used.  Older CLI releases reject v2
instead of silently comparing incompatible digests.  The current CLI reads
both v1 and v2 baselines.

Rules:

- Checksums are **JSON strings**, because SQL `bigint` values do not fit
  safely into JavaScript numbers.
- `kind` is `logical` or `physical`.
- Object `kind` is `table` or `index`.
- OIDs are never stored; identity is schema-qualified names.
- `schema_hash` is a SHA-256 of the portable object definition (columns,
  constraints, partition key, reloptions and index definition). It lets
  verify/diff report `schema_changed` separately from a data change.
- `stats` is table-only change-detection data from `pg_stat_all_tables` plus
  `relfilenode`; it is used only by `--changed-since`.
- Objects are sorted by `kind`, `schema`, `name`.
- `status` is `ok` or `skipped`; skipped objects may carry `reason`.
- A comparison adds `schema_changed` when the stored and current
  `schema_hash` differ; `--fail-on changed` counts it as a change.
- `baseline_id` is `sha256:` plus the SHA-256 of the canonical JSON encoding
  of `kind`, `hash` (when present), `extension_version`, `postgres_version`,
  `database` and the sorted object list.
- Physical baselines additionally contain `system_identifier` (string) and
  `database_oid` (number).
- Unknown fields are rejected only by schema validation tooling; the CLI
  ignores unknown fields when reading, for forward compatibility.

The full JSON Schemas are in `docs/baseline-v1.schema.json` and
`docs/baseline-v2.schema.json`.

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

### `--changed-since`

`--changed-since` is a logical-only optimization for verify. It reuses a
baseline checksum when all of the following are true:

- the object's `schema_hash` is unchanged;
- for a table, `relfilenode` and the `n_tup_ins`/`n_tup_upd`/`n_tup_del`
  counters are unchanged and non-zero;
- for an index, the owning table is unchanged and the index definition hash is
  unchanged.

The statistics counters are a heuristic: a reset, overflow or missing stats row
forces a full checksum. Use a normal verify when exactness matters. Physical
baselines do not support `--changed-since`.

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

## Release artifacts

Each `cli-v*` release contains the platform binaries, `SHA256SUMS`,
`SHA256SUMS.bundle` and `sbom.spdx.json`.  Verify a download:

```bash
sha256sum -c SHA256SUMS
cosign verify-blob \
  --bundle SHA256SUMS.bundle \
  --certificate-identity-regexp 'https://github.com/VictorNagibator/pg_integrity_checksums/.github/workflows/cli-release.yml@refs/tags/cli-v.*' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  SHA256SUMS
```

The SBOM can be inspected with any SPDX tool.  Release jobs use keyless
Sigstore signing through the GitHub OIDC token.

## Versioning

- Baseline formats: `v1` (FNV-1a 32-bit) and `v2` (explicit `hash`).
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
