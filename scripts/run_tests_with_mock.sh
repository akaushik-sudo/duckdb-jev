#!/usr/bin/env bash
# Runs the regression suite with the deterministic mock API in front of it.
#
# The endpoint (SNX_JEV_API_URL) and the key (TYPESAFE_API_KEY) come only from the
# environment the process starts with, and SQL can change neither, so each case below is
# its own unittest process with its own environment. Every run first removes both from the
# inherited environment, so a developer's real key or URL can never turn a case into a live
# request.
set -euo pipefail

BUILD=${BUILD:-release}
PORT=${JEV_MOCK_PORT:-18765}
MOCK_KEY=jev-test-key
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

unittest="./build/${BUILD}/test/unittest"
if [ ! -x "$unittest" ]; then
  echo "no test runner at $unittest - run 'make ${BUILD}' first" >&2
  exit 1
fi

python3 test/mock_api.py "$PORT" &
mock_pid=$!
trap 'kill ${mock_pid} 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
  if ! kill -0 "${mock_pid}" 2>/dev/null; then
    # Almost always "address already in use": something else owns the port, and
    # running the suite against it would test that something else.
    echo "the mock API did not start on port ${PORT} - set JEV_MOCK_PORT to a free port" >&2
    exit 1
  fi
  if python3 -c "import socket,sys; socket.create_connection(('127.0.0.1', ${PORT}), 0.2).close()" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

mock_url="http://127.0.0.1:${PORT}/v1/systemone"

# run <test file> [VAR=value ...]: one unittest process with only the given snx_jev environment.
run() {
  local test_file=$1
  shift
  echo "== ${test_file} $*"
  env -u TYPESAFE_API_KEY -u SNX_JEV_API_URL \
      -u JEV_MOCK_API_URL -u JEV_ASSERT_NO_API_KEY -u JEV_ASSERT_WRONG_KEY \
      -u JEV_ASSERT_UNREACHABLE -u JEV_ASSERT_HOST_REFUSED \
      "$@" "$unittest" --test-dir . "$test_file"
}

# No key and no endpoint override: what any environment can check
run test/sql/jev_offline.test
run test/sql/jev_no_key.test JEV_ASSERT_NO_API_KEY=1 SNX_JEV_API_URL="$mock_url"

# The judgment functions against the mock
run test/sql/jev_api.test JEV_MOCK_API_URL="$mock_url" SNX_JEV_API_URL="$mock_url" TYPESAFE_API_KEY="$MOCK_KEY"
run test/sql/jev_wrong_key.test JEV_ASSERT_WRONG_KEY=1 SNX_JEV_API_URL="$mock_url" TYPESAFE_API_KEY=not-the-key

# A loopback port nothing listens on
run test/sql/jev_unreachable.test JEV_ASSERT_UNREACHABLE=1 SNX_JEV_API_URL="http://127.0.0.1:1/v1/systemone" \
    TYPESAFE_API_KEY="$MOCK_KEY"

# Endpoints that must be refused. The ones that name the mock's port would reach it if the
# check let them through, so passing here means nothing was sent.
for hostile in \
    "https://evil.example/v1/systemone" \
    "https://api.typesafe.ai.evil.example/v1/systemone" \
    "http://127.0.0.1.nip.io:${PORT}/v1/systemone" \
    "http://localhost:${PORT}@127.0.0.1:${PORT}/v1/systemone" \
    "http://evil.example@127.0.0.1:${PORT}/v1/systemone" \
    "http://127.0.0.1:${PORT}\\@evil.example/v1/systemone" \
    "http://[::1]evil.example:${PORT}/v1/systemone" \
    "http://127.0.0.1:${PORT}x/v1/systemone" \
    "ftp://127.0.0.1:${PORT}/v1/systemone" \
    "127.0.0.1:${PORT}/v1/systemone"; do
  run test/sql/jev_host_refused.test JEV_ASSERT_HOST_REFUSED=1 SNX_JEV_API_URL="$hostile" TYPESAFE_API_KEY="$MOCK_KEY"
done
