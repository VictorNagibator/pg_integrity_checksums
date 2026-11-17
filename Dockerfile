# syntax=docker/dockerfile:1

# Build a PostgreSQL image with pg_integrity_checksums installed and enabled in
# the default database.  Override PG_MAJOR for another supported image.
ARG PG_MAJOR=16

# Base images, pinned by digest (OpenSSF Scorecard: Pinned-Dependencies).
# Dependabot updates these lines; PG_MAJOR selects one of them.  BuildKit
# builds only the stages the selected one depends on.
FROM postgres:12@sha256:2f2a8c2a7d10862e7fba2602e304523554f9df8244c632dafe2628ccb398fb5c AS base-12
FROM postgres:13@sha256:4689940c683801b4ab839ab3b0a0a3555a5fe425371422310944e89eca7d8068 AS base-13
FROM postgres:14@sha256:c0e9c0e9bac7e4c1834b698217a03ee5c708de30d313c84a506972de0e24b059 AS base-14
FROM postgres:15@sha256:2663a9f0fde52de94f5dad3603a1942359f49472f1017c1a6d64f66bfedacb40 AS base-15
FROM postgres:16@sha256:dd2bf3ce2dc83a81674cf7f217fcfd8a5009a4b325dda5b843bfee82a06cff6f AS base-16
FROM postgres:17@sha256:0d938e975177bef9f1f61f3f9dbcf2e0aa0a139568f66d3415ed8892eb97f6ac AS base-17
FROM postgres:18@sha256:2ea8630df176ad090ec92136dee850397c5af8781e4bcf2e4162593691c61bbc AS base-18

FROM base-${PG_MAJOR} AS build
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

FROM base-${PG_MAJOR}
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
