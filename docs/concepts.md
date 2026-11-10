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

Page checksums are PostgreSQL’s built-in **16-bit** page checksum.  Page-level
64-bit checksums do not exist in this extension; the other levels have both
physical and logical forms.  `pg_page_checksum` calculates the checksum but
does not compare it with the stored `pd_checksum`; page verification is a
separate operation.

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

The extension provides two forms of the same functions.

### Default 32-bit API

The default functions use FNV-1a 32-bit and return an ordinary SQL `integer`
or `bigint`:

~~~sql
SELECT pg_table_logical_checksum('orders'::regclass);
SELECT pg_table_physical_checksum('orders'::regclass, false);
~~~

This is the simplest interface and the right default for most users.  A
32-bit checksum is sufficient for detecting accidental data changes and
corruption; it is also the format used by v1 CLI baselines.

### Optional 64-bit API

The generic overloads take an algorithm name and return raw `bytea`:

~~~sql
SELECT pg_table_logical_checksum('orders'::regclass, 'fnv64');
SELECT pg_table_logical_checksum('orders'::regclass, 'fnv32');
~~~

Choose this form when you need:

- a wider 64-bit digest (`fnv64`);
- an explicit algorithm in the call;
- a cross-system format that is not tied to a SQL integer type;
- a v2 CLI baseline.

Digests are big-endian bytes: `fnv32` is 4 bytes and `fnv64` is 8 bytes.
Table and database physical checksums always return 8 bytes because the
relation/database identity is part of the value.

The 32-bit and 64-bit paths share the same canonical value handling and
aggregation rules.  A 64-bit digest is wider, but FNV-1a remains a
non-cryptographic checksum.

### Converting a digest to `bigint`

~~~sql
SELECT pg_checksum_bigint(pg_table_logical_checksum('orders'::regclass, 'fnv64'));
~~~

For 4-byte `fnv32` digests, the one-argument form sign-extends.  Use
`pg_checksum_bigint(digest, true)` when you need the unsigned value (for
example, to compare with a table logical checksum from the default API).

The detailed signatures are in the [function reference](functions.md).

## Collision probability

A 32-bit checksum has `2^32` possible outputs.  For an unrelated accidental
change, the rough chance of an unnoticed collision is about `2^-32`, or
`2.3e-10`.  For a 64-bit checksum it is about `2^-64`, or `5.4e-20`.

The final table/database checksum is still only as wide as the selected
output.  These numbers are approximate and assume a random change; FNV-1a is
not cryptographic, and an adversary who can recompute the checksum can
produce a collision.  The 32-bit default is adequate for ordinary change and
corruption detection.

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
