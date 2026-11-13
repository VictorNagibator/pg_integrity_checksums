# Functions and algorithms

New to the extension? Read [Getting started](getting-started.md) and
[Concepts](concepts.md) first.  This page is the exhaustive reference.

## Quick reference

| Level | Generic API (optional) | Default 32-bit API |
|---|---|---|
| Page | — | `pg_page_checksum(regclass, integer)` / `pg_page_checksum_status(regclass, integer)` |
| Cell | `pg_cell_checksum(regclass, tid, integer, text)` | `pg_cell_checksum(regclass, tid, integer)` |
| Tuple physical | `pg_tuple_physical_checksum(regclass, tid, boolean, text)` | `pg_tuple_physical_checksum(regclass, tid, boolean)` |
| Tuple logical | `pg_tuple_logical_checksum(regclass, tid, text)` | `pg_tuple_logical_checksum(regclass, tid)` |
| Table physical | `pg_table_physical_checksum(regclass, boolean, text)` | `pg_table_physical_checksum(regclass, boolean)` |
| Table logical | `pg_table_logical_checksum(regclass, text)` | `pg_table_logical_checksum(regclass)` |
| Index physical | `pg_index_physical_checksum(regclass, text)` | `pg_index_physical_checksum(regclass)` |
| Index logical | `pg_index_logical_checksum(regclass, text)` | `pg_index_logical_checksum(regclass)` |
| Database physical | `pg_database_physical_checksum(boolean, boolean, text)` | `pg_database_physical_checksum(boolean, boolean)` |
| Database logical | `pg_database_logical_checksum(boolean, boolean, text)` | `pg_database_logical_checksum(boolean, boolean)` |
| Data/text | `pg_data_checksum(bytea, bigint, text)` / `pg_text_checksum(text, bigint, text)` | `pg_data_checksum(bytea, integer)` / `pg_text_checksum(text, integer)` |

The generic API returns `bytea` and accepts `fnv32` or `fnv64`.  The default
32-bit API returns `integer`/`bigint` and is the simplest choice for most
users.

Privileges, NULL handling and object-level behavior are the same for the two
forms.  For per-function details, read the corresponding 32-bit function
above; only the return type and algorithm argument differ.

## Default 32-bit API and page checksums

The functions in this section return `integer` or `bigint` and use FNV-1a
32-bit.  This is the simplest API and the default for most users.
`pg_page_checksum` is the page API: PostgreSQL page checksums are 16-bit.

### Page Level Functions (Physical Only)

**`pg_page_checksum(regclass, integer) -> integer`**

Calculates PostgreSQL's built-in **16-bit** page checksum.  The block number is
validated: an out-of-range block (including `-1`, which would otherwise be
interpreted as `P_NEW`) raises an error instead of reading or extending the
relation.  The page is copied before the checksum is computed, so the shared
buffer is never modified.

This is a calculator, not a verifier: it does **not** compare the result with
the stored `pd_checksum`.  It returns `0` for a new/uninitialized page, so a
zero result cannot be distinguished from a valid page whose calculated
checksum is zero.  PostgreSQL must also have `data_checksums = on` for the
stored checksum to be meaningful; otherwise `pd_checksum` is not maintained.
Use `pg_page_checksum_status` for an actual comparison.

**`pg_page_checksum_status(regclass, integer) -> text`**

Compares the calculated 16-bit page checksum with the value stored in the page
header and returns:

| Result | Meaning |
|---|---|
| `ok` | Calculated and stored checksums match |
| `mismatch` | The page checksum does not match |
| `new` | The page is new/uninitialized |
| `checksums_disabled` | PostgreSQL was initialized without data checksums |

The status function reads relation storage directly, so it verifies the page
currently on disk rather than a possibly dirty shared-buffer copy.  An invalid
block or a relation without storage raises an error rather than returning a
status.  Use `pg_page_checksum` when you explicitly want the calculated value
from the buffer.

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
relations.  A TOAST relation is checksummed page by page, with hint bits,
the page LSN and the in-buffer `pd_checksum` masked out.  A corrupted page
produces a clean error rather than crashing the backend.

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

