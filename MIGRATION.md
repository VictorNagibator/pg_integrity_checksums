# Migrating from pg_checksums 1.x

`pg_integrity_checksums` 2.0.0 is the renamed continuation of the
`pg_checksums` extension. PostgreSQL does not support renaming an extension
with `ALTER EXTENSION`, so the old extension must be dropped and the new one
created.

## Before you start

- Take a backup or make sure the database can be restored.
- Update any scripts that install or configure the extension.
- Update `pg_checksums.workers` and `pg_checksums.strict` settings to
  `pg_integrity_checksums.workers` and `pg_integrity_checksums.strict`.
- Check for objects that depend on the extension functions before dropping it.

## Migration

```sql
DROP EXTENSION pg_checksums;
CREATE EXTENSION pg_integrity_checksums;
```

SQL function names and checksum values are unchanged. Existing physical and
logical baselines therefore remain valid.

If `DROP EXTENSION` fails because dependent objects reference extension
functions, remove or recreate those objects explicitly. Avoid `CASCADE`
unless you have reviewed everything it would drop.

## Cannot install both names

Both extensions define the same SQL function names. They cannot coexist in the
same database/schema. After migration, use:

```sql
SELECT extversion
FROM pg_extension
WHERE extname = 'pg_integrity_checksums';
```

## Configuration example

```sql
SET pg_integrity_checksums.workers = 4;
SET pg_integrity_checksums.strict = on;
```

The old `pg_checksums` 1.3.x release remains available from its previous PGXN
release and git tag for users that still need the old extension name.
