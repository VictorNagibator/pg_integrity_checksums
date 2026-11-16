# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`pg_integrity_checksums` (formerly `pg_checksums`): a PostgreSQL C extension (PG 12–19, built with PGXS) that computes page, cell, tuple, table, index and database checksums in **logical** (canonical, portable across clusters/endianness/OIDs/VACUUM) and **physical** (one cluster's storage) modes, plus a companion Go CLI in `cli/` that captures JSON baselines and verifies databases against them.

## Commands

Extension (all `make` targets accept `PG_CONFIG=/path/to/pg_config`):

```bash
make && make install                     # build + install (PGXS)
make installcheck                        # all regression tests (needs a running server; PGHOST/PGPORT/PGUSER)
make installcheck REGRESS=checksum_types # one regression test
make installcheck enable_tap_tests=yes PROVE=prove                          # + TAP tests (IPC::Run)
make installcheck enable_tap_tests=yes PROVE=prove PROVE_TESTS=t/008_strict.pl  # one TAP test
./run_tests.sh                           # clean, build, install (via sudo), start server if needed, test
python3 .github/scripts/check_release_metadata.py   # version files agree
bench/run_bench.sh                       # benchmark; diff output between builds: `result` column must be identical
```

- PGXS does not track header dependencies: after editing `checksum_internal.h` run `make clean` first, or stale objects get linked against new struct layouts (has caused crashes in parallel workers).
- A new regression test must be added to `REGRESS` in the `Makefile` (`sql/<name>.sql` + `expected/<name>.out`). Only update `expected/*.out` after reviewing `regression.diffs`; they must match byte for byte.
- When switching between two PG installs, use `pg_ctl stop` + `pg_ctl start` from the other install; `pg_ctl restart` reuses the old binary from `postmaster.opts`.
- `docs/testing.md` explains running TAP against a relocated copy of a PG install tree when the system install isn't writable.

CLI (`cd cli`):

```bash
go test ./...
go vet ./...
go test ./internal/cli -run TestName
PGICS_TEST_DSN="postgresql:///app?host=/tmp" go test ./internal/cli -run TestIntegration -v  # needs DB with extension
```

Docs: `mkdocs build --strict` (deps in `requirements-docs.txt`, hash-pinned).

## Architecture

The extension is split into translation units sharing `checksum_internal.h` (the source-layout table is in `docs/architecture.md`). Key cross-file ideas:

- **Pipeline** (table/index/database): lock relation(s) with `AccessShareLock` → take the statement/transaction snapshot → scan heap blocks or index pages, validating page/line-pointer/tuple structure (`checksum_validation.c`) and skipping invisible tuples → canonicalize values (logical) → fold into an order-independent accumulator → finalize.
- **Aggregation**: physical = avalanche-mix + XOR (`ChecksumAccum`); logical = sum + count, `final = mix(sum) ^ mix(count)` (`ChecksumLogicalAccum`), so duplicates don't cancel. Both are order-independent, which is what lets serial, parallel and partitioned scans agree.
- **Canonical encoding** (`checksum_types.c`): type tag from type *name* not OID; send function (or output function) bytes; recursive for arrays/composites/domains/ranges/multiranges; enum labels; normalized numeric; canonical NaN/-0; `reg*` types hashed by qualified object name; little-endian shape integers. A per-backend type cache plus "direct encodings" for common built-ins avoid fmgr calls — `checksum_fastpath` test verifies each direct encoding against the real send function.
- **Physical hashing** deliberately masks state that changes without data changes (page LSN, `pd_checksum`, `pd_flags`, `pd_prune_xid`, hint bits, LP_DEAD/garbage flags). Keep this in mind when touching page hashing (`t/009_physical_restart.pl`).
- **Locking rule**: validation and visibility checks happen under a buffer share lock; logical hashing (detoast, send functions, index expression eval) runs with the buffer only pinned. Physical tuple hashing stays under the share lock.
- **Index logical checksums** evaluate the index definition on visible heap rows rather than reading index pages; physical index checksums read pages. A serial `pg_database_logical_checksum` groups each table with its indexes to read the heap once, in a subtransaction, falling back to per-relation scans on error.
- **Parallelism** (`checksum_parallel.c`): DSM `ChecksumParallelState`, atomic work counter handing out block ranges (or whole relations for DB scans), serialized snapshot; disabled inside workers and for temp tables. Controlled by `pg_integrity_checksums.workers` GUC.
- **Strict mode** (`pg_integrity_checksums.strict`, default on): database-level functions fail naming an unsupported relation instead of silently skipping it.
- Only heap table AM is supported; RLS relations are rejected; database-level functions require superuser.

CLI (`cli/internal/`): `cli` (command dispatch, flags, output/JUnit; stdout is data only, progress/errors to stderr), `pgdb` (connects via pgx, resolves the extension's schema from `pg_extension`, computes a baseline in one snapshot), `baseline` (v1 = fnv32, v2 = fnv64 JSON formats with schemas in `cli/docs/`, comparison), `config`.

## Compatibility rules

- **Logical checksum values are a compatibility boundary.** Any change that alters existing values invalidates user baselines: it must be called out in `Changes` and `docs/upgrading.md` ("which upgrades change checksum values"). Don't change it silently.
- SQL API changes need a new full install script `pg_integrity_checksums--X.Y.Z.sql` and an upgrade script `--PREV--X.Y.Z.sql`, plus `default_version` in the `.control` file and both versions in `META.json` (checked by CI's Release Metadata workflow).
- User-visible changes go under `Unreleased` in `Changes` (extension) or `cli/Changes` (CLI), and in `docs/`.
- Extension and CLI are released independently (`vX.Y.Z` vs `cli-vX.Y.Z` tags from `main`).

## Conventions

- PostgreSQL C style, tabs (4 wide); keep it building on PG 12 (see `lnext_compat` and `PG_VERSION_NUM` guards in `checksum_internal.h`) and without new warnings.
- Branches: `feat/`, `fix/`, `docs/`, `test/`, `refactor/`, `ci/`, `chore/`, `release/`. Conventional Commits for commits and PR titles (`fix(extension): ...`, `feat(cli): ...`); squash merge; no direct commits to `main`.
- Every behavior change gets a regression test. Don't commit `*.o`, `*.so` or test output.
- CI on PRs runs PG 12, 16 and 19; **CI Gate** is the single required check.
