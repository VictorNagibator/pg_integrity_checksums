# Upgrading

## How to upgrade

An upgrade has two parts: install the new release on the server, then update
the extension in **every database** that has it.  Installing a package or
running `make install` alone changes nothing inside a database.

1. Install the new release the same way as the old one (package, PGXN or
   `make install`; see [Installation](installation.md)).
2. In each database:

    ~~~sql
    ALTER EXTENSION pg_integrity_checksums UPDATE;

    SELECT extversion
    FROM pg_extension
    WHERE extname = 'pg_integrity_checksums';
    ~~~

    `UPDATE` without `TO` goes to the newest installed version, applying the
    upgrade scripts in between.  Upgrades never require dropping the extension.

3. New sessions load the new library.  Reconnect long-lived clients
   (connection poolers, monitoring agents) so they stop using the old one.
4. Read the notes below for every release you skip.  If an upgrade changes the
   checksum values you store, capture new baselines from data you trust.

The CLI is released separately (`cli-v*` tags) and can be upgraded on its own;
see [CLI installation](cli.md#installation).

## Which upgrades change checksum values

A stored checksum or CLI baseline only stays comparable while the values it
contains do not change.  Releases not listed here kept every value.

| Release | Values that change | What to do |
|---|---|---|
| 2.8 | Logical checksums of tables with `reg*` columns (`regclass`, `regtype`, ..., and arrays of them); of partitioned tables whose partitions declare their columns in another order; of text values hashed with a `client_encoding` other than the server encoding; physical checksums of indexes and TOAST tables whose pages hold stale bytes between `pd_lower` and `pd_upper`.  The new per-relation, bucket, row and block range functions change no values | Recompute the affected values; re-capture affected baselines |
| 2.7 | `pg_database_logical_checksum` (every database: relations are now identified by `schema.relname`); physical checksums of mapped catalogs (`pg_class`, `pg_type`, `pg_proc`, `pg_attribute`, shared catalogs) and of indexes with metapages or non-B-tree page layouts, and the database physical checksums that include them; table and index logical checksums do not change | Recompute stored database logical checksums; re-capture physical baselines |
| 2.6 | Physical checksums of indexes and TOAST relations | Re-capture physical baselines (`capture --kind physical`) |
| 2.4 | Logical checksums of tables with dropped columns or composite types with dropped attributes; user types when the baseline was taken with a non-default `search_path`; physical tuple and table checksums with `include_header = true` | Recompute the affected values and compare; recreate baselines that differ |
| 2.1 | Logical checksums on big-endian systems only (now equal to little-endian) | Recreate logical baselines taken on big-endian servers |
| 2.0.1 | Logical checksums involving Hash indexes; database logical checksums with `include_system = true` | Recreate the affected index and database baselines |

## Release notes

The complete history is in
[Changes](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/Changes).
These notes cover what to check when upgrading.

### 2.9

2.9 adds `pg_relation_page_checksum_status()`, which verifies every page of a
relation (all forks, or one) against its stored `pd_checksum` in one call,
instead of calling `pg_page_checksum_status()` for each block (see
[Page Level Functions](functions.md#page-level-functions-physical-only)).
Upgrade with:

```sql
ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.9.0';
```

What changes:

- **No checksum value changes.**
- `pg_page_checksum_status()` on a temporary table reads the page from the
  table's file, like for other relations.  It used to compare the local buffer,
  whose `pd_checksum` is only set when the page is written out, so a modified
  page of a temporary table was reported as `mismatch`; such a page is now
  `new` (not written yet) or `ok`.  Temporary tables of other sessions are
  rejected.

### 2.8

2.8 adds functions that show *where* two databases or two copies of a
table differ: `pg_database_logical_checksums()` and
`pg_database_physical_checksums()` return the checksum of every relation of a
database scan (see
[Database Level Functions](functions.md#database-level-functions-superuser-only)),
and `pg_table_bucket_checksums()`, `pg_table_row_checksums()` and
`pg_table_block_checksums()` split a logical table checksum by key bucket or
block range down to single rows (see
[Finding differing rows](functions.md#finding-differing-rows) and
[Locating differing rows between two servers](operations.md#7-locating-differing-rows-between-two-servers)).
Upgrade with:

```sql
ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.8.0';
```

What changes:

- **Logical checksums of tables with `reg*` columns change.**  `regclass`,
  `regtype`, `regproc`, `regprocedure`, `regoper`, `regoperator`,
  `regconfig`, `regdictionary`, `regnamespace`, `regrole` and `regcollation`
  values were hashed as OIDs and did not survive a dump and restore; they are
  hashed by the qualified object name now.
- **Logical checksums of some partitioned tables change**: those with a
  partition that declares its columns in another order than the parent.  Leaf
  rows are hashed in the parent's column order, matched by name.
- **Logical checksums no longer depend on `client_encoding`.**  Values
  computed with the default (equal to the server encoding) or `SQL_ASCII` do
  not change; values computed with another client encoding now match them.
- **Physical checksums of indexes and TOAST tables can change once**: the
  free space between `pd_lower` and `pd_upper` is masked, so they no longer
  change after crash recovery.  Re-capture physical baselines.
- Tables that use a table access method other than heap are rejected with an
  error instead of being misread.
- Table and index checksums work on temporary tables with
  `pg_integrity_checksums.workers > 0`, and database checksums release each
  relation's lock when done, so large databases no longer run out of lock
  table space.

The CLI 0.6 that ships alongside records `schema_hash_version 3` in new
baselines (columns are numbered among live columns, so a dropped column no
longer makes `schema_changed` appear after a dump and restore): re-capture
baselines (`update --yes`) to compare the schema again; until then `verify`
compares data only and warns.

### 2.7

2.7 is a fix and security release; it also makes logical checksums 2-5x
faster without changing their values.  Upgrade with:

```sql
ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.7.0';
```

The upgrade script changes no SQL objects; everything is in the C library, so
reconnect clients after installing the package.

What changes:

- **`pg_database_logical_checksum` values change for every database.**  The
  per-relation checksums are now combined with each relation's qualified name,
  so moving rows between two tables of the same structure changes the result.
  Renaming a relation or moving it to another schema now changes it as well.
  Table and index logical checksums, and therefore logical CLI baselines, do
  not change.
- **Some physical checksums change.**  Mapped catalogs are read through their
  actual file number, and the LP_DEAD masking of index pages is limited to
  pages that hold index tuples.  Re-capture physical baselines
  (`pg_integrity_checksums capture --kind physical`) after the upgrade.
- **Security.**  Tuple and cell checksums return `NULL` for a tuple the query
  cannot see (deleted, old or uncommitted versions) instead of a checksum of
  its data, and `pg_tuple_physical_checksum` returns `NULL` instead of `0` for
  a TID without a tuple.  Index expressions and partial-index predicates are
  evaluated as the table owner in a security-restricted operation, as
  `REINDEX` does, so a user who can create an index can no longer run SQL as
  the role computing a checksum.
- In `SERIALIZABLE` transactions the checksum functions now take predicate
  locks like a sequential scan, so write skew through a checksum is detected.
- A relation dropped while a database checksum runs is left out with a
  `NOTICE` instead of failing the whole check, in strict mode too.

### 2.6

2.6 fixes physical index and TOAST checksums that changed without any change
to the data (after a plain `SELECT` through an index, or after a server
restart with data checksums enabled).  Upgrade with:

```sql
ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.6.0';
```

What changes:

- **Physical index and TOAST checksum values differ from 2.5.**  Re-capture
  physical baselines (`pg_integrity_checksums capture --kind physical`) after
  the upgrade; a verify against an older physical baseline reports every
  index as changed.  Logical checksums and physical heap (table) checksums do
  not change, so logical baselines stay valid.
- Logical checksums no longer run type I/O functions and index expressions
  under a buffer content lock, so long calls can be cancelled with
  `statement_timeout` or `pg_cancel_backend()`.
- The pure utility functions (`pg_data_checksum`, `pg_text_checksum` and
  their algorithm overloads, `pg_checksum_bigint`) are `IMMUTABLE` and
  `PARALLEL SAFE`, so they can be used in index expressions, generated
  columns and parallel queries.  The `ALTER EXTENSION` step applies this.

### 2.5

2.5 changes one default: `pg_integrity_checksums.strict` is now `on`.  A
database checksum (`pg_database_physical_checksum`,
`pg_database_logical_checksum`) fails when a relation cannot be checksummed,
instead of skipping it with a warning.  Checksum values do not change.
Upgrade with:

```sql
ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.5.0';
```

The new default is picked up when the new library is loaded (new sessions
after the package upgrade).  Jobs that relied on the old behavior, for example
a scheduled check that ran with an invalid index left over from a failed
`CREATE INDEX CONCURRENTLY`, with a third-party index access method, or on a
standby that holds unlogged tables, will now fail.  Fix the underlying problem
(`DROP` or `REINDEX` the invalid index), or keep skipping explicitly:

```sql
ALTER ROLE monitor SET pg_integrity_checksums.strict = off;
```

See [Strict mode](operations.md#strict-mode).

### 2.4

2.4 fixes three cases where a logical or physical checksum depended on
something other than the stored data.  Upgrade with:

```sql
ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.4.0';
```

Checksum values change, and old baselines no longer match, only in these
cases:

- **Dropped columns.**  Tables with dropped columns, and composite types with
  dropped attributes, used to hash the physical attribute number.  They now
  hash the position among the live columns, so the checksum equals that of an
  identical table without the dropped columns (for example after a
  dump/restore).  Tables without dropped columns are unchanged.
- **search_path.**  A user-defined type used to be named with its schema when
  the schema was not on the session's `search_path`.  It is now always
  identified by its unqualified name.  Baselines taken from a session with the
  default `search_path` (types in `public`) are unchanged; baselines taken with
  another `search_path` must be recreated.
- **`include_header = true`.**  Physical tuple and table checksums that include
  the tuple header now ignore the xmin/xmax hint bits, which are set lazily
  by whichever session reads a tuple first.  Checksums with
  `include_header = false` (the default) are unchanged.

To check whether an existing baseline is affected, recompute it after the
upgrade and compare.  If it differs, recreate it from the current data; the
earlier value reflected the old, less stable computation.

The 2.4 upgrade also revokes `EXECUTE` from `PUBLIC` on the generic
`pg_database_*_checksum(boolean, boolean, text)` functions.  They already
required a superuser.

### 2.3

2.3 adds `pg_page_checksum_status(regclass, integer)`, which verifies a page
against the checksum stored in its header and reports `data_checksums = off`
as `checksums_disabled`.  Upgrade with:

```sql
ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.3.0';
```

The 2.3 upgrade also refreshes the `pg_cell_checksum` and `pg_page_checksum`
comments for installations coming from 2.2.0.

### 2.2

2.2 adds generic algorithm overloads for FNV-1a 32-bit and 64-bit digests.
Existing SQL functions and their 32-bit values are unchanged; PostgreSQL page
checksums remain 16-bit.  Upgrade with:

```sql
ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.2.0';
```

Older CLI releases cannot read v2 baselines created with `--hash fnv64`; the
0.4 CLI reads both v1 (fnv32) and v2 (fnv64).


## From pg_checksums 1.x

The extension was renamed in 2.0.0 to avoid confusion with PostgreSQL's core
`pg_checksums` utility.  PostgreSQL has no `ALTER EXTENSION ... RENAME`, so
migrate by reinstalling the extension and updating the GUC prefix.  See
[MIGRATION.md](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/MIGRATION.md) for the full checklist:

```sql
DROP EXTENSION pg_checksums;
CREATE EXTENSION pg_integrity_checksums;
```

```sql
-- old settings
SET pg_checksums.workers = 4;
SET pg_checksums.strict = on;

-- new settings
SET pg_integrity_checksums.workers = 4;
SET pg_integrity_checksums.strict = on;
```

SQL function names and checksum values are unchanged, so existing logical and
physical baselines remain valid.  If `DROP EXTENSION` fails because of
dependent objects, remove or recreate those dependencies explicitly rather
than using `CASCADE` without review.
