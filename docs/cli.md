# pg_integrity_checksums CLI

[![coverage](https://codecov.io/gh/VictorNagibator/pg_integrity_checksums/branch/main/graph/badge.svg)](https://codecov.io/gh/VictorNagibator/pg_integrity_checksums)

Companion command-line tool for the `pg_integrity_checksums` PostgreSQL
extension.  The extension provides checksum functions; this tool adds
baselines, verification, diffs, filters and CI-friendly exit codes.

The CLI is released independently from the extension.  It talks to the
database through SQL and does not install or modify extension objects.  Release
notes live in [Changes](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/cli/Changes) and the release workflow reads the section for
the pushed `cli-v*` tag automatically.

## Installation

The CLI is a single static binary with no dependencies.  It needs the
extension installed in the database it connects to.

### Release binaries (recommended)

Each [`cli-v*` release](https://github.com/VictorNagibator/pg_integrity_checksums/releases?q=cli-v)
has binaries for Linux (amd64, arm64), macOS (amd64, arm64), FreeBSD (amd64)
and Windows (amd64):

~~~bash
VERSION=cli-v0.7.0
ASSET=pg_integrity_checksums-$VERSION-linux-amd64
BASE=https://github.com/VictorNagibator/pg_integrity_checksums/releases/download/$VERSION

curl -LO "$BASE/$ASSET"
curl -LO "$BASE/SHA256SUMS"
sha256sum --check --ignore-missing SHA256SUMS

install -m 0755 "$ASSET" ~/.local/bin/pg_integrity_checksums
pg_integrity_checksums version
~~~

To also check that `SHA256SUMS` was produced by the release workflow, see
[Release artifacts](#release-artifacts).

### Container image

A signed multi-arch image (amd64, arm64) is published to GHCR.  It contains
only the CLI, not PostgreSQL:

~~~bash
docker run --rm -e DATABASE_URL -v "$PWD:/work" -w /work \
  ghcr.io/victornagibator/pg_integrity_checksums-cli:latest \
  verify --baseline baseline.json
~~~

Tags: `latest`, the release tag (for example `cli-v0.7.0`) and the plain
version (`0.7.0`).  The image runs as an unprivileged
user, so the mounted directory must be writable by it if the command writes a
baseline or a report.

### go install

With Go 1.25 or newer:

~~~bash
go install github.com/VictorNagibator/pg_integrity_checksums/cli/cmd/pg_integrity_checksums@cli-v0.7.0
~~~

The binary works the same, but `version` reports a `-dev` suffix because
`go install` does not stamp the release version.  Use a release binary when the
version string matters (it is recorded in baselines as `tool_version`).

### From source

~~~bash
git clone https://github.com/VictorNagibator/pg_integrity_checksums.git
cd pg_integrity_checksums/cli
go build -o pg_integrity_checksums ./cmd/pg_integrity_checksums
~~~

## Quick start

The default 32-bit baseline works with any supported extension version; the
optional `fnv64` format requires extension 2.2 or newer, `crc32c` and `xxh3`
require 2.9 or newer.

~~~bash
# Capture a logical baseline (32-bit v1 by default)
pg_integrity_checksums capture \
  --db "$DATABASE_URL" \
  --schema public \
  --output baseline.json

# Verify the database against it
pg_integrity_checksums verify \
  --db "$DATABASE_URL" \
  --baseline baseline.json
~~~

Add `--hash fnv64` (or `xxh3`, `crc32c`) to `capture` to create a v2
baseline.  A clean verify
prints `no changes` and exits `0`.  See [Getting started](getting-started.md)
for the extension side and the [command reference](#commands) below for every
flag.

## Status

The CLI is currently **0.7.0**.  It is versioned separately from the
extension.  It writes **v1** (FNV-1a 32-bit) by default and **v2** with
`capture --hash fnv64`, `crc32c` or `xxh3`.

## Commands

```text
pg_integrity_checksums init      [options]
pg_integrity_checksums config    validate|show [options]
pg_integrity_checksums capture   [--config FILE] [--profile NAME] [options]
pg_integrity_checksums verify    [--config FILE] [--profile NAME] [options]
pg_integrity_checksums diff      OLD.json NEW.json [options]
pg_integrity_checksums compare   --source DSN --target DSN [options]
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

### Encrypted connections

The CLI passes the connection string to the PostgreSQL driver, so TLS is
configured with the usual `sslmode`, `sslrootcert`, `sslcert` and `sslkey`
parameters (or the `PGSSL*` variables).  The default `sslmode=prefer` and
`require` encrypt the traffic but do not check the server certificate, and
`prefer` falls back to plaintext, so a network attacker can intercept or alter
what the CLI reads.  For a remote server use `sslmode=verify-full`:

```text
postgresql://user@db.example.com/app?sslmode=verify-full&sslrootcert=/etc/ssl/db-ca.pem
```

When a command connects to a remote host (not a Unix socket, `localhost` or a
loopback address) without `verify-ca` or `verify-full`, the CLI prints a
warning to stderr.  Set `PGICS_NO_SSL_WARNING=1` to silence it, for example on
a trusted private network.

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
| `--hash` | `fnv32` | `fnv32` (v1), or `fnv64`, `crc32c`, `xxh3` (v2) |
| `--schema` | all non-system | Schemas to include; globs allowed |
| `--exclude-schema` | | Schemas to exclude; globs allowed |
| `--table` | all | `schema.table` or table glob to include |
| `--exclude-table` | | `schema.table` or table glob to exclude |
| `--no-indexes` | false | Do not discover indexes |
| `--no-sequences` | false | Do not discover sequences |
| `--include-system` | false | Include system schemas |
| `--workers` | 0 | Extension parallel workers (GUC) |
| `--timeout` | 0 | `statement_timeout`, e.g. `5m` |
| `--strict` | false | Fail if an object cannot be checksummed (the CLI's own preflight; unrelated to the server setting `pg_integrity_checksums.strict`) |
| `--quiet` | false | No progress on stderr |

`--kind physical` is opt-in.  A physical baseline is tied to one cluster;
it records `system_identifier` and cannot be used against another cluster.
Physical baselines contain no sequences.

Sequences need extension 2.9.0 or later.  With an older extension, a capture
that did not ask for sequences explicitly (with `--no-sequences=false` or
`include_sequences: true` in the profile) warns and writes a baseline
without them (`include_sequences: false`).

`--hash fnv64`, `crc32c` or `xxh3` writes a **v2** baseline.  The default
`--hash fnv32` keeps writing the historical **v1** format.  `crc32c` and
`xxh3` need extension 2.9 or later; `capture` fails before the scan on an
older extension.  For choosing one see
[Which API should I use?](concepts.md#generic-api-choose-the-algorithm).  `verify` and `update` inherit the
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

### `compare`

Compares the tables of two live databases row by row, for example a primary
and a logical replica, or a database and its restored dump.  It needs
extension 2.8.0 or later on both servers.  Table data is not copied to the
client: for each table present on both sides the CLI compares
`pg_table_bucket_checksums` per bucket of the key hash, then fetches
`pg_table_row_checksums` for the differing buckets only and reports each
missing (source only), extra (target only) or changed row by key.

```bash
pg_integrity_checksums compare \
  --source "postgresql://primary/app" --target "postgresql://replica/app" \
  --schema public --workers 4
```

```text
DIFFERENT      public.orders source_rows=200000 target_rows=200000 differing_buckets=3/3125
  CHANGED  (777)
  EXTRA    (300000)
  MISSING  (12345)
ONLY_IN_TARGET public.audit_tmp
tables=2 equal=0 different=1 only_in_source=0 only_in_target=1 schema_differs=0 errors=0 missing_rows=1 extra_rows=1 changed_rows=1
```

- Tables (ordinary, partitioned and materialized views; partitions through
  their parent) are matched by `schema.table`; `--schema`, `--exclude-schema`,
  `--table`, `--exclude-table` and `--include-system` select them as in
  `capture`.
- The key is the source table's primary key, or the whole row when it has
  none (a changed row then shows up as a missing and an extra row).
  `--key schema.table=col1,col2` sets it per table; `--key schema.table=`
  selects the whole row.
- A table whose columns (names and types, in order) differ is reported as
  `schema_differs` and not compared.  An error in one table (missing
  privilege, row-level security, unknown key column) is reported as `error`
  and the other tables are still compared.
- `--buckets` is the number of buckets per table; the default (0) aims at
  about 64 rows per bucket from the planner estimate.  Rows are listed when
  the differing buckets of a table hold at most `--max-rows` rows (default
  10000); otherwise only the bucket counts are reported.
- Each side is read in one repeatable-read, read-only transaction, but the
  two snapshots are taken independently: compare databases that are not
  being written to, or expect rows in flight to show up.
- Checksums use FNV-1a 64-bit.  `--workers` sets
  `pg_integrity_checksums.workers` on both servers, which parallelizes the
  bucket scans.  `--timeout`, `--connect-timeout` and `--application-name`
  apply to both connections.

`--format` accepts `text` or `json`; `--show-ok` lists equal tables in the
text report.  Exit code is `1` when anything differs (including tables only
on one side, schema differences and per-table errors) unless `--no-fail` is
given, `0` when the databases match and `2` for operational errors.

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
`capture`.  `--allow-version-mismatch` lets a baseline captured on another
PostgreSQL major be refreshed with its scope kept.

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

See [Container image](#container-image).  To build the image yourself:

```bash
docker build -t pgics-cli ./cli
docker run --rm -e DATABASE_URL -v "$PWD:/work" -w /work pgics-cli verify --baseline baseline.json
```

### GitHub Action

The composite action in `cli/action.yml` builds the CLI from the referenced
tag and runs `verify`.  The job needs a reachable database with the extension
(for example a service container) and Go is set up by the action itself:

```yaml
- uses: VictorNagibator/pg_integrity_checksums/cli@cli-v0.7.0
  with:
    baseline: baselines/prod.json
    dsn: ${{ secrets.DATABASE_URL }}
    fail_on: changed
    format: junit
    output: pgics-report.xml
```

| Input | Default | Meaning |
|---|---|---|
| `baseline` | (required) | Path to the JSON baseline |
| `dsn` | `DATABASE_URL` from the environment | Connection string |
| `format` | `json` | Report format: `text`, `json` or `junit` |
| `fail_on` | `any` | What fails the step: `changed`, `schema-changed`, `new`, `missing`, `any` or `none` |
| `changed_since` | `false` | `true` passes `--changed-since` |
| `output` | (stdout) | Write the report to this file |
| `quiet` | `true` | Suppress progress output |

The step fails with the CLI's [exit code](#exit-codes).  Pin the action to a
release tag; `main` may contain unreleased changes.

### Example: verify a restored backup

A dump that restores without errors can still be incomplete or wrong.  This
workflow restores a `pg_dump` archive into a fresh PostgreSQL service
container and verifies it against a baseline captured from the source
database when the dump was taken.  It runs every Monday and on demand, and
keeps the JUnit report as an artifact.

```yaml
name: Verify restored backup

on:
  schedule:
    - cron: "0 4 * * 1"
  workflow_dispatch:

permissions:
  contents: read

jobs:
  verify:
    runs-on: ubuntu-latest
    services:
      postgres:
        # Same PostgreSQL major and extension version as the source.
        image: ghcr.io/victornagibator/pg_integrity_checksums:pg16-v2.9.1
        env:
          POSTGRES_PASSWORD: postgres
          POSTGRES_DB: app  # same name as the source database
        ports:
          - 5432:5432
        options: >-
          --health-cmd "pg_isready -U postgres -d app"
          --health-interval 5s
          --health-timeout 5s
          --health-retries 20
    steps:
      - uses: actions/checkout@v7

      # Here the archive and its baseline are in the repository.  In a real
      # setup, download the latest ones from your backup storage instead.
      - name: Restore the backup
        run: |
          docker exec -i "${{ job.services.postgres.id }}" \
            pg_restore -U postgres -d app --no-owner --no-acl --exit-on-error \
            < backup/app.dump

      - name: Verify against the baseline
        uses: VictorNagibator/pg_integrity_checksums/cli@cli-v0.7.0
        with:
          baseline: backup/app.baseline.json
          dsn: postgresql://postgres:postgres@localhost:5432/app
          format: junit
          output: pgics-report.xml

      - name: Upload the report
        if: always()
        uses: actions/upload-artifact@v7
        with:
          name: pgics-report
          path: pgics-report.xml
```

- `pg_restore` runs inside the service container, so its version matches the
  server.  The image has already created the extension in `POSTGRES_DB`; the
  `CREATE EXTENSION IF NOT EXISTS` in the dump leaves it as it is.
  `--no-owner --no-acl` skips the source's roles, which do not exist in the
  container.
- `verify` rejects a baseline from another PostgreSQL major (see
  `--allow-version-mismatch`), and warns when the extension version or the
  database name differ; pick the image tag and `POSTGRES_DB` to match the
  source.
- The report has one test case per table, index and sequence, and a
  `<failure>` for each object whose data or definition changed, that is new
  or that is missing, so any JUnit viewer shows what is wrong.  The step fails with exit code `1` in that
  case, and the report is still uploaded.

The archive and the baseline must describe the same state of the source.
`pg_dump` and `capture` each read their own snapshot, so take both while
nothing writes to the database, for example on a standby with replay paused:

```bash
psql -h standby -d app -c "SELECT pg_wal_replay_pause()"
pg_dump -h standby -d app -Fc -f backup/app.dump
pg_integrity_checksums capture --db "postgresql://standby/app" \
  --hash fnv64 --output backup/app.baseline.json
psql -h standby -d app -c "SELECT pg_wal_replay_resume()"
```

## Exit codes

| Code | Meaning |
|---:|---|
| 0 | Success / no blocking differences |
| 1 | Differences found (according to `--fail-on`), or diff/compare found differences |
| 2 | Operational error: connection, SQL, permissions, corrupt page |
| 3 | Invalid baseline: format, JSON, kind or incompatible version |
| 4 | `update`/destructive operation refused without `--yes` |

## Baseline formats

### v1 — FNV-1a 32-bit

The v1 format identifier is:

```text
pg_integrity_checksums.baseline/v1
```

v1 has no `hash` field and uses the default numeric FNV-1a 32-bit
functions.  The CLI continues to read and write it by default.

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
    "include_indexes": true,
    "include_sequences": true
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

The supported values are `fnv32`, `fnv64`, `crc32c` and `xxh3`.  The CLI
writes a v2 baseline when `capture --hash` names anything but `fnv32`.  Older
CLI releases reject v2 instead of silently comparing incompatible digests;
releases before 0.7.0 also reject v2 baselines with `crc32c` or `xxh3`.  The current CLI reads
both v1 and v2 baselines.

Rules:

- Checksums are **JSON strings**, because SQL `bigint` values do not fit
  safely into JavaScript numbers.
- `kind` is `logical` or `physical`.
- Object `kind` is `table` or `index`.
- OIDs are never stored; identity is schema-qualified names.
- `schema_hash` is a SHA-256 of the portable object definition (columns,
  constraints, partition key, reloptions and index definition). It lets
  verify/diff report `schema_changed` separately from a data change.  It is
  computed with `search_path = pg_catalog` and `quote_all_identifiers = off`
  and with constraints ordered by name, so it does not depend on the
  connection's settings and survives a dump/restore.
- `schema_hash_version` records how `schema_hash` was computed (absent means
  1, the scheme of earlier CLI versions, which depended on `search_path` and on constraint
  creation order).  Hashes of different versions are not compared: verify and
  diff then report data changes only and print a warning; `update --yes`
  re-captures the baseline with current hashes.
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

Sequences are the exception: they are not transactional, so their state is
read when each one is checksummed, not as of the snapshot.  A `nextval()`
during a capture can therefore make a sequence look ahead of the tables.

A long repeatable-read transaction holds an `xmin` horizon and can delay
`VACUUM`.  Keep capture/verify windows bounded and monitor long-running
sessions on busy databases.

### Object scope

By default the CLI discovers:

- ordinary tables (`r`), partitioned tables (`p`) and materialized views (`m`);
- top-level relations only: partition leaves are not duplicated;
- indexes (`i`) and partitioned indexes (`I`) when indexes are enabled;
- sequences (`S`) in logical baselines, unless `--no-sequences` is given;
- non-system schemas (`pg_catalog`, `information_schema`, `pg_toast` and
  temporary schemas are excluded).

A partitioned table/index is checksummed through its parent, which aggregates
its leaves.  `--include-system` includes system schemas.  Foreign tables,
views and TOAST relations are not captured as separate objects.

Sequences are checksummed with `pg_sequence_logical_checksum`: their
definition and `last_value`/`is_called`.  This catches the classic migration
gap of copied rows with a sequence left at its start value.  A change of the
definition (`ALTER SEQUENCE ... INCREMENT`, data type, ...) is reported as
`schema_changed`; a new `last_value` as `changed`.  A sequence owned by a
column (`serial`, identity, `OWNED BY`) records its table in `table_schema`
and `table_name`, and `--table`/`--exclude-table` apply to that table as they
do for indexes; other sequences match by their own name.  Sequences on a
physical standby are ahead of the primary (values are pre-logged to WAL), so
verifying a standby against a primary's baseline reports them as changed;
capture with `--no-sequences` for that use.

Baselines written before sequences were supported have no
`include_sequences` field: `verify` and `update` read it as false and do not
report the database's sequences as new.  Capture a new baseline to cover
them.  Older CLI versions reject baselines that contain sequences.

### Logical vs physical

`logical` is the default:

- stable across VACUUM/REINDEX/physical movement;
- suitable for migrations and cross-database comparison;
- excludes relation OIDs, heap TIDs and type OIDs.

`physical` is opt-in:

- depends on storage, tuple locations, relfilenodes and architecture;
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

The counters are read just before the snapshot transaction starts, so the
counters stored with a checksum never include a change that the checksum
does not.  PostgreSQL publishes the counters with a short delay (about a
second on PostgreSQL 15 and later), so a change committed in the last moments
before a `--changed-since` verify may only be noticed by the next one; it is
never missed for good.

The window therefore works in one direction: a verify run immediately after a
commit can still reuse the old checksum of that table and report it as
unchanged, because its counters have not been published yet.  The change is
reported by a later verify (or at once by a verify without `--changed-since`).
Do not rely on `--changed-since` to see changes made in the last few seconds.

On a standby (`pg_is_in_recovery()`), `--changed-since` is ignored with a
warning and every object is checksummed: WAL replay does not update the table
statistics, so unchanged counters there say nothing about the data.

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
PGICS_TEST_DSN="postgresql:///app?host=/tmp" \
  go test ./internal/cli -run TestIntegration -v
```

It creates and drops a temporary schema, so use a test database.
