# Operations

This page covers correctness, performance and production limitations.  For an
introduction, read [Getting started](getting-started.md) and
[Concepts](concepts.md).

## Limitations and Portability

These limitations are documented rather than silently ignored.

* **Portability is scoped.** Logical checksums exclude relation OIDs, heap
  TIDs and type OIDs.  Built-in types, enums, numeric, arrays, composites,
  domains and ranges use a canonical encoding.  Cross-database comparison is
  intended for the same PostgreSQL major version and compatible schema/type
  names; custom types whose send/output representation changes between
  versions may still produce different values.
* **Custom types are type-defined.** A user-defined type is hashed through its
  binary send function, or its output function when no send function exists.
  If that representation differs between databases, the checksum differs.
* **Endianness.** Logical checksums use a canonical little-endian encoding for
  the fixed-width integers in arrays, composites and multiranges, and are
  therefore identical on little-endian and big-endian systems.  Physical
  checksums remain architecture- and storage-dependent.
* **Physical checksums are not replica fingerprints.** Tuple/table physical
  checksums depend on tuple locations, MVCC fields, tuple bytes and the
  relation's relfilenode/reltablespace; index physical checksums hash the
  index pages (with the page LSN, the stored page checksum and hint bits
  masked out).  Tuple and table physical checksums also change when a row is
  locked (`SELECT ... FOR UPDATE`/`FOR SHARE`, foreign key checks), because
  the lock is stored in the tuple's `xmax`.  Use physical checksums for
  on-disk corruption within one cluster, not for comparing replicas.
* **Text values are hashed in the server encoding.** The text types (`text`,
  `varchar`, `char(n)`, `name`, and the other types whose send function
  converts text, such as `json`) are hashed as stored, whatever the session's
  `client_encoding` is, so logical checksums do not depend on it.  They do
  depend on the server (database) encoding: the same text in databases with
  different encodings hashes differently.  An `xml` value is hashed without
  an `encoding` attribute in its XML declaration, whatever the client and
  server encodings are.
* **Logical index checksums describe expected content, not stored pages.**
  They are derived from visible table rows and the index definition, so a
  missing or corrupt index entry is not detected by this value.  Use
  `pg_index_physical_checksum` for actual index storage.
* **Coverage gaps.** The database physical checksum does not include TOAST
  content by default, and never includes sequences, large objects (unless
  `include_system = true`), FSM/VM forks or relation free space.  The
  database logical checksum does not include sequences either; check them
  with `pg_sequence_logical_checksum` or the CLI, which captures them.  Foreign
  table partitions are skipped by partitioned table checksums because they
  have no local storage in this database.
* **Non-cryptographic hashes.** The default SQL API uses 32-bit FNV-1a;
  generic overloads support 32-bit and 64-bit FNV-1a.  PostgreSQL page
  checksums are 16-bit.  None of these are cryptographic hashes or provide
  tamper evidence.
* **Collision probability.** A random accidental change is missed with
  probability about `2^-32` (`2.3e-10`) by a 32-bit checksum and `2^-64`
  (`5.4e-20`) by a 64-bit checksum.  These are rough estimates, not a
  cryptographic security claim.
* **Index expressions run as the table owner.** Logical index checksums
  (and the database logical checksum, which includes indexes by default)
  evaluate index expressions and partial-index predicates.  Like `REINDEX`
  and `ANALYZE`, they do so as the owner of the table in a
  security-restricted operation, so a function in an index definition never
  runs with the privileges of the role computing the checksum.  A function
  that the owner itself cannot execute, or one that does something a
  security-restricted operation forbids, makes the checksum fail.
* **RLS and column privileges.** Relations protected by row-level security are
  rejected because a direct heap scan cannot apply policies.  Table-level
  `SELECT` is required; column-only privileges are not sufficient.
* **Foreign tables.** Foreign partitions/tables have no local storage and are
  skipped by partitioned checksums.

---

## Parallel Computation

