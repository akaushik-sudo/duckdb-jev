#!/usr/bin/env bash
# Builds the snx_jev release binaries, in Docker, from a committed revision.
#
#     ./scripts/build-release.sh 0.2.0                 # linux_amd64 and linux_arm64
#     ./scripts/build-release.sh 0.2.0 linux_amd64     # one platform
#
# For each platform this exports the revision (git archive, submodules included, so nothing
# uncommitted gets in), builds it in ubuntu:24.04 (linux_arm64 under QEMU emulation: slow the
# first time, ccache makes rebuilds quick), runs the mock suite against the build, loads the
# binary into a stock duckdb 1.5.4 to check its platform and stamped version, and writes
#
#     dist/<version>/<platform>/snx_jev.duckdb_extension
#
# then runs scripts/package-release.sh, which checks each file again exactly as shipped and writes
# the zips to hand out: dist/<version>/snx_jev-<version>-duckdb-<duckdb>-<platform>.zip
#
# Needs: git, Docker (Docker Desktop on Windows/macOS, which ships QEMU for arm64).
# Overridable: SNX_JEV_REV (default HEAD), SNX_JEV_JOBS (parallel compile jobs, default 5).
set -euo pipefail

VERSION=${1:?usage: scripts/build-release.sh <version> [platform ...]}
shift
PLATFORMS=("$@")
[ ${#PLATFORMS[@]} -gt 0 ] || PLATFORMS=(linux_amd64 linux_arm64)
REV=${SNX_JEV_REV:-HEAD}
JOBS=${SNX_JEV_JOBS:-5}
DUCKDB_VERSION=v1.5.4

say() { printf '\033[36m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "version must look like 0.2.0 (got '$VERSION')"
for platform in "${PLATFORMS[@]}"; do
  case "$platform" in linux_amd64|linux_arm64) ;; *) die "unsupported platform '$platform'" ;; esac
done

# The revision's own version string must be the release's: jev_version() and the User-Agent
# report it.
git show "$REV:src/include/jev_client.hpp" | grep -q "#define JEV_VERSION \"$VERSION\"" ||
  die "src/include/jev_client.hpp at $REV does not say JEV_VERSION \"$VERSION\""
# The DuckDB it is built against must be the one the services run.
duckdb_commit="$(git rev-parse "$REV:duckdb")"
git -C duckdb rev-parse -q --verify "$duckdb_commit^{commit}" >/dev/null ||
  die "the duckdb submodule does not have $duckdb_commit; run git submodule update"
[ "$(git -C duckdb describe --tags --exact-match "$duckdb_commit" 2>/dev/null || true)" = "$DUCKDB_VERSION" ] ||
  die "the duckdb submodule at $REV is not $DUCKDB_VERSION"
if [ "$REV" = HEAD ] && ! git diff --quiet HEAD -- src test scripts CMakeLists.txt extension_config.cmake Makefile; then
  say "note: uncommitted changes are NOT part of this build (it exports $(git rev-parse --short HEAD))"
fi

dist="dist/$VERSION"
mkdir -p "$dist"
say "exporting $(git rev-parse --short "$REV") with its submodules"
git archive --format=tar --prefix=src/ "$REV" >"$dist/src.tar"
for sm in duckdb extension-ci-tools; do
  git -C "$sm" archive --format=tar --prefix="src/$sm/" "$(git rev-parse "$REV:$sm")" >"$dist/src-$sm.tar"
done

# Docker on Windows (Git Bash) needs a Windows path for the mount.
dist_mount="$(cd "$dist" && (pwd -W 2>/dev/null || pwd))"

for platform in "${PLATFORMS[@]}"; do
  arch="${platform#linux_}"
  say "building $platform (DuckDB $DUCKDB_VERSION)$([ "$arch" = arm64 ] && echo ' under emulation: expect a long first build')"
  MSYS_NO_PATHCONV=1 docker run --rm --platform "linux/$arch" \
    -e VERSION="$VERSION" -e PLATFORM="$platform" -e DUCKDB_VERSION="$DUCKDB_VERSION" -e JOBS="$JOBS" \
    -v "$dist_mount:/dist" -v "snx-jev-release-$arch:/work" \
    ubuntu:24.04 bash -c 'rm -rf /work/src && mkdir -p /work && for t in /dist/src*.tar; do tar -xf "$t" -C /work; done &&
                          bash /work/src/scripts/release-in-container.sh'
done
rm -f "$dist"/src*.tar

SNX_JEV_REV="$REV" ./scripts/package-release.sh "$VERSION" "${PLATFORMS[@]}"
say "done"
ls -l "$dist"/*.zip
