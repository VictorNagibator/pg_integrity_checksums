# Standalone CLI image. It contains no PostgreSQL server; point it at any
# reachable database with --dsn or DATABASE_URL.
FROM golang:1.25-alpine AS build

WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
ARG VERSION=dev
RUN CGO_ENABLED=0 go build -trimpath \
    -ldflags "-s -w -X github.com/VictorNagibator/pg_integrity_checksums/cli/internal/cli.Version=$VERSION" \
    -o /out/pg_integrity_checksums ./cmd/pg_integrity_checksums

FROM alpine:3.20
RUN apk add --no-cache ca-certificates
COPY --from=build /out/pg_integrity_checksums /usr/local/bin/pg_integrity_checksums
USER 65532:65532
ENTRYPOINT ["pg_integrity_checksums"]
