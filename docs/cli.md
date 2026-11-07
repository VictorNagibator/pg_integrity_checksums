# CLI: Baselines and Verification

The repository also contains a companion CLI in [`cli/`](../cli/README.md)
(Go 1.23+).  It captures versioned JSON baselines, verifies them, shows diffs
and updates a baseline explicitly after review.  It uses the SQL functions
above and does not install additional database objects.

```bash
cd cli
go build ./cmd/pg_integrity_checksums

pg_integrity_checksums capture --db "$DATABASE_URL" --schema public \
  --output baseline.json

pg_integrity_checksums verify --db "$DATABASE_URL" --baseline baseline.json
```

The CLI is released independently from the extension with `cli-v*` tags;
its source is included in the PGXN source archive.  Profiles, glob filters,
JUnit reports, shell completion and a composite GitHub Action are documented
in the [CLI contract](../cli/README.md).  Logical checksums are the default;
physical baselines are opt-in and tied to one cluster.  CLI release notes are
kept in [cli/Changes](../cli/Changes).

---