Table, index and database checksums can be computed in parallel using
PostgreSQL's parallel worker infrastructure. Each worker scans a subset of the
relation's blocks (or a subset of the database's relations) using a shared,
self-balancing atomic counter, and the partial results are combined by the
leader.  Logical index checksums parallelize the heap scan and evaluate the
index definition in each worker.

An index whose expressions or partial-index predicate call a function that is
not `PARALLEL SAFE` (a user-defined function is `PARALLEL UNSAFE` unless
declared otherwise) cannot be evaluated in parallel mode.  Its logical
checksum is then computed without workers, and a parallel database checksum
computes such indexes in the leader after the parallel part.  The values are
the same either way; mark the functions `PARALLEL SAFE` where that is true to
let them use workers.

### Configuration

The degree of parallelism is controlled by the `pg_integrity_checksums.workers` GUC:

| Setting                          | Behavior                                   |
| -------------------------------- | ------------------------------------------ |
| `pg_integrity_checksums.workers = 0`       | Disabled (default, single-backend)         |
| `pg_integrity_checksums.workers = N`       | Use up to `N` parallel workers             |

The actual number of workers is clamped by `max_parallel_workers` and by the
number of blocks (or relations) being scanned. Parallelism is also skipped
when the function is already running inside a parallel worker, since
parallel workers cannot be nested.

```sql
-- Compute a table checksum with 4 parallel workers
SET pg_integrity_checksums.workers = 4;
SELECT pg_table_physical_checksum('large_table'::regclass, false);

-- Disable parallelism (restore the default)
RESET pg_integrity_checksums.workers;
```

### Database checksums and parallelism

A serial logical database checksum reads each table once for the table and
all of its indexes.  A parallel one gives whole relations to the workers, so a
table and each of its indexes may be read by different workers.  With the
data in memory the parallel scan finishes sooner; when the scan is bound by
reading a few large tables from disk, the serial scan reads less.  Table and
index checksums split the blocks of a single relation across workers and
benefit from parallelism in both cases.

### Strict mode

Database checksum functions are **strict by default**
(`pg_integrity_checksums.strict = on`): if a relation in scope cannot be
checksummed, the whole call fails.  This is deliberate.  A partial result is
worse than no result for monitoring, because a skipped corrupt table would
otherwise look like "nothing changed".

| Situation | `strict = on` (default) | `strict = off` |
| --- | --- | --- |
| Invalid index, **logical** checksum | error: `relation "..." cannot be checksummed` | skipped, counted in the summary `WARNING` |
| Invalid index, **physical** checksum | included (its pages can still be read) | included |
| Index with an access method that has no physical checksum (for example a third-party AM) | error: `relation "..." cannot be checksummed` | skipped, counted in the summary `WARNING` |
| Corrupt page, unreadable file | the original error, with a `CONTEXT` line naming the relation | skipped, `WARNING` per relation plus the summary `WARNING` |
| Relation dropped while the scan runs | left out with a `NOTICE` (it is no longer part of the database) | left out with a `NOTICE` |
| Unlogged table on a standby (it has no data there) | error naming the table | skipped with a `WARNING` |
| Query cancellation, `statement_timeout`, out of memory, shutdown | always propagated | always propagated |

An error looks like this:

```sql
SELECT pg_database_logical_checksum(false, true);
-- ERROR: relation "orders_old_idx" cannot be checksummed
-- DETAIL: The index is invalid and has no content to checksum.
-- HINT: Set pg_integrity_checksums.strict = false to skip relations without
--       a usable checksum.
```

or, for a failure while reading the relation:

```
ERROR:  invalid page in block 0 of relation base/5/16414
CONTEXT:  while computing the database checksum of relation "orders" (OID 16414)
```

Tables without a primary key are supported, and logical index checksums work
for every access method.  Physical index checksums cover B-tree, Hash, GiST,
GIN, SP-GiST and BRIN.

For ad-hoc use you can allow skipping, per session or persistently:

