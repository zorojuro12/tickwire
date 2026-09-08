#!/usr/bin/env bash
set -euo pipefail

server_bin="$1"
client1_bin="$2"
client2_bin="$3"

server_out=$(mktemp)
trap 'kill "${server_pid:-}" "${c1_pid:-}" "${c2_pid:-}" 2>/dev/null || true; rm -f "$server_out"' EXIT

"$server_bin" --port 0 >"$server_out" 2>&1 &
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

echo "tw_server listening on port $port"

"$client1_bin" --host 127.0.0.1 --port "$port" &
c1_pid=$!
"$client2_bin" --host 127.0.0.1 --port "$port" &
c2_pid=$!

wait "$c1_pid"
wait "$c2_pid"
