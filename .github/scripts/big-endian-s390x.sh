#!/bin/sh
# Big-endian regression test, run inside an s390x Debian container.
set -e

PG=16
PG_SRC="/tmp/pg-src-$PG"
PREFIX="/tmp/pg$PG"

rm -rf "$PG_SRC" "$PREFIX"
curl -L -o /tmp/pg.tar.gz \
  "https://github.com/postgres/postgres/archive/refs/heads/REL_$PG"_STABLE.tar.gz
tar -xzf /tmp/pg.tar.gz -C /tmp
mv "/tmp/postgres-REL_$PG"_STABLE "$PG_SRC"

cd "$PG_SRC"
./configure \
  --prefix="$PREFIX" \
  --enable-tap-tests \
  --disable-nls \
  --without-icu \
  --without-readline \
  --without-zlib \
  --without-llvm
make -j2
make install
PG_CONFIG="$PREFIX/bin/pg_config"

cd /src
make PG_CONFIG="$PG_CONFIG" with_llvm=no
make PG_CONFIG="$PG_CONFIG" install with_llvm=no

PGBIN="$(dirname "$PG_CONFIG")"
PGDATA="/tmp/pgdata-$PG"
rm -rf "$PGDATA"
"$PGBIN/initdb" -D "$PGDATA" -U pg -A trust --no-sync
"$PGBIN/pg_ctl" -D "$PGDATA" -l "$PGDATA/server.log" -o '-p 55444' -w start
trap '"$PGBIN/pg_ctl" -D "$PGDATA" -m immediate stop || true' EXIT

TAP_ARGS=""
if command -v prove >/dev/null 2>&1; then
  TAP_ARGS="enable_tap_tests=yes PROVE=prove PROVE_FLAGS='-I $PG_SRC/src/test/perl'"
fi
PGHOST=/tmp PGPORT=55444 PGUSER=pg \
  make PG_CONFIG="$PG_CONFIG" installcheck $TAP_ARGS
