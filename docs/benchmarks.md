# Benchmarks

How long a checksum takes, compared with reading the same data.  The
measure that transfers to other hardware is the ratio to a sequential scan
(`x seq scan`): the checksum reads every page just like the scan, so the
ratio shows what hashing adds on top of the I/O.

## Setup

- AMD Ryzen 5 5600 (6 cores, 12 threads), Linux under WSL2 with 8 GB of
  memory, data on the WSL virtual disk on an NVMe SSD.
- PostgreSQL 18.7 built from source, `shared_buffers = 2GB`,
  `--data-checksums`; pg_integrity_checksums 2.9.1, which
  reads blocks through a read stream (see
  [Before the read stream](#before-the-read-stream) for 2.9.0).
- Data: `pgbench -i`, table `pgbench_accounts` (an integer key, two integers
  and a `char(84)` column) with its primary key, frozen and analyzed.
- Each case runs once to warm up, then three times; the table shows the
  median.  `workers = 4` sets both `max_parallel_workers_per_gather` (for the
  plain SQL queries) and `pg_integrity_checksums.workers`.

The cases:

| Case | Query |
|---|---|
| seq scan | `SELECT count(*) FROM pgbench_accounts` |
| SQL hash | `SELECT sum(hashtextextended(t::text, 0)) FROM pgbench_accounts t`, a hand-written order-independent row hash |
| table logical | `pg_table_logical_checksum('pgbench_accounts')`, default `fnv32`, and with `'xxh3'` |
| table physical | `pg_table_physical_checksum('pgbench_accounts', false)` |
| database logical | `pg_database_logical_checksum()`, all user tables and indexes |

## About 1 GB (fits in memory)

`pgbench -i -s 68`: 871 MB heap, 146 MB of indexes.

| Case | serial, ms | x seq scan | 4 workers, ms | x seq scan |
|---|---:|---:|---:|---:|
| seq scan | 301 | 1.0 | 208 | 1.0 |
| SQL hash | 3413 | 11.3 | 841 | 4.0 |
| table logical, fnv32 | 1080 | 3.6 | 252 | 1.2 |
| table logical, xxh3 | 769 | 2.6 | 201 | 1.0 |
| table physical, fnv32 | 977 | 3.2 | 204 | 1.0 |
| database logical | 1292 | 4.3 | 1043 | 5.0 |

## About 10 GB (larger than memory)

`pgbench -i -s 680`: 8711 MB heap, 1457 MB of indexes, more than the
machine's memory, so most of every run reads from disk.

| Case | serial, ms | x seq scan | 4 workers, ms | x seq scan |
|---|---:|---:|---:|---:|
| seq scan | 3052 | 1.0 | 1994 | 1.0 |
| SQL hash | 35805 | 11.7 | 10728 | 5.4 |
| table logical, fnv32 | 10671 | 3.5 | 7304 | 3.7 |
| table logical, xxh3 | 8783 | 2.9 | 6277 | 3.1 |
| table physical, fnv32 | 9461 | 3.1 | 6614 | 3.3 |
| database logical | 13067 | 4.3 | 10566 | 5.3 |

Timings of the 10 GB runs vary by up to about 30% from run to run on this
machine, depending on what the operating system still has cached.

## Reading the results

- On one core a logical table checksum costs about three sequential scans,
  in memory and from disk alike: the read stream keeps the disk busy while
  the rows are hashed.  It is about three times faster than hashing
  `row::text` in SQL, and does not depend on the text output format,
  `DateStyle` or other session settings.
- With workers and the data in memory, a table checksum takes about as long
  as a parallel sequential scan.  From disk the workers help less: a
  parallel table checksum takes about three times a parallel sequential
  scan.
- `xxh3` is faster than the default `fnv32` on this table because of its
  84-character column; on tables of short integers and short strings
  `fnv32` can be faster (see [Concepts](concepts.md#generic-api-choose-the-algorithm)).
- A database checksum gives whole relations to its workers, so a database
  that is mostly one large table gains little from them.  To checksum such a
  table in parallel, call `pg_table_logical_checksum` for it with workers set.

## Before the read stream

Up to 2.9.0 the extension read one block at a time.  In memory that makes
no difference; from disk, on the 10 GB data set, it took about twice as
long serially:

| Case | 2.9.0 serial, ms | read stream, ms | 2.9.0, 4 workers, ms | read stream, ms |
|---|---:|---:|---:|---:|
| table logical, fnv32 | 21233 | 10671 | 7520 | 7304 |
| table logical, xxh3 | 19009 | 8783 | 7168 | 6277 |
| table physical, fnv32 | 19797 | 9461 | 7235 | 6614 |
| database logical | 22187 | 13067 | 16529 | 10566 |

PostgreSQL 12 to 16 have no read stream and still read block by block.

## Run it yourself

`bench/compare.sh` loads the data and prints both tables, against a running
server with the extension installed (libpq environment):

```bash
bench/compare.sh                    # pgbench scale 68, about 1 GB
SCALE=680 bench/compare.sh          # about 10 GB
SETUP=no WORKERS="0 2 4 8" bench/compare.sh   # reuse the data, more worker counts
```

`bench/run_bench.sh` measures every function on several data types and is
meant for comparing two builds; see [Testing](testing.md#benchmark).
