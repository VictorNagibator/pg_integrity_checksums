# Contributing

Thanks for taking the time to contribute to `pg_integrity_checksums`.

## Reporting bugs and requesting features

Use the GitHub issue templates. For security issues, follow
[SECURITY.md](SECURITY.md) and do not open a public issue.

## Development

Requirements:

- PostgreSQL 12 or newer with server headers;
- PGXS;
- a C compiler;
- Perl and `IPC::Run` for the TAP tests.

Build and install:

```bash
make PG_CONFIG=/path/to/pg_config
make PG_CONFIG=/path/to/pg_config install
```

Run the regression tests against a running PostgreSQL instance:

```bash
make PG_CONFIG=/path/to/pg_config installcheck
```

Run the TAP suite:

```bash
make PG_CONFIG=/path/to/pg_config installcheck \
     enable_tap_tests=yes PROVE=prove
```

Use `PG_CONFIG` for non-default PostgreSQL installations. If the test server
is not on the default socket or port, set `PGHOST`, `PGPORT`, and `PGUSER`
for `make installcheck`.

## Branches and commits

`main` is release-ready. Create a short-lived branch for each change:

- `feat/<topic>` — new behavior;
- `fix/<topic>` — bug fix;
- `docs/<topic>` — documentation only;
- `test/<topic>` — tests only;
- `refactor/<topic>` — behavior-preserving cleanup;
- `ci/<topic>` — workflows and tooling;
- `chore/<topic>` — maintenance;
- `release/<version>` — release preparation.

Use Conventional Commits:

```text
<type>(<scope>): <imperative summary>
```

Common types: `feat`, `fix`, `docs`, `test`, `refactor`, `perf`,
`build`, `ci`, `chore`, `revert`.

Common scopes: `cli`, `extension`, `ci`, `docs`, `workflow`, `release`, `deps`.

Examples:

```text
feat(cli): add init and config commands
fix(release): use the cosign bundle format
docs(cli): document configuration validation
ci(actions): pin checkout to a commit SHA
test(cli): cover DSN masking
chore(deps): bump pgx to 5.12.0
```

Keep the subject in the imperative mood, lowercase, without a trailing period,
and under about 72 characters. Put details in the body. Mark a breaking change
with `!` and a `BREAKING CHANGE:` footer.

Pull request titles must also use Conventional Commits. Use squash merge so the
PR title becomes the commit on `main`. Do not commit directly to `main`; the
exception is an emergency fix, followed by a normal commit on the next branch.

Tags are created only from `main` after the relevant CI is green:

- `cli-vX.Y.Z` for CLI releases;
- `vX.Y.Z` for extension releases.

## Pull requests

- Keep each pull request focused on one change.
- Add a regression test for every behavior change.
- Update `expected/*.out` only after reviewing the actual diff. Run the tests,
  inspect `results/` or `regression.diffs`, and confirm that the new output is
  correct.
- Update `README.md` and `Changes` when user-visible behavior changes.
- If the SQL API changes, add an install script and an upgrade script, and
  update `pg_integrity_checksums.control` and `META.json`.
- Do not commit build artifacts such as `*.o`, `*.so`, or test output.
- Make sure the code builds without new compiler warnings.
- Describe how the change was tested and on which PostgreSQL versions.

## Logical checksum changes

Logical checksum values are a compatibility boundary: changing them invalidates
existing baselines. Do not change the logical format silently. Open an issue
first and describe:

- why the change is necessary;
- whether it affects table, index, or database checksums;
- how users should migrate or recompute baselines;
- which release and version should contain it.

Breaking changes must be called out explicitly in `Changes` and documented in
the README.

## Code style

Follow PostgreSQL C style and the conventions already used in
`pg_integrity_checksums.c`. Keep functions focused, remove dead code, and document
non-obvious checksum semantics. Prefer clear names over abbreviations.
