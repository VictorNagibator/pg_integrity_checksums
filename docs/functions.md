# Functions and algorithms

## Function Reference

### Page Level Functions (Physical Only)

**`pg_page_checksum(regclass, integer) -> integer`**

Computes PostgreSQL's built-in page checksum for physical page corruption detection.
The block number is validated: an out-of-range block (including `-1`, which
would otherwise be interpreted as `P_NEW`) raises an error instead of reading
or extending the relation.  The page is copied before the checksum is
computed, so the shared buffer is never modified.

```sql
-- Get checksum for page 5 of a table
SELECT pg_page_checksum('my_table'::regclass, 5);
```

### Cell Level Functions (Logical Only)

**`pg_cell_checksum(regclass, tid, integer) -> integer`**

Computes logical checksum of a cell value. NULL values return `CHECKSUM_NULL` (-1).
Requires `SELECT` on the relation; an out-of-range TID or attribute number
returns NULL.  Relations protected by row-level security are rejected.

```sql
-- Get checksum for cell (column) 3 of a specific tuple
SELECT pg_cell_checksum('employees'::regclass, '(5,2)'::tid, 3);
```

**Note**: Cell checksums are computed for individual values within specific rows, not for entire columns. To compute checksums for all values in a column, you would need to aggregate individual checksums.

### Tuple Level Functions

**`pg_tuple_physical_checksum(regclass, tid, boolean) -> integer`**

Computes physical tuple checksum based on physical location (block, offset).
Requires `SELECT` on the relation.  An invalid TID returns NULL; a corrupted
line pointer or tuple header raises a clean `DATA_CORRUPTED` error.

```sql
-- Physical checksum without header
SELECT pg_tuple_physical_checksum('orders'::regclass, ctid, false) 
FROM orders WHERE order_id = 123;

-- Physical checksum with header (includes MVCC info)
SELECT pg_tuple_physical_checksum('orders'::regclass, ctid, true) 
FROM orders WHERE order_id = 123;
```

**`pg_tuple_logical_checksum(regclass, tid) -> integer`**

Computes a canonical logical checksum of one row.  All non-dropped columns are
hashed; relation OIDs, heap TIDs and the tuple header are excluded.  No primary
key is required.  Requires `SELECT`; relations protected by row-level security
are rejected.

```sql
-- Logical checksum of one row
SELECT pg_tuple_logical_checksum('customers'::regclass, ctid)
FROM customers WHERE customer_id = 456;
```

### Table Level Functions

**`pg_table_physical_checksum(regclass, boolean) -> bigint`**

Aggregates physical tuple checksums for entire table (order-independent).
Requires `SELECT` on the table.  The function accepts ordinary tables,
materialized views, partitioned tables and (for physical checksums) TOAST
relations.  A TOAST relation is checksummed as raw pages, because its tuples
are not heap tuples.  A corrupted page produces a clean error rather than
crashing the backend.

For an ordinary table, the physical checksum covers the main heap only: a
TOAST value is represented by its pointer in the main tuple, so corruption
inside the TOAST data is not detected by this function.  Use
`pg_table_logical_checksum` (which detoasts values) or a database physical
checksum with `include_toast = true` to cover TOAST content.

```sql
-- Table physical checksum
SELECT pg_table_physical_checksum('inventory'::regclass, false);

-- With tuple headers included
SELECT pg_table_physical_checksum('inventory'::regclass, true);
```

**`pg_table_logical_checksum(regclass) -> bigint`**

Aggregates canonical logical row checksums for an entire table.  It does not
require a primary key, and relation OIDs are not included.  Duplicate rows are
preserved by a sum-plus-count aggregate, so two identical rows contribute
twice.  Requires `SELECT` on the table; returns NULL for a TOAST relation
(toast content is covered when the parent table detoasts its values).

```sql
-- Logical checksum (no primary key required)
SELECT pg_table_logical_checksum('products'::regclass);
```

**Partitioned tables**: `pg_table_physical_checksum` and
`pg_table_logical_checksum` accept partitioned tables. Since a partitioned
table has no storage of its own, the extension recurses into every leaf
partition.  Logical checksums accumulate all leaf rows into the same
accumulator, so the result does not depend on the partitioning layout.
Nested (multi-level) partitioning is supported as well.

```sql
-- Checksum a partitioned table (recurses into all partitions)
SELECT pg_table_physical_checksum('sales'::regclass, false);
SELECT pg_table_logical_checksum('sales'::regclass);
```

### Index Level Functions

**`pg_index_physical_checksum(regclass) -> integer`**

Computes physical checksum of index structure. Supports B-tree, Hash, GiST, GIN, SP-GiST, BRIN.
Requires `SELECT` on the underlying table.  Returns NULL for an unsupported
access method.  A partitioned index aggregates the physical checksums of its
leaf indexes.

```sql
-- Physical checksum of a B-tree index
SELECT pg_index_physical_checksum('idx_product_name'::regclass);
```

**`pg_index_logical_checksum(regclass) -> integer`**

Computes the logical content of an index from the visible table rows and the
index definition.  Plain columns, expressions, partial-index predicates and
INCLUDE columns are evaluated for each row; no heap TIDs, relation OIDs or dead
entries are included.  It works for every index access method (B-tree, Hash,
GiST, GIN, SP-GiST, BRIN and custom AMs), and for partitioned indexes.

```sql
-- Logical content of any index
SELECT pg_index_logical_checksum('idx_product_name'::regclass);
```

