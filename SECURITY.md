# Security Policy

## Supported versions

Security fixes are provided for the latest release of `pg_integrity_checksums`.

| pg_integrity_checksums | Supported |
| ---------------------- | --------- |
| 2.0.x                  | Yes       |
| < 2.0                  | No — upgrade |

The supported PostgreSQL major versions are listed in the README and in the CI
matrix. Please reproduce security reports on a currently supported PostgreSQL
release.

## Reporting a vulnerability

**Do not open a public GitHub issue for a security problem.**

Use GitHub's private vulnerability reporting:

https://github.com/VictorNagibator/pg_integrity_checksums/security/advisories/new

If that page is unavailable, contact me at the address listed in
`META.json`. Please do not publish details before a fix is available.

Include as much of the following as possible:

- `pg_integrity_checksums` version:
  `SELECT extversion FROM pg_extension WHERE extname = 'pg_integrity_checksums';`
- PostgreSQL version: `SELECT version();`
- operating system and architecture;
- a minimal reproduction, including SQL and any required DDL;
- expected and actual behavior;
- server logs, backtrace, or core dump details, if available.

Do not include real customer data, credentials, or production dumps. Replace
sensitive values with synthetic ones.

## Scope

Security-relevant issues include:

- crashes or memory corruption caused by malformed or corrupted relation data;
- bypassing the superuser requirement of database-level functions;
- bypassing `SELECT` privilege checks or row-level security restrictions;
- arbitrary file access or privilege escalation;
- denial of service caused by an extension function.

Out of scope:

- incorrect checksums caused by intentionally corrupted data (detecting
  corruption is the purpose of the extension);
- performance problems;
- problems in unsupported PostgreSQL or `pg_integrity_checksums` versions;
- FNV-1a collisions. This is a non-cryptographic checksum, as documented.

## What to expect

I will acknowledge the report, investigate it, and coordinate disclosure through
a GitHub Security Advisory. Please allow reasonable time for a fix before
publishing details.
