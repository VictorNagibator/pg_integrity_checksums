# Troubleshooting

## Quick triage

| Symptom | Likely cause | Fix |
|---|---|---|
| `function ... does not exist` | Extension not created, or older than the function | `CREATE EXTENSION`; check `extversion` |
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
[CLI contract](cli.md#changed-since).
