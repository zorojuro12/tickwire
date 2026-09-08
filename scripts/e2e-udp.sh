#!/usr/bin/env bash
set -euo pipefail

server_bin="$1"
loadclient_bin="$2"

server_out=$(mktemp)
trap 'kill "${server_pid:-}" 2>/dev/null || true; rm -f "$server_out"' EXIT

"$server_bin" --port 0 --ticks 600 >"$server_out" 2>&1 &
server_pid=$!

port=""
for _ in $(seq 1 50); do
  if grep -q "^port=" "$server_out" 2>/dev/null; then
    port=$(grep "^port=" "$server_out" | head -1 | cut -d= -f2)
    break
  fi
  sleep 0.1
done

if [[ -z "$port" ]]; then
  echo "FAIL: tw_server never printed a port line within 5 seconds" >&2
  cat "$server_out" >&2
  exit 1
fi

"$loadclient_bin" --host 127.0.0.1 --port "$port" --players 4 --ticks 300
