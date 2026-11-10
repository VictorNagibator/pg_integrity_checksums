# Testing and releases

## Test Suite

The pg_integrity_checksums extension includes a comprehensive test suite that validates functionality at all granularity levels and ensures algorithm correctness. The tests are divided into two categories: regression tests (SQL-based) and TAP tests (Perl-based).

```bash
# Run the regression tests
make installcheck

# Run the regression and TAP tests (TAP needs the IPC::Run Perl module and the
# PostgreSQL test modules shipped with the matching server release)
make installcheck enable_tap_tests=yes PROVE=prove
```

On merges to `main` and on releases, CI runs both suites on every supported
major version (PostgreSQL 12 through 19) on Linux x86_64 and ARM64, macOS
ARM64, Windows x64, and FreeBSD amd64 and ARM64.  Pull requests use a reduced
PostgreSQL 12/16/19 matrix on the same platforms; the full matrix runs after
merge.  [CodeQL](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/.github/workflows/codeql.yml) analyzes the C and Go sources, the
[Quality workflow](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/.github/workflows/quality.yml) runs ASAN/UBSAN and coverage
builds for PostgreSQL 12–19, and the
[Docker workflow](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/.github/workflows/docker.yml) builds PG 12–18 images and
smoke-tests PostgreSQL 16 on pull requests.  The CLI has its own
[CLI workflow](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/.github/workflows/cli.yml), the
[CLI integration matrix](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/.github/workflows/cli-integration.yml) for PostgreSQL
12–19, and independent `cli-v*` releases.

Logical checksums are canonical across endianness: the same schema and data
produce the same logical values on little-endian and big-endian systems.
Physical checksums are storage- and architecture-dependent by design.

**Tests cover:**

1. Basic functionality at all granularity levels
2. Physical and logical checksum differences
3. NULL handling and type support
4. MVCC behavior and concurrency
5. Stability of logical checksums across physical reorganizations (VACUUM, CLUSTER, REINDEX)
6. Error conditions and edge cases
7. Parallel vs serial checksum consistency at table, index and database levels
8. Partitioned tables and indexes (single- and multi-level, with and without
   primary keys)
9. Safety and validation: logical index checksums work for every access
   method; invalid block numbers/TIDs are rejected; views/sequences/indexes
   are rejected by table functions; SELECT privileges and RLS are enforced;
   and TOAST physical checksums are computed from raw pages
10. Corrupt page handling: unsafe page headers and line pointers produce clean
    errors; database scans skip the affected relation with a warning
11. Partitioned index aggregation, including deterministic physical and
    logical checksums for partitioned indexes
12. Canonical logical checksums: no-PK tables, duplicate rows, empty and
    partitioned tables, enum/numeric/array/composite/range values, all logical
    index AMs, expression/partial/INCLUDE indexes, and serial/parallel database
    consistency
13. System-catalog checksums, including anyarray/aclitem values in
    pg_statistic and pg_attribute; Hash index logical checksums match B-tree
    checksums for the same input and are independent of the index AM
14. DDL stability: table/column/index renames, added/dropped columns, type
    changes, fillfactor, index creation, TRUNCATE and enum value additions
15. Cross-database portability: identical schema/data in two databases with
    different type, table and index OIDs produces equal logical checksums and
    different physical checksums
16. Privilege boundaries: column-only SELECT is rejected; table SELECT is
    required for table, page, index, tuple and cell checksums
17. TOAST, zero-column tables, all-dropped-column tables, materialized views,
    invalid indexes and empty partitioned indexes
18. Fixed cross-version logical baselines: the same schema/data gives the same
    table, index, tuple and cell checksums on every supported major version
19. Property/fuzz coverage of canonical encodings: numeric scale, array
    literals, ranges, composites and enums, plus repeated scans of random data
20. Endian-stable logical checksums for arrays, composites and multiranges,
    with fixed baselines checked on a big-endian Linux s390x test
21. FNV-1a 64-bit vectors, generic `fnv32`/`fnv64` overloads, empty/NULL
    inputs, TOAST, partitioning, and serial/parallel 64-bit consistency
22. CLI v2 baselines: `capture --hash fnv64`, verify/update inheritance and
    rejection of mixed-hash diffs
23. Streaming standby: logical table, index and database checksums match the
    primary after WAL replay

## Building a Release

The `dist` target packs the current `HEAD` into the archive that PGXN expects
(`pg_integrity_checksums-<version>.zip`, with everything under a `pg_integrity_checksums-<version>/`
directory):

```bash
make dist
```

### Automated releases

Pushing a `v*` tag runs the [Release workflow](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/.github/workflows/release.yml),
which:

1. checks that the tag, `pg_integrity_checksums.control` and `META.json` all carry the
   same version;
2. builds the archive with `make dist` and verifies its contents;
3. takes the release notes from the matching `Changes` entry (the release fails
   if there is none, rather than publishing an empty note);
4. creates the GitHub Release and attaches the archive (tags containing
   `-rc`, `-beta` or `-alpha` are marked as pre-releases);
5. uploads the archive to [PGXN](https://manager.pgxn.org/) when the
   `PGXN_USERNAME` and `PGXN_PASSWORD` repository secrets are configured.

---

## Repository checks

Every relevant change runs:

- `actionlint` for all GitHub Actions workflows;
- CLI unit tests with `-race` and a coverage upload to Codecov;
- the PostgreSQL 12-19 CLI integration matrix;
- a Docker CLI image smoke test;
- a local composite-action smoke test on PostgreSQL 16.

GitHub Actions are pinned to commit SHAs and updated by Dependabot.  Go
dependencies are updated by Dependabot as well.
