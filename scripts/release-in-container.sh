#!/usr/bin/env bash
# Runs inside ubuntu:24.04 for scripts/build-release.sh: builds /work/src for one platform, tests
# it, checks the binary in a stock duckdb, and copies it to /dist. Not meant to be run by hand.
set -euo pipefail
: "${VERSION:?}" "${PLATFORM:?}" "${DUCKDB_VERSION:?}" "${JOBS:=5}"
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq build-essential cmake ninja-build libssl-dev python3 python3-venv ccache git >/dev/null

cd /work/src
export OVERRIDE_GIT_DESCRIBE="$DUCKDB_VERSION" GEN=ninja CMAKE_BUILD_PARALLEL_LEVEL="$JOBS"
export SNX_JEV_VERSION="v$VERSION"
export CCACHE_DIR=/work/ccache CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
make release

# The mock suite against this build. Under arm64 emulation everything runs ~10x slower, which
# breaks jev_concurrency.test's timing on purpose-built overlap windows; it runs on amd64.
for f in test/sql/*.test; do
  if [ "$PLATFORM" = linux_arm64 ] && [ "$f" = test/sql/jev_concurrency.test ]; then
    echo "== skipping $f under emulation (timing-dependent; covered by the linux_amd64 build)"
    continue
  fi
  ./scripts/run_tests_with_mock.sh "$f"
done

# The shipped file keeps the name snx_jev.duckdb_extension (DuckDB derives the entry point from
# it); the version is in the folder and zip names. Checked as shipped, in a stock duckdb.
mkdir -p "/dist/$PLATFORM"
cp build/release/extension/snx_jev/snx_jev.duckdb_extension "/dist/$PLATFORM/snx_jev.duckdb_extension"
python3 -m venv /tmp/venv
/tmp/venv/bin/pip -q install "duckdb==${DUCKDB_VERSION#v}"
/tmp/venv/bin/python scripts/verify-release.py "/dist/$PLATFORM/snx_jev.duckdb_extension" "$VERSION" "$PLATFORM"
echo "== wrote /dist/$PLATFORM/snx_jev.duckdb_extension"
