# Troubleshooting

## Quick triage

| Symptom | Likely cause | Fix |
|---|---|---|
| `function ... does not exist` | Extension not created, or older than the function | `CREATE EXTENSION`; check `extversion` |
| New release installed, but nothing changed | The database still runs the old extension version | [`ALTER EXTENSION ... UPDATE`](upgrading.md#how-to-upgrade) |
| Logical checksums differ, the data looks the same | Different `client_encoding`, type names or extension version | See [below](#logical-checksums-differ-for-the-same-data) |
| `permission denied for table` | Missing `SELECT` | Grant table-level `SELECT` |
| `row-level security` error | RLS cannot be applied to a direct heap scan | Use a non-RLS role or exclude the table |
| `unknown checksum algorithm` | Typo or extension older than 2.2 | Use `fnv32`/`fnv64`; upgrade |
| `unsupported baseline format` | Old CLI reading a v2 baseline | Upgrade the CLI |
| `baseline hashes differ` | v1/FNV32 compared with v2/FNV64 | Do not mix formats |
| `physical baseline belongs to another ...` | Physical checksum used across clusters/databases | Capture a new baseline |
| Scan is slow | Parallelism disabled or statement timeout | Set `workers` and a larger timeout |

## Extension is not installed

~~~sql
CREATE EXTENSION pg_integrity_checksums;
~~~

## Permission denied

The checksum functions require SELECT on each object.  Row-level security is
rejected because a direct heap scan cannot apply policies.  Use a role with
SELECT or restrict the scope with the CLI filters.

## `relation "..." cannot be checksummed`

Since 2.5 database checksums are strict by default: a relation without a usable
checksum aborts the call instead of being skipped.  The message and the
`CONTEXT` line name the relation; the `DETAIL` says why.  Common causes:

- **Invalid index** (for example left over from a failed
  `CREATE INDEX CONCURRENTLY`): `DROP` or `REINDEX` it.  Only logical checksums
  reject it.
- **Unsupported physical index access method** (a third-party AM such as
  Bloom): use the logical checksum, or skip it with `strict = off`.
- **Unlogged table on a standby**: the table has no data there.  Drop it on the
  primary, make it logged, or run the check on the primary.
- **Relation dropped during the scan**: retry.
- **Corrupt page or missing file**: investigate with `pg_page_checksum_status`
  (see [Investigating a mismatch](operations.md#investigating-a-mismatch)).

To skip such relations anyway, `SET pg_integrity_checksums.strict = off`; the
result then excludes them.  See [Strict mode](operations.md#strict-mode).

## Logical checksums differ for the same data

Logical checksums ignore OIDs, physical layout and dropped columns, but some
differences between two sessions or servers still change them:

- **`client_encoding`.**  Text values are hashed as their send function
  returns them, converted to the client encoding.  Compare
  `SHOW client_encoding;` in both sessions and use the same value (see
  [Limitations](operations.md#limitations-and-portability)).
- **Extension version.**  Some upgrades changed values; see
  [Which upgrades change checksum values](upgrading.md#which-upgrades-change-checksum-values).
  Compare `extversion` on both sides.
- **Type and schema names.**  Values are tagged with their type name, so a
  user-defined type with another name hashes differently.  So does a column
  of a different type (`integer` vs `bigint`, `text` vs `varchar`).
- **Visibility.**  Each call uses its own snapshot under `READ COMMITTED`.
  Concurrent writes, or a replica that has not replayed them yet, are real
  differences.
- **Physical vs logical.**  Physical checksums are expected to differ between
  servers; compare logical ones.

To narrow it down, compare table checksums first, then
`pg_tuple_logical_checksum` of matching rows, then `pg_cell_checksum` of each
column.

## Unknown checksum algorithm

The generic API accepts exactly `fnv32` and `fnv64`:

~~~sql
SELECT pg_table_logical_checksum('public.orders'::regclass, 'fnv64');
~~~

If the function exists but the algorithm is rejected, check spelling and
whether the installed extension provides the generic overloads
(`extversion >= 2.2`).

## Old CLI cannot read a v2 baseline

A v2 baseline starts with:

~~~text
pg_integrity_checksums.baseline/v2
~~~

Upgrade the CLI to 0.4 or newer.  Do not rewrite the format by hand; the hash
algorithm and checksum values are linked.

## Baselines use different hashes

`diff` and `verify` refuse to compare incompatible algorithms.  Capture both
sides with the same `--hash` setting, or migrate both to v2.

## Physical baseline rejected

A physical baseline is tied to one cluster and database.  Recreate it on the
current cluster; do not compare physical checksums across replicas.

## Slow scans

Use the extension workers and a larger timeout:

~~~bash
pg_integrity_checksums verify --workers 4 --timeout 30m --baseline baseline.json
~~~

For repeated verification of a logical baseline, see `--changed-since` in the
[CLI contract](cli.md#-changed-since).
