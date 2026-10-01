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

binary=build/release/extension/snx_jev/snx_jev.duckdb_extension
python3 -m venv /tmp/venv
/tmp/venv/bin/pip -q install "duckdb==${DUCKDB_VERSION#v}"
/tmp/venv/bin/python - "$binary" <<'PY'
import os, sys, duckdb
binary = sys.argv[1]
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute(f"LOAD '{binary}'")
platform = con.execute("PRAGMA platform").fetchone()[0]
row = con.execute("SELECT extension_version FROM duckdb_extensions() WHERE extension_name = 'snx_jev'").fetchone()
version = con.execute("SELECT jev_version()").fetchone()[0]
functions = sorted(r[0] for r in con.execute(
    "SELECT DISTINCT function_name FROM duckdb_functions() WHERE function_name LIKE 'snx_%' OR function_name LIKE 'jev%'").fetchall())
print(f"loaded into duckdb {duckdb.__version__} on {platform}: extension_version={row[0]} jev_version()={version} functions={functions}")
assert platform == os.environ["PLATFORM"], platform
assert row[0] == "v" + os.environ["VERSION"], row
assert version == os.environ["VERSION"], version
assert functions == ["jev_version", "snx_jev_last_query_stats", "snx_prompt_intent"], functions
PY

name="snx_jev-${VERSION}-duckdb-${DUCKDB_VERSION}-${PLATFORM}.duckdb_extension"
cp "$binary" "/dist/$name"
( cd /dist && sha256sum "$name" >"$name.sha256" )
echo "== wrote /dist/$name"
