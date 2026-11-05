#!/usr/bin/env bash
# Compile the generic DACA relationship runtime and run its end-to-end demo.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
runtime_bin="$script_dir/daca_relationship_runtime"
socket_path="${XDG_RUNTIME_DIR:-/tmp}/daca-relationship-demo-${UID}.sock"
daemon_pid=""

cleanup() {
    if [[ -n "$daemon_pid" ]] && kill -0 "$daemon_pid" 2>/dev/null; then
        kill -TERM "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -f "$socket_path"
}
trap cleanup EXIT INT TERM

"${CXX:-g++}" -O3 -std=c++17 -Wall -Wextra -Wpedantic -pthread \
    "$script_dir/daca_relationship_runtime.cpp" -o "$runtime_bin"

rm -f "$socket_path"
"$runtime_bin" --daemon --socket "$socket_path" --memory-mib 32 \
    >"$script_dir/daca_relationship_daemon.log" 2>&1 &
daemon_pid=$!

for _ in $(seq 1 100); do
    [[ -S "$socket_path" ]] && break
    sleep 0.05
done
if [[ ! -S "$socket_path" ]]; then
    echo "DACA daemon did not create its socket." >&2
    cat "$script_dir/daca_relationship_daemon.log" >&2 || true
    exit 1
fi

"$runtime_bin" --demo --socket "$socket_path"

kill -TERM "$daemon_pid"
wait "$daemon_pid" || true
daemon_pid=""
cat "$script_dir/daca_relationship_daemon.log"
