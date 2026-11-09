# Architecture

This page describes internals for contributors and advanced users.  New
readers should start with [Getting started](getting-started.md) and
[Concepts](concepts.md).

## Overview

`pg_integrity_checksums` is a PostgreSQL extension that provides checksums at six granularity levels. Page checksums are physical, cell checksums are logical, and tuple, table, index and database checksums are available in both physical and logical variants. It enables data integrity verification, change detection, consistency checking, and corruption detection across the database hierarchy.

---

## Key Features

### Multi-Granularity & Dual-Mode Support

* **Six Granularity Levels**: Page, Cell, Tuple, Table, Index, and Database
* **Dual Checksum Types**: Physical (storage-dependent) and Logical (content-dependent)
* **Type-Aware**: Full support for all PostgreSQL data types
* **NULL-Aware**: Special `CHECKSUM_NULL` constant for NULL values
* **MVCC-Aware**: Incorporates transaction visibility in physical checksums
* **No Primary Key Required**: Logical table/database checksums hash row
  content directly and preserve duplicate rows with a sum-plus-count aggregate
* **Portable Content**: Logical checksums omit relation OIDs, heap TIDs and
  type OIDs; enum labels, numeric scale, arrays, composites and ranges are
  canonicalized
* **Endian-Independent**: Fixed-width integers in the canonical form use a
  little-endian byte order, so logical values match on little-endian and
  big-endian systems
* **All Logical Index Access Methods**: Logical index checksums are derived
  from table data and the index definition, so B-tree, Hash, GiST, GIN,
  SP-GiST, BRIN and custom AMs are supported
* **Selectable Hash Width**: Legacy SQL functions use FNV-1a 32-bit;
  generic overloads add FNV-1a 64-bit raw `bytea` digests.  Page checksums
  remain PostgreSQL's built-in 32-bit checksum.
* **Parallel Computation**: Table, index and database checksums can be
  computed with PostgreSQL parallel workers for faster scans of large
  relations
* **Partitioned Support**: Table and index checksums transparently aggregate
  the checksums of all leaf partitions/indexes
* **Strict Mode**: `pg_integrity_checksums.strict` makes database scans fail instead of
  skipping relations that cannot be checksummed

---

## Physical and Logical Checksums

### Physical Checksums

* **Depend on**: Physical storage layout (block numbers, offsets, page headers)
* **Change when**: tables are rewritten or moved (`VACUUM FULL`, `CLUSTER`,
  physical data movement), or physical index pages are rebuilt (`REINDEX`)
* **Useful for**: Physical corruption detection, storage integrity
* **Includes**: Location encoding, MVCC information, page structure

### Logical Checksums

* **Depend on**: Only logical data content.  Relation OIDs, heap TIDs and
  type OIDs are excluded.
* **Stable across**: `VACUUM`, `VACUUM FULL`, `CLUSTER` and `REINDEX`.
* **Useful for**: Logical consistency, replication validation, migration and
  cross-database content comparison.
* **No primary key required**: tables without a PK are covered.
* **Index checksums**: derived from visible table rows and the index
  definition, not from physical index pages.  This works for every index AM
  and is stable across `VACUUM`/`REINDEX`.  Physical index checksums remain
  the tool for detecting missing or corrupt index entries.

---
