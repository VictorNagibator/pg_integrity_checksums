# Installation

### Install from packages or Docker (recommended)

| Method | PostgreSQL versions | Notes |
|---|---|---|
| **PGXN / source** | 12–19 | Always gets the latest release |
| **PIG / APT / DNF** | 14–18 | Binary packages from the PIGSTY repository via [PGEXT.CLOUD](https://pgext.cloud/ext/pg_integrity_checksums) |
| **Docker** | 12–18 | Prebuilt images with the extension enabled in the default database |

The fastest way on a machine that already has PostgreSQL 14–18 and the PIGSTY
repository configured:

```bash
# Any distribution supported by PIG
pig install pg_integrity_checksums

# Debian / Ubuntu (PIGSTY repository configured)
sudo apt install postgresql-16-pg-integrity-checksums

# RHEL / Fedora / Rocky / AlmaLinux (PIGSTY repository configured)
sudo dnf install pg_integrity_checksums_16

psql -d your_database -c "CREATE EXTENSION pg_integrity_checksums;"
```

Replace `16` with your PostgreSQL major version.  The package page is
[PGEXT.CLOUD: pg_integrity_checksums](https://pgext.cloud/ext/pg_integrity_checksums).

> **Version note:** binary packages are rebuilt by the PIGSTY/PGEXT maintainers
> after a release and can lag behind PGXN.  Check the installed version with
> `SELECT extversion FROM pg_extension WHERE extname = 'pg_integrity_checksums';`.

### Docker

Prebuilt images are published to GHCR for PostgreSQL 12–18.  The extension is
created automatically in `POSTGRES_DB` on first initialization.

```bash
docker run --rm --name pg-integrity \
  -e POSTGRES_PASSWORD=secret \
  -e POSTGRES_DB=demo \
  -p 5432:5432 \
  ghcr.io/victornagibator/pg_integrity_checksums:pg16
```

Then, from another terminal:

```bash
psql -h localhost -U postgres -d demo \
  -c "SELECT extversion FROM pg_extension WHERE extname = 'pg_integrity_checksums';" \
  -c "SELECT pg_table_logical_checksum('pg_class'::regclass);"
```

To build an image for another PostgreSQL major version locally:

```bash
docker build --build-arg PG_MAJOR=17 -t pg-integrity-checksums:pg17 .
```

Images are built by the
[Docker workflow](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/.github/workflows/docker.yml); pull requests and releases are
smoke-tested with PostgreSQL 16.

### Prerequisites

* PostgreSQL 12 or later (tested on 12, 13, 14, 15, 16, 17, 18, and 19)
* C compiler and PostgreSQL development headers
* Superuser access to install the extension and to run the database-level
  checksum functions

> **Privileges:** the table, page, cell, tuple and index checksum functions
> read relation storage directly.  They require `SELECT` on the relation (or
> on the base table for an index) and reject relations protected by row-level
> security, because a direct page scan cannot apply RLS policies.  The
> database-level functions are superuser-only; this is enforced in C, not just
> in SQL.

> **Note:** PostgreSQL 12 and 13 are past end-of-life upstream, but they are
> still fully supported by this extension. The build deliberately fails with a
> clear error on PostgreSQL 11 and older, which lack the table-AM API the
> extension relies on.

> **Note:** this extension is named `pg_integrity_checksums` specifically to
> distinguish it from the `pg_checksums` utility shipped with PostgreSQL core.
> They are unrelated: the core utility verifies data page checksums on disk,
> while this extension provides SQL functions for computing checksums.

### Build and Install

The extension builds with PGXS, so `pg_config` must be on your `PATH`, or you
can point `make` at a specific installation with `PG_CONFIG=/path/to/pg_config`.

```bash
# Clone or extract the extension
git clone https://github.com/VictorNagibator/pg_integrity_checksums

# Go into extension directory
cd pg_integrity_checksums

# Build
make

# Install
make install

# Create extension in your database
psql -d your_database -c "CREATE EXTENSION pg_integrity_checksums;"
```

Or install it straight from [PGXN](https://pgxn.org/dist/pg_integrity_checksums/) with the
[pgxn client](https://github.com/pgxn/pgxnclient):

```bash
pgxn install pg_integrity_checksums
```

The extension is also listed in
[PGEXT.CLOUD](https://pgext.cloud/ext/pg_integrity_checksums).

### Upgrading to 2.2

2.2 adds generic algorithm overloads for FNV-1a 32-bit and 64-bit digests.
Existing SQL functions and their 32-bit values are unchanged; page checksums
remain 32-bit.  Upgrade with:

```sql
ALTER EXTENSION pg_integrity_checksums UPDATE TO '2.2.0';
```

Older CLI releases cannot read v2 baselines created with `--hash fnv64`; the
0.4 CLI reads both v1 (fnv32) and v2 (fnv64).

### Migrating from pg_checksums 1.x

The extension was renamed in 2.0.0 to avoid confusion with PostgreSQL's core
`pg_checksums` utility.  PostgreSQL has no `ALTER EXTENSION ... RENAME`, so
migrate by reinstalling the extension and updating the GUC prefix.  See
[MIGRATION.md](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/MIGRATION.md) for the full checklist:

```sql
DROP EXTENSION pg_checksums;
CREATE EXTENSION pg_integrity_checksums;
```

```sql
-- old settings
SET pg_checksums.workers = 4;
SET pg_checksums.strict = on;

-- new settings
SET pg_integrity_checksums.workers = 4;
SET pg_integrity_checksums.strict = on;
```

SQL function names and checksum values are unchanged, so existing logical and
physical baselines remain valid.  If `DROP EXTENSION` fails because of
dependent objects, remove or recreate those dependencies explicitly rather
than using `CASCADE` without review.

---
