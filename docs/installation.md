# Installation

## Install from packages or Docker (recommended)

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

## Docker

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

## Prerequisites

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

## Build and Install

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

## Verify installation

After `CREATE EXTENSION`, check the installed version and compute one
checksum with the default 32-bit API:

~~~sql
SELECT extversion
FROM pg_extension
WHERE extname = 'pg_integrity_checksums';

SELECT pg_table_logical_checksum('pg_class'::regclass);
~~~

Extension 2.2 and newer also provide the optional generic API:

~~~sql
SELECT encode(
    pg_table_logical_checksum('pg_class'::regclass, 'fnv64'),
    'hex'
);
~~~

If the generic call reports that the function does not exist, the installed
extension is older than 2.2 or the upgrade did not run.  See
[Upgrading](upgrading.md#22).

## Upgrading

Installing a newer release does not change an existing database until you run
`ALTER EXTENSION pg_integrity_checksums UPDATE`.  See
[Upgrading](upgrading.md) for the procedure, the notes for each release and
which upgrades change checksum values.

## CLI

The companion CLI is installed separately, from release binaries, a container
image or `go install`.  See [CLI installation](cli.md#installation).

## Next

Continue with [Getting started](getting-started.md) for the first checksum and
[Concepts](concepts.md) for the logical/physical model.

