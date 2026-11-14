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
| 2.6 | Physical checksums of indexes and TOAST relations | Re-capture physical baselines (`capture --kind physical`) |
| 2.4 | Logical checksums of tables with dropped columns or composite types with dropped attributes; user types when the baseline was taken with a non-default `search_path`; physical tuple and table checksums with `include_header = true` | Recompute the affected values and compare; recreate baselines that differ |
| 2.1 | Logical checksums on big-endian systems only (now equal to little-endian) | Recreate logical baselines taken on big-endian servers |
| 2.0.1 | Logical checksums involving Hash indexes; database logical checksums with `include_system = true` | Recreate the affected index and database baselines |

## Release notes

The complete history is in
[Changes](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/Changes).
These notes cover what to check when upgrading.

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
