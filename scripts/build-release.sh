#!/usr/bin/env bash
# Builds the snx_jev release binaries, in Docker, from a committed revision.
#
#     ./scripts/build-release.sh 0.2.0                 # linux_amd64 and linux_arm64
#     ./scripts/build-release.sh 0.2.0 linux_amd64     # one platform
#
# Resolves the revision to one commit, exports it (git archive with LF line endings, submodules
# included, so nothing uncommitted gets in), builds each platform in ubuntu:24.04 (linux_arm64
# under QEMU emulation: slow the first time, ccache makes rebuilds quick), runs the mock suite
# against the build, and writes
#
#     dist/<version>/SOURCE_COMMIT
#     dist/<version>/<platform>/snx_jev.duckdb_extension   (+ TESTS.txt)
#
# then runs scripts/package-release.sh, which loads each file exactly as shipped into a stock
# duckdb and writes the zips to hand out: dist/<version>/snx_jev-<version>-duckdb-<duckdb>-<platform>.zip
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

# One commit for everything below, even if HEAD moves during an hours-long arm64 build.
COMMIT="$(git rev-parse "$REV^{commit}")" || die "unknown revision '$REV'"

# The revision's own version string must be the release's: jev_version() and the User-Agent
# report it.
git show "$COMMIT:src/include/jev_client.hpp" | grep -q "#define JEV_VERSION \"$VERSION\"" ||
  die "src/include/jev_client.hpp at $REV does not say JEV_VERSION \"$VERSION\""
# The DuckDB it is built against must be the one the services run.
duckdb_commit="$(git rev-parse "$COMMIT:duckdb")"
git -C duckdb rev-parse -q --verify "$duckdb_commit^{commit}" >/dev/null ||
  die "the duckdb submodule does not have $duckdb_commit; run git submodule update"
[ "$(git -C duckdb describe --tags --exact-match "$duckdb_commit" 2>/dev/null || true)" = "$DUCKDB_VERSION" ] ||
  die "the duckdb submodule at $COMMIT is not $DUCKDB_VERSION"
if [ "$REV" = HEAD ] && ! git diff --quiet HEAD -- src test scripts docs CMakeLists.txt extension_config.cmake Makefile; then
  say "note: uncommitted changes are NOT part of this build (it exports ${COMMIT:0:7})"
fi

dist="dist/$VERSION"
mkdir -p "$dist"
trap 'rm -f "$dist"/src*.tar' EXIT
echo "$COMMIT" >"$dist/SOURCE_COMMIT"
say "exporting ${COMMIT:0:7} with its submodules"
# autocrlf off: Git for Windows defaults would export CRLF scripts that bash cannot run.
git -c core.autocrlf=false archive --format=tar --prefix=src/ "$COMMIT" >"$dist/src.tar"
for sm in duckdb extension-ci-tools; do
  git -C "$sm" -c core.autocrlf=false archive --format=tar --prefix="src/$sm/" "$(git rev-parse "$COMMIT:$sm")"     >"$dist/src-$sm.tar"
done

# Docker on Windows (Git Bash) needs a Windows path for the mount.
dist_mount="$(cd "$dist" && (pwd -W 2>/dev/null || pwd))"

for platform in "${PLATFORMS[@]}"; do
  arch="${platform#linux_}"
  say "building $platform (DuckDB $DUCKDB_VERSION)$([ "$arch" = arm64 ] && echo ' under emulation: expect a long first build')"
  MSYS_NO_PATHCONV=1 docker run --rm --platform "linux/$arch" \
    -e VERSION="$VERSION" -e PLATFORM="$platform" -e DUCKDB_VERSION="$DUCKDB_VERSION" -e JOBS="$JOBS"     -e HOST_IDS="$(id -u):$(id -g)" \
    -v "$dist_mount:/dist" -v "snx-jev-release-$arch:/work" \
    ubuntu:24.04 bash -c 'rm -rf /work/src && mkdir -p /work && for t in /dist/src*.tar; do tar -xf "$t" -C /work; done &&
                          bash /work/src/scripts/release-in-container.sh'
done
rm -f "$dist"/src*.tar

./scripts/package-release.sh "$VERSION" "${PLATFORMS[@]}"
say "done"
ls -l "$dist"/*.zip
