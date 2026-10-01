#!/usr/bin/env bash
# Runs inside ubuntu:24.04 for scripts/build-release.sh: builds /work/src for one platform, runs the
# mock suite against it, and leaves /dist/<platform>/snx_jev.duckdb_extension with TESTS.txt.
# scripts/package-release.sh checks the file as shipped and zips it. Not meant to be run by hand.
set -euo pipefail
: "${VERSION:?}" "${PLATFORM:?}" "${DUCKDB_VERSION:?}" "${JOBS:=5}" "${HOST_IDS:=}"
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq build-essential cmake ninja-build libssl-dev python3 ccache git >/dev/null

cd /work/src
export OVERRIDE_GIT_DESCRIBE="$DUCKDB_VERSION" GEN=ninja CMAKE_BUILD_PARALLEL_LEVEL="$JOBS"
export SNX_JEV_VERSION="v$VERSION"
export CCACHE_DIR=/work/ccache CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
make release

# The mock suite against this build. Under arm64 emulation everything runs ~10x slower, which
# breaks jev_concurrency.test's purpose-built overlap windows; TESTS.txt says so, and it ships
# in the zip's VERSION.txt.
ran=()
skipped=()
for f in test/sql/*.test; do
  if [ "$PLATFORM" = linux_arm64 ] && [ "$f" = test/sql/jev_concurrency.test ]; then
    echo "== skipping $f under emulation (timing-dependent)"
    skipped+=("$(basename "$f")")
    continue
  fi
  ./scripts/run_tests_with_mock.sh "$f"
  ran+=("$(basename "$f")")
done

out="/dist/$PLATFORM"
mkdir -p "$out"
# The shipped name: DuckDB derives the extension's entry point from it.
cp build/release/extension/snx_jev/snx_jev.duckdb_extension "$out/snx_jev.duckdb_extension"
{
  echo "tests passed on this build: ${ran[*]}"
  [ ${#skipped[@]} -eq 0 ] || echo "tests skipped on this build (emulated, timing-dependent): ${skipped[*]}"
} >"$out/TESTS.txt"
[ -z "$HOST_IDS" ] || chown -R "$HOST_IDS" "$out" 2>/dev/null || true
echo "== wrote $out/snx_jev.duckdb_extension"