**Logical vs physical index checksums.**  The logical checksum describes what
the index should contain from the table data; it does not inspect physical
index pages.  A missing or corrupt index entry is therefore detected by
`pg_index_physical_checksum`, not by the logical value.

### Database Level Functions (Superuser Only)

**`pg_database_physical_checksum(boolean, boolean) -> bigint`**

Aggregates physical relation checksums for entire database.  Superuser-only;
the check is enforced in C (the SQL functions are security invoker, so
`superuser()` sees the calling session user).

```sql
-- Database physical checksum (excluding system tables)
SELECT pg_database_physical_checksum(false, false);

-- Include system catalogs and TOAST tables
SELECT pg_database_physical_checksum(true, true);
```

**TOAST:** `include_toast` is independent of `include_system`.  With
`include_toast = true`, TOAST relations are hashed as raw pages.  With
`include_toast = false` (the default), changes inside toasted values are not
covered by the physical database checksum; table-level physical checksums
store only the toast pointer in the main tuple.

**Note about shared system tables**: The database checksum only includes
relations from the current database.  Cluster-wide shared catalogs (e.g.,
`pg_authid`, `pg_database`) are excluded even when `include_system` is true.

**Skipped relations:** if a relation cannot be processed (dropped
concurrently, corrupt, unsupported, ...), the scan rolls back an internal
subtransaction, emits a warning and continues.  A summary warning is emitted
at the end.  Cancellation and `statement_timeout` are propagated, so a
canceled scan never returns a partial result as if it were complete.

**PostgreSQL 12-16 and parallel database scans:** those releases prohibit
internal subtransactions while in parallel mode.  A parallel database scan
therefore cannot isolate relation errors: if a relation fails, the scan is
aborted.  Run with `pg_integrity_checksums.workers = 0` (the default) to get the
skip-with-warning behavior on PostgreSQL 12-16.  Serial scans and parallel
scans on PostgreSQL 17+ isolate errors as described above.

**`pg_database_logical_checksum(include_system boolean, include_indexes boolean) -> bigint`**

Aggregates canonical logical checksums for the database.  Tables without a
primary key are included.  TOAST relations are skipped: their content is
covered when the parent table detoasts the values.  `include_indexes`
(default true) controls whether logical index checksums are included; because
they are derived from table data, they do not change when dead index entries
are vacuumed.

Superuser-only (enforced in C).

```sql
-- Database logical checksum, including indexes
SELECT pg_database_logical_checksum(false, true);

-- Tables only
SELECT pg_database_logical_checksum(false, false);
```

### Utility Functions

**`pg_data_checksum(bytea, integer) -> integer`**

Generic checksum for arbitrary binary data using FNV-1a.

```sql
SELECT pg_data_checksum(E'\x01020304'::bytea, 0);
```

**`pg_text_checksum(text, integer) -> integer`**

Wrapper for text data checksum.

```sql
SELECT pg_text_checksum('sample text', 0);
```

---

## Algorithm Details

### FNV-1a 32-bit Hash Algorithm

Logical, cell, tuple, table, index and utility checksums use the FNV-1a
(Fowler-Noll-Vo) 32-bit hash.  PostgreSQL's `pg_page_checksum` is computed with
PostgreSQL's own page-checksum routine, which is based on a modified FNV-1a
variant; it is not the same function as the logical checksum.

**Algorithm:**

```text
hash = FNV_BASIS_32;
for each byte in data:
    hash = hash ^ byte;
    hash = hash * FNV_PRIME_32;
```

**Constants:**

```
FNV_BASIS_32 = 2166136261U
FNV_PRIME_32 = 16777619U
CHECKSUM_NULL = 0xFFFFFFFF (signed -1)
```

### Order-Independent Aggregation

Physical and index aggregates use XOR, which is commutative and associative and
therefore independent of scan order and partition boundaries.

Logical table, index and database aggregates use a **sum plus count** instead.
Addition is still order-independent, but the count preserves duplicate rows,
which a pure XOR would cancel.  Parallel workers combine their partial sums and
counts before the final 32-bit mix, so serial and parallel results are
identical.

### Type-Specific Handling

Logical values use a canonical type-aware encoding:

* **Scalar types** use their binary send function (network byte order), or the
  output function when no send function exists.
* **Enums** are hashed by label, not by type OID.
* **numeric** is normalized with `numeric_normalize()`, so `1.0` and `1.00`
  compare equal.
* **Arrays, composites, domains and ranges** are traversed recursively, so
  embedded type OIDs are not part of the hash.
* **Fixed-width integers** in array shape, composite counts and multirange
  counts are encoded in little-endian byte order, independent of the server's
  native endianness.
* **Floating point** canonicalizes NaN and negative zero.

---

## Performance Characteristics

### Computational Complexity

| Function                 |       Complexity | Notes                           |
| ------------------------ | ---------------: | ------------------------------- |
| `pg_cell_checksum`     |             O(1) | Single cell, minimal overhead |
| `pg_tuple_*_checksum`    |    O(tuple size) | Requires buffer lock            |
| `pg_table_*_checksum`    |   O(rows × columns) | Order-independent aggregation |
| `pg_index_physical_checksum` | O(index size) | Bulk read strategy |
| `pg_index_logical_checksum` | O(table rows) | Scans heap rows, not index pages |
| `pg_database_*_checksum` | O(total DB size) | Periodic interrupt checks |

Table, index, and database scans can be parallelized with
`pg_integrity_checksums.workers`; the scan work is divided among the workers with
linear speedup up to the point where the scan becomes I/O-bound.

---
