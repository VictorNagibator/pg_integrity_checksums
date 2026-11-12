# syntax=docker/dockerfile:1

# Build a PostgreSQL image with pg_integrity_checksums installed and enabled in
# the default database.  Override PG_MAJOR for another supported image.
ARG PG_MAJOR=16

FROM postgres:${PG_MAJOR} AS build
ARG PG_MAJOR

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        postgresql-server-dev-${PG_MAJOR} \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN make clean \
    && make -j"$(nproc)" with_llvm=no \
    && make install DESTDIR=/stage with_llvm=no

FROM postgres:${PG_MAJOR}
ARG PG_MAJOR

# The build stage installs postgresql-server-dev from the PGDG repository, which
# upgrades the server packages of its base image to the newest minor release.
# An extension compiled against newer headers can reference server symbols that
# an older minor release does not export (the library then fails to load with
# "undefined symbol"), and the official image can lag behind PGDG by a few days.
# Bring the runtime image to the same minor release as the build stage.
RUN apt-get update \
    && apt-get install -y --no-install-recommends --only-upgrade \
        postgresql-${PG_MAJOR} \
        postgresql-client-${PG_MAJOR} \
        libpq5 \
    && rm -rf /var/lib/apt/lists/*

LABEL org.opencontainers.image.title="pg_integrity_checksums"
LABEL org.opencontainers.image.description="PostgreSQL with the pg_integrity_checksums extension"
LABEL org.opencontainers.image.source="https://github.com/VictorNagibator/pg_integrity_checksums"
LABEL org.opencontainers.image.licenses="MIT"

COPY --from=build /stage/usr/lib/postgresql/${PG_MAJOR}/lib/pg_integrity_checksums.so \
                  /usr/lib/postgresql/${PG_MAJOR}/lib/
COPY --from=build /stage/usr/share/postgresql/${PG_MAJOR}/extension/pg_integrity_checksums* \
                  /usr/share/postgresql/${PG_MAJOR}/extension/
COPY docker/init-extension.sql /docker-entrypoint-initdb.d/10-pg_integrity_checksums.sql
