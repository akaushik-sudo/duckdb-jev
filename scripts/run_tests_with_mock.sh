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

# Every case: a test file, then the environment its unittest process gets. The value of an
# assignment must not contain whitespace (the cases are split on it).
CASES=()
case_() { CASES+=("$*"); }

# No key and no endpoint override: what any environment can check
case_ test/sql/jev_offline.test
case_ test/sql/jev_no_key.test JEV_ASSERT_NO_API_KEY=1 SNX_JEV_API_URL="$mock_url"

# The judgment functions against the mock
case_ test/sql/jev_api.test JEV_MOCK_API_URL="$mock_url" SNX_JEV_API_URL="$mock_url" TYPESAFE_API_KEY="$MOCK_KEY"
case_ test/sql/jev_wrong_key.test JEV_ASSERT_WRONG_KEY=1 SNX_JEV_API_URL="$mock_url" TYPESAFE_API_KEY=not-the-key

# A loopback port nothing listens on
case_ test/sql/jev_unreachable.test JEV_ASSERT_UNREACHABLE=1 SNX_JEV_API_URL="http://127.0.0.1:1/v1/systemone" \
    TYPESAFE_API_KEY="$MOCK_KEY"

# Endpoints that must be refused. The ones that name the mock's port would reach it if the
# check let them through, so passing here means nothing was sent. `snxleak` must never
# appear in the error (the test asserts it), since a refused URL can carry credentials.
for hostile in \
    "https://evil.example/v1/systemone" \
    "https://api.typesafe.ai.evil.example/v1/systemone" \
    "http://127.0.0.1.nip.io:${PORT}/v1/systemone" \
    "http://localhost:${PORT}@127.0.0.1:${PORT}/v1/systemone" \
    "http://evil.example@127.0.0.1:${PORT}/v1/systemone" \
    "http://user:snxleak@127.0.0.1:${PORT}/v1/systemone" \
    "http://127.0.0.1:${PORT}\\@evil.example/v1/systemone" \
    "http://[::1]evil.example:${PORT}/v1/systemone" \
    "http://127.0.0.1:${PORT}x/v1/systemone" \
    "http://127.0.0.1:99999999999/v1/systemone" \
    "http://127.0.0.1:65536/v1/systemone" \
    "http://127.0.0.1:0/v1/systemone" \
    "http://127.0.0.1:/v1/systemone" \
    "http://127.0.0.1:${PORT}/v1/systemone?host=evil.example" \
    "http://127.0.0.1:${PORT}/v1/%2e%2e/systemone" \
    "ftp://127.0.0.1:${PORT}/v1/systemone" \
    "127.0.0.1:${PORT}/v1/systemone"; do
  case_ test/sql/jev_host_refused.test JEV_ASSERT_HOST_REFUSED=1 SNX_JEV_API_URL="$hostile" TYPESAFE_API_KEY="$MOCK_KEY"
done

# A test file with no case would never run here, and `make test` skips it for lack of its
# environment: CI would stay green with no coverage. Refuse that.
for f in test/sql/*.test; do
  covered=false
  for c in "${CASES[@]}"; do
    if [ "${c%% *}" = "$f" ]; then covered=true; fi
  done
  if ! $covered; then
    echo "$f has no case in $0 - add one" >&2
    exit 1
  fi
done

# run <test file> [VAR=value ...]: one unittest process with only the given snx_jev environment.
run() {
  local test_file=$1
  shift
  local refused=false url=""
  for assignment in "$@"; do
    case "$assignment" in
      JEV_ASSERT_HOST_REFUSED=*) refused=true ;;
      SNX_JEV_API_URL=*) url=${assignment#SNX_JEV_API_URL=} ;;
    esac
  done
  # Anything that may send must send to loopback: never let a case fall through to the live API.
  if ! $refused && [ "$test_file" != test/sql/jev_offline.test ]; then
    case "$url" in
      http://127.0.0.1:*|http://localhost:*) ;;
      *) echo "refusing to run ${test_file} with SNX_JEV_API_URL='${url}': cases send to loopback only" >&2; exit 1 ;;
    esac
  fi
  echo "== ${test_file} $*"
  env -u TYPESAFE_API_KEY -u SNX_JEV_API_URL \
      -u JEV_MOCK_API_URL -u JEV_ASSERT_NO_API_KEY -u JEV_ASSERT_WRONG_KEY \
      -u JEV_ASSERT_UNREACHABLE -u JEV_ASSERT_HOST_REFUSED \
      "$@" "$unittest" --test-dir . "$test_file"
}

# ./scripts/run_tests_with_mock.sh [test/sql/<file>.test]: every case, or only that file's.
only=${1:-}
matched=false
for c in "${CASES[@]}"; do
  read -r -a parts <<<"$c"
  if [ -n "$only" ] && [ "${parts[0]}" != "$only" ]; then
    continue
  fi
  matched=true
  run "${parts[@]}"
done
if [ -n "$only" ] && ! $matched; then
  echo "no case runs '${only}' - pass a path like test/sql/jev_api.test" >&2
  exit 1
fi
