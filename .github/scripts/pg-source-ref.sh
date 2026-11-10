#!/usr/bin/env bash
# Pinned PostgreSQL source revisions for CI.
#
# Keep this file as the single source of truth instead of downloading from
# REL_xx_STABLE.  Tags make released branches reproducible; PostgreSQL 19 has
# no final release tag yet, so it is pinned to a commit on REL_19_STABLE.
set -euo pipefail

case "${1:?usage: pg-source-ref.sh MAJOR}" in
  12) echo REL_12_22 ;;
  13) echo REL_13_23 ;;
  14) echo REL_14_24 ;;
  15) echo REL_15_19 ;;
  16) echo REL_16_15 ;;
  17) echo REL_17_11 ;;
  18) echo REL_18_6 ;;
  19) echo dffcca2e3c81178c64d4c1f44543a3507d0b7508 ;;
  *) echo "unsupported PostgreSQL major: $1" >&2; exit 1 ;;
esac
