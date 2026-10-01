#!/usr/bin/env bash
# Packages built release binaries into the zips that are handed out, after checking each one.
#
#     ./scripts/package-release.sh 0.2.0                 # every platform under dist/0.2.0/
#     ./scripts/package-release.sh 0.2.0 linux_amd64
#
# Takes dist/<version>/<platform>/snx_jev.duckdb_extension and dist/<version>/SOURCE_COMMIT (both
# written by build-release.sh), loads that exact file - shipped name and all - into a stock
# duckdb 1.5.4 on its platform, and writes dist/<version>/snx_jev-<version>-duckdb-<duckdb>-<platform>.zip:
#
#     snx_jev.duckdb_extension          the name must stay: DuckDB derives the entry point from it
#     snx_jev.duckdb_extension.sha256
#     USING.md                          how to load and use it
#     VERSION.txt                       version, DuckDB version, platform, commits, tests run
#
# USING.md and the check script come from a commit, never the working tree: SOURCE_COMMIT by
# default, or SNX_JEV_DOCS_REV to ship corrected docs with a binary built earlier. Everything is
# done in a container (python:3.12-slim), so the host needs only git and Docker.
set -euo pipefail

VERSION=${1:?usage: scripts/package-release.sh <version> [platform ...]}
shift
DUCKDB_VERSION=v1.5.4

say() { printf '\033[36m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "version must look like 0.2.0 (got '$VERSION')"
dist="dist/$VERSION"
[ -f "$dist/SOURCE_COMMIT" ] || die "no $dist/SOURCE_COMMIT: build with scripts/build-release.sh first"
source_commit="$(cat "$dist/SOURCE_COMMIT")"
docs_commit="$(git rev-parse "${SNX_JEV_DOCS_REV:-$source_commit}^{commit}")"

PLATFORMS=("$@")
if [ ${#PLATFORMS[@]} -eq 0 ]; then
  for d in "$dist"/linux_*/; do [ -d "$d" ] && PLATFORMS+=("$(basename "$d")"); done
fi
[ ${#PLATFORMS[@]} -gt 0 ] || die "nothing built under $dist/<platform>/"
for platform in "${PLATFORMS[@]}"; do
  case "$platform" in linux_amd64|linux_arm64) ;; *) die "unsupported platform '$platform'" ;; esac
  [ -f "$dist/$platform/snx_jev.duckdb_extension" ] || die "no $dist/$platform/snx_jev.duckdb_extension"
done

# The committed files the package uses, extracted from their commit.
stage="$dist/.package"
rm -rf "$stage" && mkdir -p "$stage"
trap 'rm -rf "$stage"' EXIT
git show "$docs_commit:docs/USING.md" >"$stage/USING.md"
git show "$docs_commit:scripts/verify-release.py" >"$stage/verify-release.py"

dist_mount="$(cd "$dist" && (pwd -W 2>/dev/null || pwd))"
host_ids="$(id -u):$(id -g)"

for platform in "${PLATFORMS[@]}"; do
  zip="snx_jev-$VERSION-duckdb-$DUCKDB_VERSION-$platform.zip"
  say "checking $platform as shipped, in a stock duckdb $DUCKDB_VERSION, and packaging $zip"
  MSYS_NO_PATHCONV=1 docker run --rm --platform "linux/${platform#linux_}" -v "$dist_mount:/dist" \
    -e VERSION="$VERSION" -e PLATFORM="$platform" -e DUCKDB_VERSION="$DUCKDB_VERSION" -e ZIP="$zip" \
    -e SOURCE_COMMIT="$source_commit" -e DOCS_COMMIT="$docs_commit" -e HOST_IDS="$host_ids" \
    python:3.12-slim bash -euo pipefail -c '
      pip install -q --root-user-action=ignore --disable-pip-version-check "duckdb==${DUCKDB_VERSION#v}"
      cd "/dist/$PLATFORM"
      python3 /dist/.package/verify-release.py "$PWD/snx_jev.duckdb_extension" "$VERSION" "$PLATFORM"
      sha256sum snx_jev.duckdb_extension >snx_jev.duckdb_extension.sha256
      cp /dist/.package/USING.md USING.md
      {
        echo "snx_jev $VERSION"
        echo "DuckDB $DUCKDB_VERSION"
        echo "platform $PLATFORM"
        echo "binary built from $SOURCE_COMMIT"
        echo "USING.md from $DOCS_COMMIT"
        cat TESTS.txt 2>/dev/null || echo "tests: not recorded"
      } >VERSION.txt
      rm -f "../$ZIP"
      python3 -m zipfile -c "../$ZIP" snx_jev.duckdb_extension snx_jev.duckdb_extension.sha256 USING.md VERSION.txt
      # hand the files back to the host user (a rootful Docker on Linux would leave them root-owned)
      chown -R "$HOST_IDS" . "../$ZIP" 2>/dev/null || true
      cat snx_jev.duckdb_extension.sha256'
  say "wrote $dist/$zip"
done
