# Verifying a logical replica

This recipe checks that the tables of a logical replication subscriber hold
the same rows as on the publisher, and finds the rows that differ when they
do not.  It compares logical checksums, which do not depend on OIDs, row
positions or `VACUUM`, so they are equal on both sides when the rows are.
Physical checksums always differ between two servers; do not use them here.

The examples use a publication `app_pub` with the tables `public.orders`
(primary key `id`) and `public.events` (no primary key, `REPLICA IDENTITY
FULL`), and a subscription `app_sub` in the database `app` of the subscriber.
The output is from PostgreSQL 19 with extension 2.9.1.

## 1. Check the prerequisites

The checksums of a table are equal on both sides only when everything that
goes into them is equal:

- **The same extension version** (`SELECT extversion FROM pg_extension WHERE
  extname = 'pg_integrity_checksums'`).  Some upgrades change values; see
  [Which upgrades change checksum values](upgrading.md#which-upgrades-change-checksum-values).
- **The same server encoding** (`SHOW server_encoding`).  Text is hashed as
  stored.
- **The same table definitions**: the same column names and types, in the
  same order.  Logical replication tolerates a subscriber table with its
  columns in another order or with extra columns, but the checksum of a row
  includes all its columns in order, so such a table always has a different
  checksum.  Create the subscriber's tables from the publisher's schema, for
  example with `pg_dump --schema-only`.
- **The whole table is published.**  A publication with a row filter
  (`WHERE`) or a column list sends only part of the table, so the table
  checksums differ by design.  The same applies when the publication does
  not publish `TRUNCATE`, or the subscription was created with
  `copy_data = false` on a table that already had rows.

`TimeZone`, `DateStyle` and the other session settings do not matter: values
are hashed in their binary form.

## 2. Wait until the subscriber has caught up

Rows that the subscriber has not applied yet are real differences.  For an
exact comparison, stop writes to the published tables (or pick a quiet
moment) and wait until the subscriber has confirmed everything up to the
publisher's current WAL position.

On the subscriber, every table must be in state `r` (ready; the initial copy
is done) and the subscription enabled:

```sql
SELECT srrelid::regclass AS table_name, srsubstate
FROM pg_subscription_rel ORDER BY 1::text;
```

```text
 table_name | srsubstate
------------+------------
 events     | r
 orders     | r
(2 rows)
```

On the publisher, after the last write, take the current WAL position and
check that the subscription's slot has confirmed it.  In `psql`:

```sql
SELECT pg_current_wal_lsn() AS target \gset

SELECT slot_name, confirmed_flush_lsn,
       confirmed_flush_lsn >= :'target' AS caught_up
FROM pg_replication_slots WHERE slot_name = 'app_sub';
```

```text
 slot_name | confirmed_flush_lsn | caught_up
-----------+---------------------+-----------
 app_sub   | 0/02DEC778          | t
(1 row)
```

Repeat the second query until `caught_up` is `t`.  A subscription that stays
behind is usually stopped by an apply error (a conflict); the subscriber's
server log says which.

## 3. Compare the table checksums

On the publisher, checksum every table of the publication:

```sql
SELECT pt.schemaname || '.' || pt.tablename AS table_name,
       pg_table_logical_checksum(
           format('%I.%I', pt.schemaname, pt.tablename)::regclass,
           'fnv64') AS checksum
FROM pg_publication_tables pt
WHERE pt.pubname = 'app_pub'
ORDER BY 1;
```

```text
  table_name   |      checksum
---------------+--------------------
 public.events | \xaa1f15acb0ce8b1d
 public.orders | \x0df943b3c49f04ca
(2 rows)
```

On the subscriber, checksum every table of the subscription:

```sql
SELECT n.nspname || '.' || c.relname AS table_name,
       pg_table_logical_checksum(c.oid, 'fnv64') AS checksum
FROM pg_subscription_rel sr
JOIN pg_subscription s ON s.oid = sr.srsubid
JOIN pg_class c ON c.oid = sr.srrelid
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE s.subname = 'app_sub'
ORDER BY 1;
```

```text
  table_name   |      checksum
---------------+--------------------
 public.events | \xaa1f15acb0ce8b1d
 public.orders | \x3d04d4a7376c4adb
(2 rows)
```

`public.events` matches.  `public.orders` does not: the two copies differ in
at least one row.  The 64-bit `fnv64` checksum misses a difference with
probability about 2^-64; the default 32-bit functions work too, with
probability about 2^-32.

The tables need `SELECT` for the role that runs the queries, and must not
have row-level security enabled for it.  For many tables, set
`pg_integrity_checksums.workers` to scan each table in parallel (see
[Parallel computation](operations.md#parallel-computation)).

## 4. What a mismatch means

With the prerequisites met and the subscriber caught up, a mismatch means
the rows really differ.  Common causes:

- **Writes on the subscriber.**  A subscriber is an ordinary writable
  database; an application, a migration or a person changed rows there.
- **Skipped or failed changes.**  A conflict stopped the apply worker and the
  transaction was skipped (`ALTER SUBSCRIPTION ... SKIP`), or
  `disable_on_error` disabled the subscription.
- **Triggers or rules on the subscriber** that are enabled for replication
  (`ENABLE ALWAYS` or `ENABLE REPLICA`) and modify rows.
- **A prerequisite that does not hold**: different column order, extra
  columns, a row filter or column list, a different extension version.

Sequences are not part of a publication of tables: the subscriber's
sequences keep their own values, so `pg_sequence_logical_checksum` (and a
CLI baseline that includes sequences) differs even when every table
matches.

## 5. Find the differing rows

Split the table into buckets of its key hash and compare the buckets, then
compare the rows of the buckets that differ.  The procedure in
[Locating differing rows](operations.md#7-locating-differing-rows-between-two-servers)
does this with [`dblink`](https://www.postgresql.org/docs/current/dblink.html).
Run on the subscriber, with `:'pub'` set to a connection string for the
publisher (`\set pub 'host=primary dbname=app'` in `psql`):

```sql
CREATE TEMP TABLE differing AS
SELECT s.bucket, p.row_count AS publisher_rows, s.row_count AS subscriber_rows
FROM pg_table_bucket_checksums('orders', 1024) s
JOIN dblink(:'pub',
            $$SELECT bucket, row_count, checksum
              FROM pg_table_bucket_checksums('orders', 1024)$$)
     AS p(bucket integer, row_count bigint, checksum bytea)
  USING (bucket)
WHERE s.checksum <> p.checksum;

TABLE differing ORDER BY bucket;
```

```text
 bucket | publisher_rows | subscriber_rows
--------+----------------+-----------------
    470 |             94 |              93
    513 |             90 |              90
    851 |             98 |              99
(3 rows)
```

Three of the 1024 buckets differ.  Then fetch the rows of those buckets only,
and match them by key hash:

```sql
WITH sel AS (SELECT array_agg(bucket) AS b FROM differing),
s AS (SELECT r.* FROM sel, pg_table_row_checksums('orders', 1024, sel.b) r),
p AS (SELECT x.* FROM sel, dblink(:'pub',
        format('SELECT key_hash, key, row_checksum
                  FROM pg_table_row_checksums(%L, 1024, %L)',
               'orders', sel.b))
        AS x(key_hash bytea, key text, row_checksum bytea))
SELECT coalesce(p.key, s.key) AS key,
       CASE WHEN s.key IS NULL THEN 'missing on subscriber'
            WHEN p.key IS NULL THEN 'only on subscriber'
            ELSE 'changed' END AS difference
FROM p FULL JOIN s USING (key_hash)
WHERE p.row_checksum IS DISTINCT FROM s.row_checksum
ORDER BY 1;
```

```text
   key    |      difference
----------+-----------------------
 (12345)  | missing on subscriber
 (300000) | only on subscriber
 (777)    | changed
(3 rows)
```

The key is the primary key.  For a table without one, such as
`public.events`, it is the whole row, and a changed row shows up as one row
missing and one only on the subscriber.  Pass `key_columns` (the same on
both sides) to use another unique key; see
[Finding differing rows](functions.md#finding-differing-rows).

Each call scans the whole table.  The buckets are narrowed per call, so this
costs two scans of the table on each side whatever the number of buckets.
Pick the number so that a bucket holds a few hundred to a few thousand rows.

## Or use the CLI

[`pg_integrity_checksums compare`](cli.md#compare) runs steps 3 and 5 for
every table of the selected schemas and reports the differing rows by key.
It also reports tables whose columns differ as `schema_differs` instead of
comparing them:

```bash
pg_integrity_checksums compare \
  --source "postgresql://primary/app" \
  --target "postgresql://replica/app" \
  --schema public --show-ok
```

```text
EQUAL          public.events rows=20000
DIFFERENT      public.orders source_rows=100000 target_rows=100000 differing_buckets=3/1024
  MISSING  (12345)
  CHANGED  (777)
  EXTRA    (300000)
tables=2 equal=1 different=1 only_in_source=0 only_in_target=0 schema_differs=0 errors=0 missing_rows=1 extra_rows=1 changed_rows=1
```

It exits with code `1` when anything differs.  The catch-up check of step 2
still applies: each side is read in its own snapshot.
