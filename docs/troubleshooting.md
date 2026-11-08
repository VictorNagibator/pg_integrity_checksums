# Troubleshooting

## Extension is not installed

~~~sql
CREATE EXTENSION pg_integrity_checksums;
~~~

## Permission denied

The checksum functions require SELECT on each object.  Row-level security is
rejected because a direct heap scan cannot apply policies.  Use a role with
SELECT or restrict the scope with the CLI filters.

## Physical baseline rejected

A physical baseline is tied to one cluster and database.  Recreate it on the
current cluster; do not compare physical checksums across replicas.

## Slow scans

Use the extension workers and a larger timeout:

~~~bash
pg_integrity_checksums verify --workers 4 --timeout 30m --baseline baseline.json
~~~

For repeated verification of a logical baseline, see --changed-since in the
[CLI contract](https://github.com/VictorNagibator/pg_integrity_checksums/blob/main/cli/README.md).
