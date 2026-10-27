---
name: Bug report
about: Report a reproducible defect in pg_integrity_checksums
title: "[BUG]"
labels: bug
assignees: ''
type: Bug

---

### What happened?

<!-- Briefly describe the failure: which function, what result, what error. -->

### Expected behavior

<!-- What should have happened instead? -->

### Steps to reproduce

1. ...
2. ...
3. ...

Minimal example:

```sql
-- DDL/DML
CREATE TABLE ...;
INSERT INTO ...;

-- failing call
SELECT pg_...;
```

### Environment

- pg_integrity_checksums version:
  ```sql
  SELECT extversion FROM pg_extension WHERE extname = 'pg_integrity_checksums';
  ```
- PostgreSQL version:
  ```sql
  SELECT version();
  ```
- OS and architecture:
- Installation method: PGXN / `make install` / package / other
- `SHOW pg_integrity_checksums.workers;`
- `SHOW pg_integrity_checksums.strict;`

### Which checksum is affected?

- [ ] physical
- [ ] logical
- [ ] page / cell / tuple / table / index / database
- [ ] table without a primary key
- [ ] partitioned table / partitioned index
- [ ] TOAST
- [ ] expression / partial / INCLUDE index
- [ ] parallel mode (`pg_integrity_checksums.workers > 0`)

### Additional checks

- Does it reproduce with `SET pg_integrity_checksums.workers = 0`?
- Does the result change after `VACUUM`, `VACUUM FULL`, `CLUSTER`, or `REINDEX`?
- Does it affect only a specific index AM: B-tree, Hash, GiST, GIN, SP-GiST, BRIN?
- Is this a server crash or an incorrect checksum value?

### Logs

<details>
<summary>Error / server log / backtrace</summary>

```text
...
```

</details>

### Checklist

- [ ] I am using the latest pg_integrity_checksums release
- [ ] I searched for existing issues
- [ ] The example is minimal and contains no real data or secrets
- [ ] This is not a security vulnerability; I will use a private advisory for that
