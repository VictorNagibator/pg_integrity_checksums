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

If your PostgreSQL installation is not writable by you (a system package, or a
tree installed by root), run the TAP tests against a private copy: an install
tree is relocatable, so copy it, install the extension into the copy and run
`prove` with that copy's `bin` first on `PATH`:

```bash
cp -a /usr/local/pgsql /tmp/pginst            # or any installed PostgreSQL tree
export PG_CONFIG=/tmp/pginst/bin/pg_config
make && make install                           # installs into the copy
B=/tmp/pginst
mkdir -p tmp_check
TESTLOGDIR=$PWD/tmp_check/log TESTDATADIR=$PWD/tmp_check PGPORT=65432 \
PATH="$B/bin:$PATH" PG_REGRESS=$B/lib/pgxs/src/test/regress/pg_regress \
prove -I $B/lib/pgxs/src/test/perl -I . t/*.pl
```

See [Continuous integration](#continuous-integration) for where these suites
run automatically.

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
24. Strict mode (default on): every database-level function rejects an invalid
    index and a corrupt page, parallel scans follow the same rules, and
    `strict = off` skips the relation with a warning and returns the checksum
    of everything else; an unlogged table on a standby behaves the same
25. Portability: logical checksums ignore dropped columns and the session's
    `search_path`, and tuple checksums with the header ignore hint bits
26. Direct encodings: for every built-in type encoded without its send
    function, cell and tuple checksums are recomputed in SQL from the real
    send function (`int4send()`, `textsend()`, ...) and must match, including
    extreme values, dropped and added columns, a domain, and a client
    encoding that differs from the server encoding
27. Grouped logical database scan: serial (grouped) and parallel
    (per-relation) results agree, and an index whose expression fails is
    retried per relation, so strict mode names it and `strict = off` skips
    only that index

## Benchmark

`bench/` measures the checksum functions on a generated data set (about
400 MB at scale 1: a 1M-row table with fixed-width columns, a text/numeric/
array table, a 60-column table with NULLs, a TOAST table, and expression,
partial and INCLUDE indexes).  It reports the median time of each function
and its value:

```bash
# against a running server with the extension installed (libpq environment)
bench/run_bench.sh                     # load the data set and time everything
SETUP=no WORKERS=4 bench/run_bench.sh  # reuse the data set, 4 parallel workers
SCALE=4 RUNS=3 bench/run_bench.sh      # larger data set, fewer runs
```

To compare two builds, run it once per build on the same data set
(`SETUP=no` for the second run) and diff the output: the `result` column must
be identical; only `median_ms` may change.  Restart the server with `pg_ctl
stop` and `pg_ctl start` from the other installation; `pg_ctl restart`
reuses the `postgres` binary recorded in `postmaster.opts`.

## Continuous integration

Workflows are in `.github/workflows`.  Pull requests run a reduced
PostgreSQL matrix (12, 16 and 19: the oldest, a middle and the newest
version); merges to `main` run all of 12 to 19.  A workflow only runs when the
files it tests change.

| Workflow | When | What |
|---|---|---|
| CI | PRs, `main` | Regression and TAP tests on Linux x86_64 and arm64, macOS arm64, Windows x64 (MinGW), FreeBSD amd64 and arm64 |
| Quality | PRs, `main` | Coverage (regression tests) and an ASAN/UBSAN build running every regression test except `checksum_parallel` |
| CodeQL | PRs, `main`, weekly | Static analysis of the C and Go sources |
| Big-endian | weekly, manual | Fixed logical baselines (arrays, ranges, multiranges, composites, enums, numeric, text) on s390x (QEMU, PostgreSQL 16) |
| Release Metadata | PRs, `main` | Version in the control file, `META.json`, `Changes` and upgrade scripts agree |
| Docs | PRs, `main` | `mkdocs build --strict`; deploys the site from `main` |
| Workflow Lint | PRs, `main` | `actionlint` on the workflows |
| CLI | PRs, `main` | Go unit tests, `-race`, `go vet`, build, Codecov upload |
| CLI Integration | PRs, `main` | CLI against a real server for each PostgreSQL version, plus the composite action |
| CLI Docker | PRs, `main` | Builds and smoke-tests the CLI image |
| Docker images | PRs (smoke test), `v*` tags | PostgreSQL 12–18 images with the extension, pushed to GHCR on a tag |
| Release | `v*` tags | Extension release: GitHub Release and PGXN upload |
| CLI Release | `cli-v*` tags | CLI binaries, `SHA256SUMS`, Sigstore bundle and SBOM |
| CLI Container | `cli-v*` tags | Signed multi-arch CLI image on GHCR |

Actions are pinned to commit SHAs.  Dependabot updates them (including the
composite action in `cli/action.yml`), the Go modules, the CLI base image and
the documentation dependencies.

## Releasing

Tags are created from `main` after its CI is green.  The extension and the
CLI are released independently.

### Extension (`vX.Y.Z`)

1. In a `release/X.Y.Z` branch:
    - set `default_version` in `pg_integrity_checksums.control` and both
      versions in `META.json`;
    - add `pg_integrity_checksums--X.Y.Z.sql` (the full install script) and
      `pg_integrity_checksums--PREV--X.Y.Z.sql` (the upgrade script; it may
      contain only a comment when no SQL object changes);
    - rename the `Unreleased` section of `Changes` to `X.Y.Z   YYYY-MM-DD`;
    - add the release to [Upgrading](upgrading.md), including any checksum
      value change, and update the supported versions in `SECURITY.md`.
2. Open a pull request.  The Release Metadata workflow checks the version
   files; you can run it locally with
   `python3 .github/scripts/check_release_metadata.py`.
3. After the merge, tag `main` and push the tag:

    ```bash
    git tag -a vX.Y.Z -m "pg_integrity_checksums X.Y.Z"
    git push origin vX.Y.Z
    ```

The Release workflow then checks that the tag, the control file and
`META.json` agree, builds the PGXN archive with `make dist` and verifies its
contents, takes the release notes from the `Changes` entry (it fails rather
than publish empty notes), creates the GitHub Release (tags with `-rc`,
`-beta` or `-alpha` become pre-releases) and uploads the archive to
[PGXN](https://manager.pgxn.org/) when the `PGXN_USERNAME` and
`PGXN_PASSWORD` secrets are set.  The Docker images workflow publishes the
`pgNN-vX.Y.Z` and `pgNN` images.

`make dist` packs `HEAD` into `pg_integrity_checksums-<version>.zip`, so it can
also be run locally to inspect the archive.

### CLI (`cli-vX.Y.Z`)

1. Rename the `Unreleased` section of `cli/Changes` to `X.Y.Z   YYYY-MM-DD`
   and update the version examples in [CLI](cli.md) (installation, GitHub
   Action) and the supported versions in `SECURITY.md`.
2. After the merge, tag `main` with `cli-vX.Y.Z` and push the tag.

The CLI Release workflow builds the binaries with the version stamped in,
writes `SHA256SUMS`, signs it with Sigstore, generates an SBOM and publishes
the release with the notes from `cli/Changes`.  The CLI Container workflow
publishes the signed image.