**Skipped relations:** by default (`pg_integrity_checksums.strict = on`, the
default since 2.5) a relation that cannot be processed (dropped concurrently,
corrupt, an invalid index, unsupported, ...) makes the whole call fail with
`relation "..." cannot be checksummed`, so a partial result is never returned
as if it were complete.  With `strict = off` the scan rolls back an internal
subtransaction for that relation, emits a warning, excludes the relation and
continues; a summary warning is emitted at the end.  Cancellation and
`statement_timeout` are always propagated.  See
[Strict mode](operations.md#strict-mode).

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

## Generic API (optional 32/64-bit)

The default numeric functions above keep their FNV-1a 32-bit return type.
The generic overloads take an extra `text` algorithm argument and return a
raw `bytea` digest:

```sql
pg_data_checksum(bytea, bigint, text)
pg_text_checksum(text, bigint, text)
pg_cell_checksum(regclass, tid, integer, text)
pg_tuple_physical_checksum(regclass, tid, boolean, text)
pg_tuple_logical_checksum(regclass, tid, text)
pg_table_physical_checksum(regclass, boolean, text)
pg_table_logical_checksum(regclass, text)
pg_index_physical_checksum(regclass, text)
pg_index_logical_checksum(regclass, text)
pg_database_physical_checksum(boolean, boolean, text)
pg_database_logical_checksum(boolean, boolean, text)
```

Supported algorithms:

* `fnv32` — 4-byte big-endian digest;
* `fnv64` — 8-byte big-endian digest.

Table and database physical checksums always return 8 bytes because relation
identity is part of the value.  Convert a digest to a signed SQL `bigint`
with:

```sql
pg_checksum_bigint(bytea) -> bigint
pg_checksum_bigint(bytea, boolean) -> bigint
```

The second argument selects unsigned interpretation for 4-byte `fnv32`
digests.  The one-argument form sign-extends 4-byte values, matching the
`integer` functions.  Use the unsigned form when comparing with default table
logical checksums, which return a positive 32-bit value in a `bigint`.

```sql
SELECT encode(pg_table_logical_checksum('orders'::regclass, 'fnv64'), 'hex');
SELECT pg_checksum_bigint(pg_table_logical_checksum('orders'::regclass, 'fnv64'));
```

`pg_page_checksum` intentionally has no generic overload: page checksums are
PostgreSQL's built-in 16-bit page checksum.

---

## Algorithm Details

### FNV-1a 32-bit and 64-bit

Default logical, cell, tuple, table, index and utility functions use the
FNV-1a (Fowler-Noll-Vo) 32-bit hash.  Generic overloads add the 64-bit
variant.  PostgreSQL's `pg_page_checksum` is computed with PostgreSQL's own
page-checksum routine, which is based on a modified FNV-1a variant; it is not
the same function as the logical checksum and is always 32-bit.

**Algorithm:**

```text
hash = basis
for each byte in data:
    hash = hash ^ byte;
    hash = hash * prime;
```

**Constants:**

```
FNV_BASIS_32 = 2166136261U
FNV_PRIME_32 = 16777619U
FNV_BASIS_64 = 14695981039346656037
FNV_PRIME_64 = 1099511628211
CHECKSUM_NULL = 0xFFFFFFFF (signed -1, 32-bit path)
```

The 64-bit path uses a 64-bit final mix.  Its NULL sentinel is the 32-bit
sentinel zero-extended, `0x00000000FFFFFFFF`: `pg_cell_checksum(..., 'fnv64')`
returns `\x00000000ffffffff` for a NULL cell.  (Earlier documentation said
`UINT64_MAX`; the value has always been `0x00000000FFFFFFFF`, and it is kept so
existing v2 baselines stay valid.)  Non-NULL values that hash to zero or to
`UINT64_MAX` are remapped by the final mix.
All canonical value handling (types, arrays, ranges, numerics, endianness)
is shared between the 32-bit and 64-bit paths.

### Order-Independent Aggregation

Physical and index aggregates use XOR, which is commutative and associative and
therefore independent of scan order and partition boundaries.

Logical table, index and database aggregates use a **sum plus count** instead.
Addition is still order-independent, but the count preserves duplicate rows,
which a pure XOR would cancel.  Parallel workers combine their partial sums and
counts before the final mix, so serial and parallel results are identical for
both 32-bit and 64-bit algorithms.

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
