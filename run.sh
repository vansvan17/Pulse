#!/usr/bin/env bash
# benchmarks/run.sh — reproducible benchmark run.
#
# Outputs benchmark results to stdout. Suitable for piping into a file
# committed alongside the README. Re-run after every meaningful change.
#
# Requires: wrk, ab. On Debian/Ubuntu:
#   apt-get install wrk apache2-utils

set -euo pipefail

cd "$(dirname "$0")/.."

if [[ ! -x ./build/pulse ]]; then
    echo "Build first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
    exit 1
fi

PORT=${PORT:-18099}
DURATION=${DURATION:-10s}
WORKERS=${WORKERS:-1}

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "=== Pulse benchmark ==="
echo "Port:     $PORT"
echo "Duration: $DURATION"
echo "Workers:  $WORKERS"
echo "Cores:    $(nproc)"
echo "Kernel:   $(uname -r)"
echo

./build/pulse --port "$PORT" --doc-root ./www --workers "$WORKERS" 2>/dev/null &
SERVER_PID=$!
sleep 0.5

echo "--- ab: 100k req, 100 conn, keep-alive ---"
ab -n 100000 -c 100 -k -q "http://127.0.0.1:$PORT/" 2>&1 \
    | grep -E "Requests per|Time per|Failed|Transfer rate|Percentage|50%|95%|99%|100%"

echo
echo "--- wrk: 4 threads, 100 conn, $DURATION ---"
wrk -t4 -c100 -d"$DURATION" --latency "http://127.0.0.1:$PORT/" 2>&1 | tail -15

echo
echo "--- wrk: 8 threads, 1000 conn, $DURATION ---"
wrk -t8 -c1000 -d"$DURATION" --latency "http://127.0.0.1:$PORT/" 2>&1 | tail -15

echo
echo "--- 404 path under load (should not regress) ---"
wrk -t4 -c100 -d5s "http://127.0.0.1:$PORT/does-not-exist" 2>&1 | tail -8
