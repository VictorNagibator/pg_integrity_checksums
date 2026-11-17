# Functions and algorithms

New to the extension? Read [Getting started](getting-started.md) and
[Concepts](concepts.md) first.  This page is the exhaustive reference.

## Quick reference

| Level | Generic API (optional) | Default 32-bit API |
|---|---|---|
| Page | — | `pg_page_checksum(regclass, integer)` / `pg_page_checksum_status(regclass, integer)` |
| Pages of a relation | — | `pg_relation_page_checksum_status(regclass, text, boolean)` |
| Cell | `pg_cell_checksum(regclass, tid, integer, text)` | `pg_cell_checksum(regclass, tid, integer)` |
| Tuple physical | `pg_tuple_physical_checksum(regclass, tid, boolean, text)` | `pg_tuple_physical_checksum(regclass, tid, boolean)` |
| Tuple logical | `pg_tuple_logical_checksum(regclass, tid, text)` | `pg_tuple_logical_checksum(regclass, tid)` |
| Table physical | `pg_table_physical_checksum(regclass, boolean, text)` | `pg_table_physical_checksum(regclass, boolean)` |
| Table logical | `pg_table_logical_checksum(regclass, text)` | `pg_table_logical_checksum(regclass)` |
| Index physical | `pg_index_physical_checksum(regclass, text)` | `pg_index_physical_checksum(regclass)` |
| Index logical | `pg_index_logical_checksum(regclass, text)` | `pg_index_logical_checksum(regclass)` |
| Sequence logical | `pg_sequence_logical_checksum(regclass, text)` | `pg_sequence_logical_checksum(regclass)` |
| Database physical | `pg_database_physical_checksum(boolean, boolean, text)` | `pg_database_physical_checksum(boolean, boolean)` |
| Database logical | `pg_database_logical_checksum(boolean, boolean, text)` | `pg_database_logical_checksum(boolean, boolean)` |
| Database, per relation | `pg_database_physical_checksums(boolean, boolean, regclass[], text)` / `pg_database_logical_checksums(boolean, boolean, regclass[], text)` | same functions (`algorithm` defaults to `fnv32`) |
| Data/text | `pg_data_checksum(bytea, bigint, text)` / `pg_text_checksum(text, bigint, text)` | `pg_data_checksum(bytea, integer)` / `pg_text_checksum(text, integer)` |
| Key buckets (logical) | `pg_table_bucket_checksums(regclass, integer, text[], text)` | — |
| Rows of buckets (logical) | `pg_table_row_checksums(regclass, integer, integer[], text[], text)` | — |
| Block ranges (logical) | `pg_table_block_checksums(regclass, integer, text)` | — |

