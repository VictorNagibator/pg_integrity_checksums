## Summary

<!-- What does this change and why? Link the issue it closes, if any. -->

<!--
The PR title becomes the squash commit, so it must follow Conventional
Commits, for example `fix(extension): ...` or `feat(cli): ...`.
See CONTRIBUTING.md.
-->

Closes #

## Checksum values

- [ ] Checksum values do not change
- [ ] Checksum values change (explain which, and why it cannot wait for a major release)

## How it was tested

<!-- Remove the lines that do not apply and add what is missing. -->

- [ ] `make installcheck` (PostgreSQL version: )
- [ ] `make installcheck enable_tap_tests=yes PROVE=prove`
- [ ] `cd cli && go test ./... && go vet ./...`
- [ ] `mkdocs build --strict`
- [ ] `bench/run_bench.sh` before and after (for performance changes)

## Checklist

- [ ] Tests added or updated; `expected/*.out` reviewed before committing
- [ ] Documentation in `docs/` updated for user-visible changes
- [ ] `Changes` (extension) or `cli/Changes` (CLI) updated
- [ ] New SQL objects: install script, upgrade script, `.control` and `META.json`
- [ ] No new compiler warnings, build artifacts or secrets
