# Concepts

This page explains the ideas behind the functions.  Read it once, then use the
[function reference](functions.md) as a lookup table.

## Checksum levels

The extension works at six levels:

| Level | Physical | Logical | Typical question |
|---|---:|---:|---|
| Page | yes | — | Is this one storage page intact? |
| Cell | — | yes | Does this one value have the expected hash? |
| Tuple | yes | yes | Does this row still match its expected bytes or content? |
| Table | yes | yes | Did the table’s storage or logical content change? |
| Index | yes | yes | Is the physical index intact, or does it contain the expected entries? |
| Database | yes | yes | Did anything in the database change? |

Page checksums are PostgreSQL’s built-in page checksum.  Page-level 64-bit
checksums do not exist in this extension; the other levels have both physical
and logical forms.

## Logical vs physical

### Logical checksums

A logical checksum is a fingerprint of **data content**.  It deliberately
excludes:

- heap TIDs and tuple locations;
- relation and type OIDs;
- page layout and MVCC headers;
- physical row order.

It is stable across `VACUUM`, `VACUUM FULL`, `CLUSTER`, `REINDEX`, dump/restore
and OID changes.  That makes it suitable for:

- comparing a primary and a replica;
- checking data after a restore or migration;
- detecting logical data changes in CI;
- comparing the same schema in two databases.

A primary key is **not required**.  Duplicate rows are preserved by a sum plus
count aggregate, so two identical rows contribute twice.

### Physical checksums

A physical checksum is a fingerprint of one cluster’s storage.  It includes
physical locations, tuple headers, MVCC fields, page/index bytes and relation
storage identity.  It changes after:

- `VACUUM FULL`, `CLUSTER` or a table rewrite;
- `REINDEX`;
- moving a tuple to another block or offset;
- changing the relation’s relfilenode or tablespace.

Use physical checksums to detect on-disk corruption or to confirm that a
maintenance operation changed exactly what you expected.  Do **not** compare
physical checksums between different clusters, replicas or databases.

### A simple rule

| You want to know... | Use |
|---|---|
| Has the data changed? | logical |
| Has this cluster’s storage changed? | physical |
| Is an index entry missing or corrupt? | physical index |
| Does the index logically match the table? | logical index |

## Which API should I use?

There are two SQL API styles.

### Generic API (`fnv64` or `fnv32`)

Generic overloads take an algorithm name and return a raw `bytea` digest:

~~~sql
SELECT pg_table_logical_checksum('orders'::regclass, 'fnv64');
SELECT pg_table_logical_checksum('orders'::regclass, 'fnv32');
~~~

Use `fnv64` for new work when a 64-bit digest is useful.  `fnv32` is the
generic form of the historical 32-bit algorithm.  The generic API is also the
only way to select an algorithm explicitly.

Digests are big-endian bytes:

- `fnv32` — 4 bytes;
- `fnv64` — 8 bytes.

Table and database physical checksums always return 8 bytes because the
relation/database identity is part of the value.

### Original numeric API

The original functions return `integer` or `bigint` and always use FNV-1a
32-bit:

~~~sql
SELECT pg_table_logical_checksum('orders'::regclass);
~~~

These functions remain supported for existing baselines and integrations.
They are documented in the [numeric API reference](functions.md#numeric-api-and-page-checksums).
New code that needs 64-bit digests should use the generic API.

### Converting a digest to `bigint`

~~~sql
SELECT pg_checksum_bigint(pg_table_logical_checksum('orders'::regclass, 'fnv64'));
~~~

For 4-byte `fnv32` digests, the one-argument form sign-extends.  Use
`pg_checksum_bigint(digest, true)` when you need the unsigned value (for
example, to compare with a historical table logical checksum).

## How aggregation works

Table, index and database checksums are order-independent:

- physical aggregates combine hashes with XOR;
- logical aggregates use sum plus count.

The count is important: a pure XOR would cancel duplicate rows.

Parallel workers split the scan by blocks or relations and the leader combines
their partial results.  With `pg_integrity_checksums.workers > 0`, serial and
parallel executions produce identical checksums, for both 32-bit and 64-bit
paths.

## Baseline formats

The CLI stores a snapshot in a versioned JSON baseline.

| Format | Algorithm | Written by |
|---|---|---|
| `v1` | FNV-1a 32-bit | `capture` by default |
| `v2` | explicit `hash` (`fnv32` or `fnv64`) | `capture --hash fnv64` |

`verify` and `update` inherit the algorithm from the baseline they read.
Older CLI releases reject v2 instead of comparing incompatible digests.

See [CLI baseline formats](cli.md#baseline-formats).

## What this is not

- FNV-1a is **not cryptographic**.  It detects accidental changes, not a
  malicious attacker who can recompute the hash.
- Physical checksums are tied to one cluster and database.
- Logical checksums describe content, not physical layout.
- The extension does not replace backups, `pg_checksums` or `amcheck`.

## Permissions and safety

Table, page, cell, tuple and index checksums read relation storage directly:

- they require `SELECT` on the relation (or the base table for an index);
- relations with row-level security are rejected, because a direct heap scan
  cannot apply RLS policies;
- database-level functions require superuser.

Capture and verify take one repeatable-read, read-only snapshot.  The CLI
never modifies application data.

## Read next

- [Getting started](getting-started.md) — run your first checksum.
- [Function reference](functions.md) — exact signatures and return types.
- [Operations](operations.md) — limitations, performance and use cases.
- [Troubleshooting](troubleshooting.md) — common errors.