The generic API returns `bytea` and accepts `fnv32`, `fnv64`, `crc32c` or
`xxh3` (see [Algorithm Details](#algorithm-details)).  The default
32-bit API returns `integer`/`bigint` and is the simplest choice for most
users.  The bucket, row and block range functions (see
[Finding differing rows](#finding-differing-rows)) return sets and default to
`fnv64`.

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

The read takes no buffer lock, so it can overlap with a concurrent write of the
same block and see a torn page.  As `pg_basebackup` does, a block whose
checksum does not match is read again (up to five times, with short pauses)
and `mismatch` is returned only when every read disagrees; real corruption
fails every read.

```sql
-- Get checksum for page 5 of a table
SELECT pg_page_checksum('my_table'::regclass, 5);
```

Temporary tables are verified the same way, from their file: a page of a
temporary table that is still only in this session's local buffers is `new`.
Temporary tables of other sessions are rejected.

**`pg_relation_page_checksum_status(relname regclass, fork_name text DEFAULT NULL, include_ok boolean DEFAULT false)`**
**`-> SETOF (fork text, blkno bigint, status text, stored_checksum integer, calculated_checksum integer)`**

Verifies every page of a relation (table, index, materialized view, sequence
or TOAST table) against its stored checksum, in one call.  Each block is
checked exactly as `pg_page_checksum_status` checks it: the on-disk copy is
read, bypassing shared buffers, and a mismatching block is read again before
it is reported.

- `fork_name`: `main`, `fsm`, `vm` or `init`.  `NULL` (the default) checks
  every fork that exists, like the `pg_checksums` utility does; the free space
  map and visibility map pages carry checksums as well.
- `include_ok`: by default only blocks with status `mismatch` are returned, so
  an empty result means the relation verified.  With `true`, every block is
  returned with status `ok`, `mismatch` or `new`.

`stored_checksum` and `calculated_checksum` are the 16-bit values (NULL for a
`new` page).  The function fails if the cluster runs without data checksums,
since no page could be verified.  It requires `SELECT` on the relation;
relations without storage (views, partitioned tables) raise an error, so check
each partition separately.  The relation is locked with `AccessShareLock` for
the duration of the scan; blocks added after the scan started are not checked.
The scan reads the whole relation from disk, so run it outside peak hours on
large relations.  A page changed in shared buffers but not yet written is
verified in its previous on-disk version; run `CHECKPOINT` first to verify the
newest state.

```sql
-- Corrupt blocks of a table, any fork
SELECT fork, blkno, stored_checksum, calculated_checksum
FROM pg_relation_page_checksum_status('orders');

-- Summary of every block of the table's main fork
SELECT status, count(*)
FROM pg_relation_page_checksum_status('orders', 'main', include_ok => true)
GROUP BY status;

-- Every table and index of a schema
SELECT c.oid::regclass AS relation, s.fork, s.blkno
FROM pg_class c
CROSS JOIN LATERAL pg_relation_page_checksum_status(c.oid) s
WHERE c.relnamespace = 'public'::regnamespace
  AND c.relkind IN ('r', 'i', 'm', 't', 'S')
  AND c.relpersistence <> 't';
```

### Cell Level Functions (Logical Only)

**`pg_cell_checksum(regclass, tid, integer) -> integer`**

Computes logical checksum of a cell value. NULL values return `CHECKSUM_NULL` (-1).
Requires `SELECT` on the relation; an out-of-range TID or attribute number
returns NULL.  Relations protected by row-level security are rejected.

All tuple and cell functions return NULL for a TID whose tuple is not visible
to the query's snapshot: a deleted row, an old version of an updated row, or
a row inserted by a transaction that has not committed.  Such a TID still
points at the row's data, and a checksum of it would reveal data the caller
cannot `SELECT`.  Pass `ctid` values from the same query (as in the examples)
to checksum the rows you see.

```sql
-- Get checksum for cell (column) 3 of a specific tuple
SELECT pg_cell_checksum('employees'::regclass, '(5,2)'::tid, 3);
```

**Note**: Cell checksums are computed for individual values within specific rows, not for entire columns. To compute checksums for all values in a column, you would need to aggregate individual checksums.

### Tuple Level Functions

**`pg_tuple_physical_checksum(regclass, tid, boolean) -> integer`**

Computes physical tuple checksum based on physical location (block, offset).
Requires `SELECT` on the relation.  An invalid TID, a TID without a tuple and
a tuple that is not visible return NULL; a corrupted line pointer or tuple
header raises a clean `DATA_CORRUPTED` error.

The checksum includes the tuple's `xmin`, `xmax` and command id even when the
header is not requested, so a row lock (`SELECT ... FOR UPDATE`/`FOR SHARE`,
or the `FOR KEY SHARE` lock a foreign key check takes on the referenced row)
and a rolled-back `UPDATE` or `DELETE` change it although the row's data did
not change.  The boolean argument adds the whole tuple header (`t_ctid`, the
infomask bits other than hint bits, `t_hoff`, the null bitmap).  Use
`pg_tuple_logical_checksum` to detect changes of the data itself.

```sql
-- Physical checksum without header
SELECT pg_tuple_physical_checksum('orders'::regclass, ctid, false) 
FROM orders WHERE order_id = 123;

-- Physical checksum with the rest of the tuple header
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

Aggregates physical tuple checksums for entire table (order-independent).  Like
the tuple checksums, it changes when rows are locked (see
[Physical checksums](concepts.md#physical-checksums)).
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
GiST, GIN, SP-GiST, BRIN and custom AMs), and for partitioned indexes.  The
table must use the heap table access method, as for the table checksums.  The
table is locked before the index, in the order PostgreSQL itself uses, so a
concurrent `DROP INDEX` or `DROP TABLE` waits instead of deadlocking.

```sql
-- Logical content of any index
SELECT pg_index_logical_checksum('idx_product_name'::regclass);
```

**Logical vs physical index checksums.**  The logical checksum describes what
the index should contain from the table data; it does not inspect physical
index pages.  A missing or corrupt index entry is therefore detected by
`pg_index_physical_checksum`, not by the logical value.

### Sequence Level Functions (Logical Only)

**`pg_sequence_logical_checksum(regclass) -> integer`**

Checksums a sequence: its definition (data type, start, increment, minimum,
maximum, cache and cycle, from `pg_sequence`) and its state (`last_value`
and `is_called`).  This is what `pg_dump` restores with `CREATE SEQUENCE` and
`setval()`, so after a dump/restore or a migration the checksum matches the
source only if the sequence was carried over: a sequence left at its start
value, while the table rows were copied, hands out keys that already exist.

```sql
SELECT pg_sequence_logical_checksum('orders_id_seq'::regclass);

-- Every sequence of a schema, to compare between two databases
SELECT c.relname, pg_sequence_logical_checksum(c.oid)
FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE c.relkind = 'S' AND n.nspname = 'public'
ORDER BY 1;
```

* The value does not depend on the sequence's name or OID: two sequences
  with the same definition and state have the same checksum.
* `log_cnt` (the number of values pre-logged to WAL) is not included; it
  changes on `nextval()` and `setval()` without telling anything new.
* Sequences are not transactional, so the state is read as it is at the
  call, not as of the transaction snapshot.  Compare sequences while nothing
  calls `nextval()`, as for any migration cut-over.
* A physical standby does not match its primary: WAL replay applies the
  pre-logged value, so `last_value` on a standby is ahead by up to 32 values
  (and a primary's own value jumps the same way after a crash).  Use it to
  check migrations, dumps and logical copies, not physical replicas.
* Requires `SELECT` on the sequence (`USAGE`, which allows `nextval()`, is
  not enough).  Returns NULL for an unlogged sequence on a standby, which has
  no state to read.  Fails for a relation that is not a sequence.

Sequences are not part of `pg_database_logical_checksum` or
`pg_database_logical_checksums`; including them would change existing
database checksum values.  The [CLI](cli.md) captures them as objects of
their own.

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

Each relation's checksum is combined with its qualified name
(`schema.relname`) before it is aggregated, so moving rows from one table to
another table of the same structure changes the result.  The name, not the
OID, is used so that the value survives a dump and restore; renaming a
relation or moving it to another schema therefore changes the database
checksum, although the relation's own logical checksum does not change.

Superuser-only (enforced in C).

```sql
-- Database logical checksum, including indexes
SELECT pg_database_logical_checksum(false, true);

-- Tables only
SELECT pg_database_logical_checksum(false, false);
```

**`pg_database_logical_checksums(include_system, include_indexes, relations, algorithm)`**
**`pg_database_physical_checksums(include_system, include_toast, relations, algorithm)`**

Set-returning variants (since 2.8) that return the checksum of every relation
of the database scan instead of one aggregate value, so a changed database
checksum can be traced to the relations that changed.  All arguments are
optional:

| Argument | Default | Meaning |
|---|---|---|
| `include_system` | `false` | Include `pg_catalog` relations, as in the aggregate functions |
| `include_indexes` / `include_toast` | `true` / `false` | As in `pg_database_logical_checksum` / `pg_database_physical_checksum` |
| `relations regclass[]` | `NULL` (all) | Only return these relations.  Listed relations that the scan would not include (a catalog without `include_system`, a view, a temporary table) return no row; `NULL` elements are ignored |
| `algorithm text` | `'fnv32'` | `fnv32`, `fnv64`, `crc32c` or `xxh3` |

Each row has these columns:

| Column | Type | Content |
|---|---|---|
| `relid` | `regclass` | The relation |
| `schema_name`, `rel_name` | `name` | Its qualified name, as of the scan snapshot |
| `relkind` | `"char"` | `r` table, `m` materialized view, `i` index, `t` TOAST table |
| `checksum` | `bytea` | The relation's checksum; `NULL` unless `status` is `ok` |
| `status` | `text` | `ok`, `skipped` (no checksum in this mode, for example an invalid index) or `error` |
| `reason` | `text` | Why the relation was skipped or failed; `NULL` for `ok` |

`checksum` is exactly what the per-relation function with the same algorithm
returns (`pg_table_logical_checksum(rel, algorithm)`,
`pg_index_physical_checksum(rel, algorithm)`, ...), including its width:
4 bytes for `fnv32` and `crc32c` except physical table checksums, which are
always 8 bytes.  Use `pg_checksum_bigint(checksum)` for the integer form.  It is the
relation's own checksum: the aggregate database checksum additionally folds in
the relation identity.

The scan is the one of the aggregate functions, with the same snapshot for
every relation, the same relation selection, the same parallelism
(`pg_integrity_checksums.workers` hands whole relations to workers) and, for a
serial logical scan, one heap pass per table for the table and all of its
indexes.  Strict mode applies: with `strict = on` a relation without a usable
checksum fails the call; with `strict = off` it is returned with status
`skipped` or `error` (the per-relation warning carries the error message).
A relation dropped while the scan runs is left out.  Rows are ordered by
schema and relation name.  Superuser-only (enforced in C).

```sql
-- Every relation of the database
SELECT schema_name, rel_name, relkind, checksum, status
FROM pg_database_logical_checksums();

-- Two tables, 64-bit
SELECT rel_name, checksum
FROM pg_database_logical_checksums(
         relations => ARRAY['public.orders', 'public.customers']::regclass[],
         algorithm => 'fnv64');

-- Store a snapshot, later list what changed
CREATE TABLE checksums_before AS
SELECT relid, checksum FROM pg_database_logical_checksums();
-- ...
SELECT n.relid, n.status
FROM pg_database_logical_checksums() AS n
FULL JOIN checksums_before AS b USING (relid)
WHERE n.checksum IS DISTINCT FROM b.checksum;
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

## Generic API (algorithm argument)

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
pg_sequence_logical_checksum(regclass, text)
pg_database_physical_checksum(boolean, boolean, text)
pg_database_logical_checksum(boolean, boolean, text)
```

Supported algorithms:

| Algorithm | Digest | Since | Notes |
|---|---|---|---|
| `fnv32` | 4 bytes | 2.2 | FNV-1a 32-bit, the same values as the default `integer` API |
| `fnv64` | 8 bytes | 2.2 | FNV-1a 64-bit |
| `crc32c` | 4 bytes | 2.9 | CRC-32C (Castagnoli), with PostgreSQL's implementation (CPU CRC instructions where available) |
| `xxh3` | 8 bytes | 2.9 | XXH3-64 from xxHash 0.8.3 |

`crc32c` and `xxh3` are much faster than FNV-1a on long inputs (pages,
large values) and slightly slower on short ones; see
[Which API should I use?](concepts.md#generic-api-choose-the-algorithm).
Digests are big-endian.  Names are case-sensitive; an unknown name is an
error that lists the supported ones.  None of the algorithms is
cryptographic.

The `bigint` seed of `pg_data_checksum` and `pg_text_checksum` selects the
starting state: `0` is the algorithm's standard start (the FNV offset basis,
a fresh CRC-32C, XXH3's default seed).  For `crc32c` the seed is a previous
CRC-32C value to continue, so the CRC of a concatenation can be computed in
parts; for `xxh3` it is the XXH3 seed; for `fnv32` and `crc32c` only its low
32 bits are used.

```sql
-- the standard CRC-32C check value
SELECT encode(pg_text_checksum('123456789', 0, 'crc32c'), 'hex');  -- e3069283
-- continue a CRC: crc32c('hello world') in two parts
SELECT pg_data_checksum(convert_to(' world', 'UTF8'),
                        pg_checksum_bigint(pg_data_checksum(convert_to('hello', 'UTF8'), 0, 'crc32c'), true),
                        'crc32c');
```

Table and database physical checksums always return 8 bytes because relation
identity is part of the value.  Convert a digest to a signed SQL `bigint`
with:

```sql
pg_checksum_bigint(bytea) -> bigint
pg_checksum_bigint(bytea, boolean) -> bigint
```

The second argument selects unsigned interpretation for 4-byte (`fnv32`,
`crc32c`) digests.  The one-argument form sign-extends 4-byte values, matching the
`integer` functions.  Use the unsigned form when comparing with default table
logical checksums, which return a positive 32-bit value in a `bigint`.

```sql
SELECT encode(pg_table_logical_checksum('orders'::regclass, 'fnv64'), 'hex');
SELECT pg_checksum_bigint(pg_table_logical_checksum('orders'::regclass, 'fnv64'));
```

`pg_page_checksum` intentionally has no generic overload: page checksums are
PostgreSQL's built-in 16-bit page checksum.

---

## Finding differing rows

A table checksum tells *that* two copies of a table differ.  These three
set-returning functions (since 2.8) split the same logical checksum into
pieces, so the differing rows can be found without moving the table between
servers.  All of them require `SELECT` on the table, reject tables with
row-level security, see the statement's snapshot (only visible rows count)
and accept every algorithm of the generic API (`fnv64` is the default) as
`algorithm`.  The key hash that assigns a row to a bucket is always 64-bit
FNV-1a, so the buckets of a row are the same whichever algorithm computes the
checksums.
`pg_table_bucket_checksums` hands chunks of blocks to parallel workers when
`pg_integrity_checksums.workers` is above zero, like the table checksums (not
for temporary tables, nor for a partition whose columns are in another order
than the partitioned table's); the result is the same as a serial scan.
`pg_table_row_checksums` and `pg_table_block_checksums` always run in the
calling backend.

The row checksums are the logical tuple checksums, and each bucket or range is
aggregated like `pg_table_logical_checksum`: the checksum of one bucket that
holds every row equals `pg_table_logical_checksum(relname, algorithm)`, and an
empty bucket has the checksum of an empty table.

**`pg_table_bucket_checksums(relname regclass, buckets integer, key_columns text[] DEFAULT NULL, algorithm text DEFAULT 'fnv64')`**
**`-> TABLE(bucket integer, row_count bigint, checksum bytea)`**

Hashes the key of every visible row and returns one logical checksum per
bucket of the key hash space, in bucket order and including empty buckets.
`buckets` is between 1 and 1048576.

The key is

- `key_columns` when given, in that order (column names, not quoted
  identifiers);
- the primary key when `key_columns` is `NULL`;
- the whole row when `key_columns` is an empty array, or when it is `NULL`
  and the table has no primary key.

The key hash uses the canonical logical encoding, so a row lands in the same
bucket on every server, after a dump and restore and on any byte order.  Use
the same key on both sides: when only one side has a primary key, pass
`key_columns` explicitly.  With the whole row as the key a changed row shows up
as a missing and an extra row.

Buckets are equal ranges of the 64-bit key hash: bucket `b` of `n` contains
exactly the buckets `b*k` to `b*k+k-1` of `n*k`.  A comparison can therefore
be refined by multiplying the number of buckets.  Pick `n` so that a bucket
holds a few hundred to a few thousand rows.

```sql
SELECT bucket, row_count, checksum
FROM pg_table_bucket_checksums('orders', 1024);

-- Composite key in a chosen order, 32-bit checksums.
SELECT * FROM pg_table_bucket_checksums('order_lines', 256,
                                        '{order_id,line_no}', 'fnv32');
```

**`pg_table_row_checksums(relname regclass, buckets integer DEFAULT 1, only_buckets integer[] DEFAULT NULL, key_columns text[] DEFAULT NULL, algorithm text DEFAULT 'fnv64')`**
**`-> TABLE(bucket integer, key_hash bytea, key text, row_checksum bytea, ctid tid, tableoid regclass)`**

One row per visible row whose bucket is in `only_buckets` (every row when it
is `NULL`), in physical order.  `buckets` and `key_columns` must be the values
passed to `pg_table_bucket_checksums`.

| Column | Meaning |
|---|---|
| `bucket` | the row's bucket |
| `key_hash` | 64-bit canonical hash of the key; portable, compare rows by it |
| `key` | the key as record text, for example `(42)` or `(7,abc)`; produced by the output functions, so it depends on settings such as `DateStyle` |
| `row_checksum` | logical checksum of the row, as `pg_tuple_logical_checksum(relname, ctid, algorithm)` |
| `ctid`, `tableoid` | where the row is, on this server and in this snapshot (`tableoid` names the partition) |

A key hash that only one side has is a missing or extra row; a key hash with
different row checksums is a changed row.  `key` can be compared with
`ROW(...)::text` of the key columns: `WHERE ROW(id)::text = '(42)'`.

```sql
SELECT key, row_checksum
FROM pg_table_row_checksums('orders', 1024, '{17,512}');
```

**`pg_table_block_checksums(relname regclass, blocks_per_range integer DEFAULT 1, algorithm text DEFAULT 'fnv64')`**
**`-> TABLE(start_block bigint, end_block bigint, row_count bigint, checksum bytea)`**

One logical checksum of the visible rows per range of heap blocks
`[start_block, end_block)`.  `end_block` is always `start_block +
blocks_per_range`, so ranges line up between servers whose table has a
different number of blocks.  Block numbers only mean the same thing on a
primary and its **physical** standbys, which store every row at the same TID:
use this function to find where a standby diverges, and the key buckets for
everything else (logical replicas, restored dumps, other clusters).  Not
available for partitioned tables; call it for each partition.  To list the
rows of a range, select `ctid` and `pg_tuple_logical_checksum` with a TID
range condition:

```sql
SELECT * FROM pg_table_block_checksums('orders', 128);

SELECT ctid, pg_tuple_logical_checksum('orders'::regclass, ctid, 'fnv64')
FROM orders WHERE ctid >= '(1280,0)' AND ctid < '(1408,0)';
```

The complete procedure for comparing two servers is in
[Locating differing rows](operations.md#7-locating-differing-rows-between-two-servers).

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

### CRC-32C and XXH3

`crc32c` and `xxh3` replace FNV-1a wherever bytes are hashed: values,
tuples, pages, and the order-dependent combination of the parts of a value
(type tag, position, array elements, composite fields, a row's columns).
The order-independent aggregation of rows, pages and relations, the final
mixing and the sentinels described here depend only on the digest width, so
`crc32c` aggregates like `fnv32` and `xxh3` like `fnv64`.  An empty table,
for example, has the same logical checksum with `fnv32` and `crc32c`.

A logical value is hashed in steps, each continuing from the previous result
and starting from the FNV offset basis of the algorithm's width:

1. the type tag, then that hash as little-endian bytes (4 for `crc32c`, 8 for
   `xxh3`): the value prefix;
2. the column position, as little-endian bytes of the same width;
3. the canonical encoding of the value (for scalars, the bytes of the send
   function, in one step).

A row hashes its column hashes in order, as little-endian bytes.  "Continuing"
means the CRC-32C register for `crc32c`, so for it every sequence of steps
equals one CRC-32C over the concatenation of their bytes (as with FNV-1a);
for `xxh3` it means the previous result is the seed of the next XXH3 call.
The `checksum_fastpath` regression test recomputes cell and tuple checksums
from these rules with `pg_data_checksum` and the types' send functions, for
every algorithm.

Logical `crc32c` and `xxh3` checksums are portable like the FNV ones: the
same on every platform, byte order and cluster.  Physical ones describe one
cluster, as with FNV.

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
  output function when no send function exists.  Text types are hashed in the
  server encoding, independently of `client_encoding`; see
  [Operations](operations.md#limitations-and-portability).
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
| `pg_tuple_*_checksum`    |    O(tuple size) | Reads one heap page |
| `pg_table_*_checksum`    |   O(rows × columns) | Order-independent aggregation |
| `pg_index_physical_checksum` | O(index size) | Bulk read strategy |
| `pg_index_logical_checksum` | O(table rows) | Scans heap rows, not index pages |
| `pg_database_*_checksum` | O(total DB size) | Periodic interrupt checks |

Table, index, and database scans can be parallelized with
`pg_integrity_checksums.workers`; the scan work is divided among the workers with
close to linear speedup up to the point where the scan becomes I/O-bound.  See
[Operations](operations.md#parallel-computation) for how database checksums
use workers, and `bench/` in the repository
([Testing](testing.md#benchmark)) to measure a change.

---