```sql
SET pg_integrity_checksums.strict = off;
SELECT pg_database_logical_checksum(false, true);
-- WARNING: skipping relation 16438 while computing database checksum
-- WARNING: database checksum skipped 1 relation(s)
-- HINT: Set pg_integrity_checksums.strict = on to fail with the reason ...

ALTER DATABASE appdb SET pg_integrity_checksums.strict = off;  -- persistent
```

With strict mode off, a skipped relation is excluded from the result and is
only reported as a warning (in parallel workers only in the server log), so do
not use that value for integrity monitoring.  Before 2.5.0 the default was
`off`.  The CLI's `--strict` flag is separate from this setting; it controls
the CLI's own preflight, not the server GUC.

### Investigating a mismatch

A database or table checksum differs from the baseline.  Work from the outside
in:

1. **Physical or logical?**  A physical checksum also changes after
   `VACUUM FULL`, `CLUSTER`, `REINDEX` or moving a table to another
   tablespace; that is expected.  A logical difference means the data (or a
   column or type name) changed.
2. **Freeze the view of the data.**  Run the comparison in one transaction, so
   that every call sees the same snapshot:

   ```sql
   BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY;
   ```

3. **Find the table.**  Compare per-table logical checksums on both sides (or
   against the baseline) and diff the two lists:

   ```sql
   SELECT c.oid::regclass AS tbl, pg_table_logical_checksum(c.oid) AS checksum
   FROM pg_class c
   WHERE c.relkind IN ('r', 'm') AND c.relnamespace = 'public'::regnamespace
   ORDER BY 1;
   ```

   The CLI does this for you: `pg_integrity_checksums verify` reports each
   object that differs.
