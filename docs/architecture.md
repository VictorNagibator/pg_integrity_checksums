# Architecture and internals

This page describes how checksums are computed, aggregated and parallelized.
It is intended for contributors and advanced users; start with
[Getting started](getting-started.md) and [Concepts](concepts.md) for usage.

## Scope

The extension exposes six levels of checksums:

| Level | Physical | Logical |
|---|---:|---:|
| Page | yes (PostgreSQL 16-bit page checksum) | — |
| Cell | — | yes |
| Tuple | yes | yes |
| Table | yes | yes |
| Index | yes | yes |
| Database | yes | yes |

Page checksums are PostgreSQL's built-in checksum and do not use the
extension's FNV paths.  The other levels use FNV-1a 32-bit by default and can
select FNV-1a 64-bit through the generic overloads.

## Execution model

Table, index and database functions follow the same basic pipeline:

1. Open the relation or enumerate relations under `AccessShareLock`.
2. Obtain a snapshot from the active statement or the surrounding
   transaction.
3. Scan heap blocks (or index pages), validate page and tuple structure, and
   skip tuples that are not visible to the snapshot.
4. Canonicalize the selected values for logical checksums.
5. Fold each per-row or per-page value into an order-independent accumulator.
6. Finalize the accumulator and combine relation/storage metadata where the
   selected level requires it.

Table and index scans use the table access method and heap visibility rules.
Index logical checksums evaluate the index definition on visible heap rows
rather than reading index pages; index physical checksums read the index
pages directly.

## Canonical logical encoding

The goal is a value that is independent of relation OIDs, heap TIDs, tuple
headers and physical layout.  The encoder:

- uses a type tag derived from the type name, not the type OID;
- uses the type's binary send function, or its output function when no send
  function exists;
- recursively encodes arrays, composites, domains, ranges and multiranges;
- hashes enum labels rather than enum OIDs;
- normalizes `numeric` with `numeric_normalize()`, so `1.0` and `1.00` match;
- canonicalizes NaN and negative zero for floating-point values;
- writes fixed-width shape/count integers in little-endian byte order.

These rules are shared by the 32-bit and 64-bit logical paths.  The only
difference is the hash width and finalizer.

## Physical encoding

Physical checksums include the information that identifies a particular
storage layout:

- block number and tuple offset;
- relation OID, relfilenode and tablespace;
- tuple bytes and optionally the tuple header;
- MVCC fields stored in the tuple header;
- index page bytes and page layout metadata for index physical checksums.

Physical checksums deliberately omit state that changes without any change
to the stored data:

- tuple hashing leaves out the page LSN, `pd_checksum` and `pd_flags`, and
  masks the tuple's infomask hint bits;
- index pages and raw TOAST pages are hashed from a masked copy: the page LSN,
  `pd_checksum`, `pd_prune_xid` and the `pd_flags` hint bits are cleared, as
  are the hints index scans set under a share lock (`LP_DEAD` line pointers,
  the B-tree `BTP_HAS_GARBAGE`, hash `LH_PAGE_HAS_DEAD_TUPLES` and GiST
  `F_HAS_GARBAGE` flags) and the infomask hint bits of TOAST tuples.

The in-buffer `pd_checksum` matters even when nothing is modified:
PostgreSQL sets the checksum on the copy of the page it writes out, so the
value in shared buffers is stale until the page is evicted and read back.
Hashing it made physical checksums change across a server restart.

A physical checksum identifies one cluster's storage and is not a
replica-comparison value.

## Aggregation

Physical and index aggregates are order-independent.  Each 32-bit or 64-bit
value is passed through an avalanche mixer and combined with XOR, so the
result does not depend on scan order, worker boundaries or partition order.

Logical table, index and database aggregates use a sum plus count:

~~~text
sum   = sum + hash(value)
count = count + 1
final = mix(sum) XOR mix(count)
~~~

Addition is still commutative, while the count preserves duplicate rows that
a pure XOR would cancel.  The finalizer is `hash_mix32` for the default API
and `hash_mix64` for the 64-bit API.

## Parallel execution

Parallel scans use PostgreSQL's dynamic shared-memory parallel context:

- the leader publishes a `ChecksumParallelState` with the task type,
  relation OID, algorithm and an atomic work counter;
- workers claim block ranges (or relations) atomically;
- the snapshot is serialized once and restored by every worker;
- each worker returns a fixed-size partial aggregate and count;
- the leader merges partials with XOR (physical) or sum/count (logical).

The same worker implementation handles 32-bit and 64-bit scans.  Parallelism
is disabled when already inside a worker, because parallel workers cannot
start nested parallel workers.

## Snapshots, locks and privileges

A single SQL statement uses one snapshot.  Separate calls under `READ
COMMITTED` can see different states; use a `REPEATABLE READ READ ONLY`
transaction when several checksum calls must describe one point in time.
Long repeatable-read transactions hold an `xmin` horizon and can delay
`VACUUM`.

Scans take `AccessShareLock` on relations.  Table, page, cell, tuple and index
functions require `SELECT` and reject row-level-security relations because a
direct heap scan cannot apply RLS policies.  Database-level functions require
superuser.

Pages are validated and tuple visibility is checked under a buffer share
lock.  Logical checksums then hash the visible tuples with the buffer only
pinned, like a page-at-a-time sequential scan: canonicalization detoasts
values, calls type send functions and evaluates index expressions, and none
of that may run under a buffer content lock (interrupts are held off, so the
call could not be cancelled, and reading other buffers or the catalogs under
the lock risks an undetectable lock-ordering deadlock).  Physical tuple
hashing only reads the page and stays under the share lock, because it
includes header fields such as `xmax` that concurrent backends change.

## Error handling

Corrupt page headers and line pointers are validated before dereference and
raise `DATA_CORRUPTED`.  Database scans run each relation in an internal
subtransaction where PostgreSQL permits it, so one unsupported or
concurrently dropped relation does not abort the whole scan.  Cancellation
and statement timeout are propagated instead of returning a partial result.

## Page checksums

`pg_page_checksum` calculates PostgreSQL's built-in **16-bit** page checksum.
It does not compare the result with the stored `pd_checksum` and returns `0`
for a new page.  `pg_page_checksum_status` performs the comparison and reports
`ok`, `mismatch`, `new` or `checksums_disabled`; it is the verification API.

## Source layout

The implementation is split into a small set of translation units:

| File | Responsibility |
|---|---|
| `pg_integrity_checksums.c` | module entry point, GUCs and shared worker/snapshot helpers |
| `checksum_hash.c` | FNV-1a, hash combination, order-independent aggregation |
| `checksum_types.c` | canonical encoding for scalar, array, composite, range and multirange values |
| `checksum_validation.c` | relation, page, line-pointer and tuple validation |
| `checksum_page.c` | page checksum verification and canonical cell checksums |
| `checksum_tuple.c` | physical and logical tuple checksums |
| `checksum_scan.c` | serial block-range scans |
| `checksum_table.c` | table checksums and partition helpers |
| `checksum_index.c` | physical/logical index checksums and index scans |
| `checksum_parallel.c` | dynamic shared-memory parallel execution |
| `checksum_database.c` | database enumeration and database SQL entry points |
| `checksum_utility.c` | generic data/text checksum helpers |
| `checksum_internal.h` | internal interfaces and state shared by the modules |

The split is a maintainability refactor, not a behavioral change.

## Related documentation

- [Concepts](concepts.md) — logical vs physical and API selection.
- [Function reference](functions.md) — signatures and return types.
- [Operations](operations.md) — limits, snapshots, performance.
