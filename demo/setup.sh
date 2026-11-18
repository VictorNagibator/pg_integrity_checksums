#!/bin/sh
# Prepare the two databases of the README demo.
#
#   demo/setup.sh PROD_DSN BACKUP_DSN
#
# PROD gets the demo data; BACKUP gets a pg_dump of PROD, as a backup restored
# on another server would.  Both databases must exist and the extension must be
# installed on both servers.  Existing demo tables are replaced.
set -eu

if [ $# -ne 2 ]; then
	echo "usage: $0 PROD_DSN BACKUP_DSN" >&2
	exit 2
fi

dir=$(dirname "$0")
psql="psql -X -q -v ON_ERROR_STOP=1"
PGOPTIONS="-c client_min_messages=warning"
export PGOPTIONS

$psql -d "$1" -f "$dir/seed.sql"
$psql -d "$2" -c 'DROP TABLE IF EXISTS orders, products, customers'
pg_dump --no-owner "$1" | $psql -d "$2" >/dev/null
