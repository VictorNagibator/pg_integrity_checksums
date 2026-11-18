#!/bin/bash
# bench/compare.sh - checksum cost relative to a sequential scan
#
# Loads pgbench data and runs bench/compare.sql serially and with parallel
# workers.  Runs against an already running server with
# pg_integrity_checksums available; connection settings come from the usual
# libpq environment (PGHOST, PGPORT, PGUSER, PGDATABASE).
#
# Environment overrides:
#   SCALE    pgbench scale factor (default: 68, about 1 GB; 680 is about 10 GB)
#   RUNS     timed runs per case; the median is reported (default: 3)
#   WORKERS  space-separated worker counts to run (default: "0 4")
#   SETUP    set to "no" to reuse existing pgbench tables (default: yes)

set -e

SCALE="${SCALE:-68}"
RUNS="${RUNS:-3}"
WORKERS="${WORKERS:-0 4}"
SETUP="${SETUP:-yes}"

cd "$(dirname "$0")"

PSQL="psql -X -q -v ON_ERROR_STOP=1"

$PSQL -c "SET client_min_messages = warning; CREATE EXTENSION IF NOT EXISTS pg_integrity_checksums"

if [ "$SETUP" = "yes" ]; then
    echo ">>> Loading pgbench data (scale $SCALE)" >&2
    pgbench -i -q -s "$SCALE" >/dev/null 2>&1
    $PSQL -c "VACUUM (FREEZE, ANALYZE) pgbench_accounts"
fi

$PSQL -At -c "SELECT 'server: ' || version()"
$PSQL -At -c "SELECT 'extension: ' || extversion FROM pg_extension WHERE extname = 'pg_integrity_checksums'"
$PSQL -At -c "SELECT 'pgbench_accounts: ' || pg_size_pretty(pg_table_size('pgbench_accounts'))
                  || ' heap, ' || pg_size_pretty(pg_indexes_size('pgbench_accounts')) || ' indexes; database: '
                  || pg_size_pretty(pg_database_size(current_database()))"
$PSQL -At -c "SELECT 'shared_buffers: ' || current_setting('shared_buffers')"

for w in $WORKERS; do
    echo
    echo "workers = $w"
    $PSQL -v runs="$RUNS" -v workers="$w" -f compare.sql
done
