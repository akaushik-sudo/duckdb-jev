#!/usr/bin/env bash
# Packages built release binaries into the zips that are handed out, after checking each one.
#
#     ./scripts/package-release.sh 0.2.0                 # every platform under dist/0.2.0/
#     ./scripts/package-release.sh 0.2.0 linux_amd64
#
# Takes dist/<version>/<platform>/snx_jev.duckdb_extension (written by build-release.sh), loads
# that exact file - shipped name and all - into a stock duckdb 1.5.4 on its platform (Docker),
# and writes dist/<version>/snx_jev-<version>-duckdb-<duckdb>-<platform>.zip holding
#
#     snx_jev.duckdb_extension          the name must stay: DuckDB derives the entry point from it
#     snx_jev.duckdb_extension.sha256
#     USING.md                          how to load and use it
#     VERSION.txt                       version, DuckDB version, platform, source commit
set -euo pipefail

VERSION=${1:?usage: scripts/package-release.sh <version> [platform ...]}
shift
DUCKDB_VERSION=v1.5.4

say() { printf '\033[36m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
dist="dist/$VERSION"
PLATFORMS=("$@")
if [ ${#PLATFORMS[@]} -eq 0 ]; then
  for d in "$dist"/linux_*/; do [ -d "$d" ] && PLATFORMS+=("$(basename "$d")"); done
fi
[ ${#PLATFORMS[@]} -gt 0 ] || die "nothing built under $dist/<platform>/"

root_mount="$(pwd -W 2>/dev/null || pwd)"
commit="$(git rev-parse --short "${SNX_JEV_REV:-HEAD}")"

for platform in "${PLATFORMS[@]}"; do
  dir="$dist/$platform"
  [ -f "$dir/snx_jev.duckdb_extension" ] || die "no $dir/snx_jev.duckdb_extension"
  say "checking $platform in a stock duckdb $DUCKDB_VERSION"
  MSYS_NO_PATHCONV=1 docker run --rm --platform "linux/${platform#linux_}" -v "$root_mount:/repo:ro" python:3.12-slim \
    bash -c "pip -q install --root-user-action=ignore duckdb==${DUCKDB_VERSION#v} 2>/dev/null &&
             python3 /repo/scripts/verify-release.py /repo/$dir/snx_jev.duckdb_extension $VERSION $platform"

  ( cd "$dir" && sha256sum snx_jev.duckdb_extension >snx_jev.duckdb_extension.sha256 )
  cp docs/USING.md "$dir/USING.md"
  printf 'snx_jev %s\nDuckDB %s\nplatform %s\nsource %s\n' "$VERSION" "$DUCKDB_VERSION" "$platform" "$commit" \
    >"$dir/VERSION.txt"
  zip="snx_jev-$VERSION-duckdb-$DUCKDB_VERSION-$platform.zip"
  rm -f "$dist/$zip"
  ( cd "$dir" && python3 -m zipfile -c "../$zip" snx_jev.duckdb_extension snx_jev.duckdb_extension.sha256 \
      USING.md VERSION.txt 2>/dev/null || python -m zipfile -c "../$zip" snx_jev.duckdb_extension \
      snx_jev.duckdb_extension.sha256 USING.md VERSION.txt )
  say "wrote $dist/$zip"
  cat "$dir/snx_jev.duckdb_extension.sha256"
done
