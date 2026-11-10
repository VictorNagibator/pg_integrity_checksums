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
  relation's relfilenode/reltablespace; index physical checksums hash raw
  index pages, including page LSNs.  Use physical checksums for on-disk
  corruption within one cluster, not for comparing replicas.
* **Logical index checksums describe expected content, not stored pages.**
  They are derived from visible table rows and the index definition, so a
  missing or corrupt index entry is not detected by this value.  Use
  `pg_index_physical_checksum` for actual index storage.
* **Coverage gaps.** The database physical checksum does not include TOAST
  content by default, and never includes sequences, large objects (unless
  `include_system = true`), FSM/VM forks or relation free space.  Foreign
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

### Strict mode

Database checksum functions normally skip a relation that cannot be
checksummed (for example an invalid index, a foreign partition, or a relation
that fails during the scan).  Tables without a primary key are supported, and
logical index checksums work for every access method.  Physical index
checksums cover B-tree, Hash, GiST, GIN, SP-GiST and BRIN; other physical
index AMs are skipped.  Set `pg_integrity_checksums.strict = on` to turn an
actual skip into an error instead:

```sql
SET pg_integrity_checksums.strict = on;
SELECT pg_database_logical_checksum(false, true);
-- ERROR: relation "..." cannot be checksummed
-- DETAIL: The relation has no usable checksum.
```

The default is `off`.  In strict mode a skipped relation aborts the whole
database checksum, which is useful for monitoring where a partial result
would be worse than no result.

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

```sql
-- Compare source and replica logical content.
-- Logical checksums exclude relation OIDs, heap TIDs and type OIDs, so they
-- are the appropriate values to compare when schema/type names and row
-- content match.  Physical checksums include page LSNs, tuple locations and
-- other storage details, so they will normally differ on a standby.
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
(new page or a genuine zero).  Use it as a calculator, not as a standalone
corruption verdict; `data_checksums = on` is required for stored page
checksums to be meaningful.

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

CI runs regression and TAP tests for PostgreSQL 12–19 on:

- Linux x86_64 and arm64
- macOS arm64
- Windows x86_64
- FreeBSD amd64 and arm64
- Linux s390x (big-endian, manually triggered [Big-endian workflow](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/.github/workflows/big-endian.yml))

Logical checksums use a canonical little-endian encoding for the few fixed-width
integers in the canonical form (array shape, composite field count and
multirange count).  They are therefore identical on little-endian and
big-endian systems.  Physical checksums remain storage- and
architecture-dependent by design; use logical checksums for cross-platform
comparisons.

---