4. **Find the rows.**  Compare logical checksums per key bucket, then the rows
   of the buckets that differ (see
   [Locating differing rows](#7-locating-differing-rows-between-two-servers)):

   ```sql
   SELECT bucket, row_count, checksum
   FROM pg_table_bucket_checksums('orders', 1024);

   SELECT key, row_checksum
   FROM pg_table_row_checksums('orders', 1024, '{17,512}');
   ```

   The CLI does both steps for every table of two databases:
   `pg_integrity_checksums compare --source DSN --target DSN`.

5. **If the data is identical but the storage is suspect**, verify the pages
   against their stored checksums (needs `data_checksums = on`).  The check
   reads the on-disk copy of each page, so run `CHECKPOINT` first; pages that
   have not been written yet are reported as `new`:

   ```sql
   SELECT fork, blkno, stored_checksum, calculated_checksum
   FROM pg_relation_page_checksum_status('orders');
   ```

   No rows means every page of every fork matches its stored checksum.

6. **If a database call fails instead of returning a value**, see
   [`relation "..." cannot be checksummed`](troubleshooting.md).

### Correctness

Because aggregation is order- and partition-independent, the parallel result
is always identical to the serial result for the same data state, regardless
of the number of workers. The same consistent snapshot is propagated to all
workers, so every tuple is checksummed as of the same point in time.

### Snapshot consistency across calls

One SQL statement sees one snapshot.  A single `pg_table_logical_checksum` or
`pg_database_logical_checksum` call is therefore internally consistent, but two
separate calls under the default `READ COMMITTED` isolation can see different
points in time.  A table and its index, or several tables, should be checked
inside one transaction when the comparison matters:

```sql
BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY;
SELECT pg_table_logical_checksum('orders'::regclass);
SELECT pg_table_logical_checksum('customers'::regclass);
COMMIT;
```

The CLI already does this for capture and verify.  A long repeatable-read
transaction keeps an `xmin` horizon and can delay `VACUUM`; keep checksum
transactions as short as the workload allows and monitor long-running sessions.

In a `SERIALIZABLE` transaction a checksum is a read like any other: a table,
index or database checksum takes a relation-level SIREAD lock on every table
it reads (as a sequential scan does), and a tuple or cell checksum takes a
page-level one.  A concurrent write to what the checksum read can therefore
make one of the transactions fail with a serialization error, which the
application retries as usual.  A `READ ONLY DEFERRABLE` transaction avoids
this.

---

## Practical Applications

### 1. Data Integrity Monitoring

```sql
-- Create checksum monitoring system
CREATE TABLE checksum_audit_log (
    audit_time timestamptz DEFAULT now(),
    table_name text,
    checksum_type text,  -- 'physical' or 'logical'
    checksum_value bigint,
    PRIMARY KEY (table_name, checksum_type, audit_time)
);

-- Periodic monitoring of critical tables
INSERT INTO checksum_audit_log (table_name, checksum_type, checksum_value)
SELECT 
    'transactions',
    'physical',
    pg_table_physical_checksum('transactions'::regclass, false)
UNION ALL
SELECT 
    'transactions',
    'logical',
    pg_table_logical_checksum('transactions'::regclass);
```

### 2. Replication Consistency Validation

For a logical replica, the step-by-step procedure (prerequisites, waiting for
the subscriber to catch up, finding the differing rows) is in
[Verifying a logical replica](logical-replica.md).

```sql
-- Compare source and replica logical content.
-- Logical checksums exclude relation OIDs, heap TIDs and type OIDs, so they
-- are the appropriate values to compare when schema/type names and row
-- content match.  Physical checksums depend on tuple locations and other
-- storage details and are not meant for comparing two servers.
-- Run on source and replica and compare logical_cs only:
SELECT
    c.relname,
    pg_table_logical_checksum(c.oid) AS logical_cs,
    pg_table_physical_checksum(c.oid, false) AS physical_cs
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = 'public'
  AND c.relkind IN ('r', 'p', 'm')
ORDER BY c.relname;
```

### 3. Data Migration Verification

```sql
-- Verify migration integrity.  Logical checksums omit relation OIDs and heap
-- TIDs, so values are comparable between source and target databases.
-- Run this query on both databases and compare the two result sets.
WITH table_checksums AS (
    SELECT
        c.relname,
        pg_table_logical_checksum(c.oid) AS checksum
    FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'public'
      AND c.relkind IN ('r', 'p', 'm')
)
SELECT * FROM table_checksums ORDER BY relname;
```

### 4. Compare Against a Known-Good Value

A checksum is useful only when compared with a value recorded at a known-good
point.  The audit table from the first example provides that history:

```sql
WITH expected AS (
    SELECT checksum_value
    FROM checksum_audit_log
    WHERE table_name = 'transactions'
      AND checksum_type = 'logical'
    ORDER BY audit_time DESC
    LIMIT 1
)
SELECT
    pg_table_logical_checksum('transactions'::regclass) AS current_value,
    expected.checksum_value AS expected_value,
    pg_table_logical_checksum('transactions'::regclass) = expected.checksum_value AS matches
FROM expected;
```

`pg_page_checksum` only calculates PostgreSQL's 16-bit page checksum.  It does
not compare the value with the stored `pd_checksum`, and `0` is ambiguous
(new page or a genuine zero).  Use `pg_page_checksum_status` when you want a
verification result.  `data_checksums = on` is required for stored page
checksums to be meaningful; without it the status is `checksums_disabled`.

### 5. Batch Change Auditing

Record checksums after a batch, ETL run or maintenance window.  Do not compute a
whole-table checksum in a per-row trigger: that makes every write O(table) and
would flag every legitimate change as suspicious.

```sql
-- Record the state once, after the batch has finished.
INSERT INTO checksum_audit_log (table_name, checksum_type, checksum_value)
SELECT 'transactions', 'logical',
       pg_table_logical_checksum('transactions'::regclass)
UNION ALL
SELECT 'transactions', 'physical',
       pg_table_physical_checksum('transactions'::regclass, false);
```

Compare later runs with the most recent known-good row, for example with the
query in section 4.

### 6. Restore Validation

Record a checksum before the backup, then compare the restored table with that
recorded value:

```sql
-- Before backup: record a known-good logical checksum.
INSERT INTO checksum_audit_log (table_name, checksum_type, checksum_value)
VALUES ('transactions', 'logical',
        pg_table_logical_checksum('transactions'::regclass));

-- After restore: compare with the latest recorded value.
WITH expected AS (
    SELECT checksum_value
    FROM checksum_audit_log
    WHERE table_name = 'transactions'
      AND checksum_type = 'logical'
    ORDER BY audit_time DESC
    LIMIT 1
)
SELECT
    pg_table_logical_checksum('transactions'::regclass) = expected.checksum_value AS restored_ok
FROM expected;
```

A database-level checksum is also available when the recorded value was
produced by `pg_database_logical_checksum`; do not compare a table checksum
with a database checksum.

---

## Platform Support

CI runs regression and TAP tests for PostgreSQL 12–19 on (see
[Continuous integration](testing.md#continuous-integration)):

- Linux x86_64 and arm64
- macOS arm64
- Windows x86_64
- FreeBSD amd64 and arm64
- Linux s390x (big-endian, PostgreSQL 16, weekly [Big-endian workflow](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/.github/workflows/big-endian.yml))

Logical checksums use a canonical little-endian encoding for the few fixed-width
integers in the canonical form (array shape, composite field count and
multirange count).  They are therefore identical on little-endian and
big-endian systems.  Physical checksums remain storage- and
architecture-dependent by design; use logical checksums for cross-platform
comparisons.

---

### 7. Locating differing rows between two servers

Two copies of `orders` (a logical replica, a restored dump, a migration
target) have different logical checksums.  Find the rows without copying the
table, in two passes over each side.  The example uses
[`dblink`](https://www.postgresql.org/docs/current/dblink.html) to query the
other server from one session; running the queries on each side and comparing
the results elsewhere works the same way.

1. Compare the bucket checksums.  Use the same number of buckets and the same
   key on both sides; aim for a few hundred to a few thousand rows per bucket.

    ```sql
    CREATE EXTENSION IF NOT EXISTS dblink;

    CREATE TEMP TABLE differing AS
    SELECT l.bucket, l.row_count AS local_rows, r.row_count AS remote_rows
    FROM pg_table_bucket_checksums('orders', 1024) l
    JOIN dblink('host=replica dbname=app',
                $$SELECT bucket, row_count, checksum
                  FROM pg_table_bucket_checksums('orders', 1024)$$)
         AS r(bucket integer, row_count bigint, checksum bytea)
      USING (bucket)
    WHERE l.checksum <> r.checksum;
    ```

2. Compare the rows of those buckets by key hash:

    ```sql
    WITH sel AS (SELECT array_agg(bucket) AS b FROM differing),
    l AS (SELECT r.* FROM sel, pg_table_row_checksums('orders', 1024, sel.b) r),
    r AS (SELECT x.* FROM sel, dblink('host=replica dbname=app',
            format('SELECT key_hash, key, row_checksum
                      FROM pg_table_row_checksums(%L, 1024, %L)',
                   'orders', sel.b))
            AS x(key_hash bytea, key text, row_checksum bytea))
    SELECT coalesce(l.key, r.key) AS key,
           CASE WHEN r.key IS NULL THEN 'only here'
                WHEN l.key IS NULL THEN 'only on replica'
                ELSE 'changed' END AS difference
    FROM l FULL JOIN r USING (key_hash)
    WHERE l.row_checksum IS DISTINCT FROM r.row_checksum
    ORDER BY 1;
    ```

Every call reads the whole table, so this costs two scans per side whatever
the number of buckets; more buckets only make the second pass return fewer
rows.  Bucket `b` of 1024 is exactly buckets `16*b` to `16*b+15` of 16384, so
results taken with different bucket counts (for example stored ones) can be
related to each other.

Each call sees its own snapshot.  On a table that is being written to, run the
two passes on each side inside one `REPEATABLE READ` transaction, and expect
rows changed by replication lag to show up as differences.

For a **physical** standby, `pg_table_block_checksums` compares ranges of
heap blocks instead, without a key; see the
[function reference](functions.md#finding-differing-rows).
