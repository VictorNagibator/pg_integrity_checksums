#!/bin/bash
# bench/run_bench.sh - load the benchmark data set and time the checksums
#
# Runs against an already running server with pg_integrity_checksums
# installed.  Connection settings come from the usual libpq environment
# (PGHOST, PGPORT, PGUSER, PGDATABASE).
#
# Environment overrides:
#   SCALE    data set size multiplier (default: 1, about 400 MB)
#   RUNS     timed runs per case; the median is reported (default: 5)
#   WORKERS  pg_integrity_checksums.workers for the run (default: 0)
#   SETUP    set to "no" to reuse an existing data set (default: yes)
#
# Compare two builds by running this script once per build on the same data
# set (SETUP=no for the second run) and diffing the output: the "result"
# column must be identical, only "median_ms" may differ.

set -e

SCALE="${SCALE:-1}"
RUNS="${RUNS:-5}"
WORKERS="${WORKERS:-0}"
SETUP="${SETUP:-yes}"

cd "$(dirname "$0")"

PSQL="psql -X -q -v ON_ERROR_STOP=1"

$PSQL -c "SET client_min_messages = warning; CREATE EXTENSION IF NOT EXISTS pg_integrity_checksums"

if [ "$SETUP" = "yes" ]; then
    echo ">>> Loading data set (scale $SCALE)" >&2
    $PSQL -v scale="$SCALE" -f setup.sql >/dev/null
fi

$PSQL -c "SELECT extversion FROM pg_extension WHERE extname = 'pg_integrity_checksums'" -At |
    sed 's/^/extension version: /'
$PSQL -v runs="$RUNS" -v workers="$WORKERS" -f run.sql
