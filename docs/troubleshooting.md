# Troubleshooting

## Quick triage

| Symptom | Likely cause | Fix |
|---|---|---|
| `function ... does not exist` | Extension not created, or older than the function | `CREATE EXTENSION`; check `extversion` |
| New release installed, but nothing changed | The database still runs the old extension version | [`ALTER EXTENSION ... UPDATE`](upgrading.md#how-to-upgrade) |
| Logical checksums differ, the data looks the same | Different server encoding, type names or extension version | See [below](#logical-checksums-differ-for-the-same-data) |
| `permission denied for table` | Missing `SELECT` | Grant table-level `SELECT` |
| `row-level security` error | RLS cannot be applied to a direct heap scan | Use a non-RLS role or exclude the table |
| `unknown checksum algorithm` | Typo, or an extension older than the algorithm | Use `fnv32`, `fnv64`, `crc32c` or `xxh3`; upgrade |
| `unsupported baseline format` | Old CLI reading a v2 baseline | Upgrade the CLI |
| `baseline hashes differ` | v1/FNV32 compared with v2/FNV64 | Do not mix formats |
| `physical baseline belongs to another ...` | Physical checksum used across clusters/databases | Capture a new baseline |
| Scan is slow | Parallelism disabled or statement timeout | Set `workers` and a larger timeout |

## Error messages

The messages the extension raises, by area.  `...` stands for a relation,
column or number in the actual message.  With `\set VERBOSITY verbose` in
`psql` the error also shows its SQLSTATE and source location, which helps
when reporting a bug.

### Wrong kind of relation

| Message | Cause | What to do |
|---|---|---|
| `relation "..." is an index, not a table` | An index passed to a table, tuple, cell or row-diff function | Use `pg_index_logical_checksum` or `pg_index_physical_checksum` |
| `relation "..." is a sequence, not a table` | A sequence passed to a table, tuple or cell function | Use `pg_sequence_logical_checksum` |
| `relation "..." is not a table with storage` | A view, foreign table or other relation without heap storage | Checksum the tables behind it; materialized views are supported |
| `partitioned table "..." has no storage of its own` | A partitioned table passed to a function that addresses rows or blocks of one heap: the tuple and cell functions, `pg_table_block_checksums` | Call the function for the partition (`tableoid` in `pg_table_row_checksums` names it); table, index, bucket and row checksums accept the partitioned table |
| `TOAST relation "..." is not a heap relation` | A `pg_toast` relation passed to a tuple, cell or row-diff function | Checksum the table that owns it; its logical checksums include the TOASTed values |
| `relation "..." does not use the heap table access method` | The table uses another table access method (for example a columnar one); only heap is supported | Exclude the table; in a database checksum, see [strict mode](operations.md#strict-mode) |

### Privileges

| Message | Cause | What to do |
|---|---|---|
| `permission denied for table ...` | The role has no table-level `SELECT` (column privileges are not enough) | `GRANT SELECT` on the table |
| `cannot compute checksum of relation "..." because row-level security is enabled` | The table has RLS enabled for this role; a direct heap scan cannot apply policies | Run as a superuser, a role with `BYPASSRLS` or the table owner (unless the table has `FORCE ROW LEVEL SECURITY`), or exclude the table |
| `permission denied for function pg_database_...` | Database-level functions are not executable by `PUBLIC` | Run them as a superuser |
| `must be superuser to compute database checksum` | `EXECUTE` on a database function was granted to a non-superuser; the function still requires superuser | Run it as a superuser; for a non-superuser, use the table and index functions or the CLI |

### Database checksums

| Message | Cause | What to do |
|---|---|---|
| `relation "..." cannot be checksummed` | Strict mode: a relation has no usable checksum (invalid index, unsupported access method, ...) | See [below](#relation-cannot-be-checksummed) |
| `WARNING: database checksum skipped ... relation(s)` | `strict = off` and some relations were left out of the result | `pg_database_logical_checksums()` (or `pg_database_physical_checksums()`) lists them with `status` and `reason` |
| `WARNING: skipping relation ... while computing database checksum` | `strict = off` and one relation failed with an error, shown in `DETAIL`; the relation is left out | Fix the error in `DETAIL`; with `strict = on` the error itself is raised, with a `CONTEXT` line naming the relation |
| `NOTICE: relation with OID ... was dropped during the database checksum and is not included` | A relation listed at the start of the scan was dropped before it was read | Nothing; the result describes the database without it.  Run again if you need a stable value |
| `only the relations argument can be NULL` | `NULL` passed for `include_system`, `include_indexes`/`include_toast` or `algorithm` of `pg_database_*_checksums` | Pass a value, or leave the argument out to use the default |

### Finding differing rows

These come from `pg_table_bucket_checksums`, `pg_table_row_checksums` and
`pg_table_block_checksums` (see
[Finding differing rows](functions.md#finding-differing-rows)).

| Message | Cause | What to do |
|---|---|---|
| `number of buckets must be between 1 and 1048576` | `buckets` is 0, negative or too large | Pick a count so that a bucket holds a few hundred to a few thousand rows |
| `number of buckets must not be NULL`, `blocks per range must not be NULL`, `checksum algorithm must not be NULL` | `NULL` passed for a required argument | Pass a value, or leave the argument out to use the default |
| `column "..." of relation "..." does not exist` | A `key_columns` entry is not a column of the table | Use column names exactly as stored (case-sensitive, without quotes) |
| `key column "..." specified more than once` | The same column appears twice in `key_columns` | List each column once |
| `key column names must not be NULL`, `bucket numbers must not be NULL` | An array argument contains `NULL` | Remove the `NULL` elements |
| `key columns must be a one-dimensional array`, `buckets must be a one-dimensional array` | A multi-dimensional array, such as `'{{a,b}}'` | Pass a flat array, such as `'{a,b}'` |
| `bucket ... is out of range for ... buckets` | `only_buckets` has a bucket number not below `buckets`, typically from a call with another bucket count | Pass the same `buckets` as the `pg_table_bucket_checksums` call that found them |
| `blocks per range must be at least 1` | `blocks_per_range` is 0 or negative | Pass a positive value |

### Corruption

| Message | Cause | What to do |
|---|---|---|
| `corrupt page header in relation "..." at block ...` | The page header fails the same sanity checks as PostgreSQL's page verification | The page is damaged.  Locate the damaged pages with `pg_relation_page_checksum_status` (see [Investigating a mismatch](operations.md#investigating-a-mismatch)), check the table with `amcheck`, and restore it from a backup |
| `corrupt line pointer in relation "..." at block ..., offset ...` | A line pointer points outside the page's tuple area | As above |
| `corrupt tuple header in relation "..." at block ..., offset ...` | A tuple is shorter than its header, or its header length is out of range | As above |

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
- **Corrupt page or missing file**: investigate with
  `pg_relation_page_checksum_status`
  (see [Investigating a mismatch](operations.md#investigating-a-mismatch)).

To skip such relations anyway, `SET pg_integrity_checksums.strict = off`; the
result then excludes them.  See [Strict mode](operations.md#strict-mode).

## Logical checksums differ for the same data

Logical checksums ignore OIDs, physical layout and dropped columns, but some
differences between two sessions or servers still change them:

- **Server encoding.**  Text values are hashed in the database's encoding,
  independently of `client_encoding`.  Compare `SHOW server_encoding;` on
  both sides (see
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

The generic API accepts `fnv32`, `fnv64`, `crc32c` and `xxh3`:

~~~sql
SELECT pg_table_logical_checksum('public.orders'::regclass, 'fnv64');
~~~

If the function exists but the algorithm is rejected, check spelling and
the installed extension version: the generic overloads with `fnv32` and
`fnv64` exist since 2.2, `crc32c` and `xxh3` since 2.9.0.

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
