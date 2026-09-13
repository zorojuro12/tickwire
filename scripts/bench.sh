#!/usr/bin/env bash
# Regenerates every row of the P5 numbers table. Run inside the pinned
# toolchain image (`scripts/tw bash scripts/bench.sh`), against a build
# already configured at build/plain (RelWithDebInfo not required -- the
# default plain config is what CI itself builds and tests).
#
# --smoke shortens every run so this stays usable as a CI-time check that the
# harness still works, without paying for the full multi-minute measurement.
# The real table comes from a plain `scripts/bench.sh` run.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

smoke=0
if [[ "${1:-}" == "--smoke" ]]; then
  smoke=1
fi

server_bin="$repo_root/build/plain/tw_server"
loadclient_bin="$repo_root/build/plain/tw_loadclient"
bench_queue_bin="$repo_root/build/plain/bench_queue"

for bin in "$server_bin" "$loadclient_bin" "$bench_queue_bin"; do
  if [[ ! -x "$bin" ]]; then
    echo "bench.sh: missing $bin -- build build/plain first" >&2
    exit 1
  fi
done

echo "# machine: $(uname -srm)"
echo "# nproc: $(nproc)"
echo "# image: tickwire-dev:gcc10-cmake3.28.4-x11"
echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "# command: scripts/tw bash scripts/bench.sh"

rows=0
server_pid=""
server_out=""
port=""

cleanup() {
  if [[ -n "$server_pid" ]]; then
    kill "$server_pid" 2>/dev/null || true
  fi
  if [[ -n "$server_out" ]]; then
    rm -f "$server_out"
  fi
}
trap cleanup EXIT

# Starts tw_server in the background with the given args; sets `port`,
# `server_pid`, `server_out` for the caller. Exits the whole script if the
# server never prints its port line within 5 seconds.
start_server() {
  server_out=$(mktemp)
  "$server_bin" "$@" >"$server_out" 2>&1 &
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
    echo "bench.sh: tw_server never printed a port line" >&2
    cat "$server_out" >&2
    exit 1
  fi
}

# parse_field <file> <key> -- extracts the first "<key>=<digits>" match.
parse_field() {
  grep -o "$2=[0-9]*" "$1" | head -1 | cut -d= -f2
}

if [[ $smoke -eq 1 ]]; then
  ticks_jitter=60
  ticks_cliff=60
  ticks_throughput=60
  bench_items=10000
  # A paced (--rate 640) bench_queue run takes items/rate seconds; 10000 at
  # 640/s is ~15.6s per ring, which a CI-time smoke check shouldn't pay for.
  # The saturation (--rate 0) run is unaffected -- it runs flat out regardless
  # of item count.
  bench_items_paced=200
else
  ticks_jitter=1200
  ticks_cliff=600
  ticks_throughput=600
  bench_items=10000
  bench_items_paced=10000
fi

echo "## group1: tick_jitter_vs_players"
printf 'players\tp50_ns\tp99_ns\tmax_ns\n'
for players in 1 2 4 8 16 32; do
  start_server --port 0 --ticks "$ticks_jitter" --threads 2 --ring spsc
  "$loadclient_bin" --host 127.0.0.1 --port "$port" --players "$players" \
    --ticks "$ticks_jitter" >/dev/null
  wait "$server_pid" 2>/dev/null || true
  server_pid=""
  p50=$(parse_field "$server_out" p50)
  p99=$(parse_field "$server_out" p99)
  maxv=$(parse_field "$server_out" max)
  printf '%s\t%s\t%s\t%s\n' "$players" "$p50" "$p99" "$maxv"
  rm -f "$server_out"
  server_out=""
  rows=$((rows + 1))
done

echo "## group2: jitter_cliff_vs_sim_load_us"
printf 'sim_load_us\tp50_ns\tp99_ns\tmax_ns\n'
cliff_us=""
for load in 0 2000 8000 14000 16000 18000 20000; do
  start_server --port 0 --ticks "$ticks_cliff" --threads 2 --ring spsc --sim-load-us "$load"
  wait "$server_pid" 2>/dev/null || true
  server_pid=""
  p50=$(parse_field "$server_out" p50)
  p99=$(parse_field "$server_out" p99)
  maxv=$(parse_field "$server_out" max)
  printf '%s\t%s\t%s\t%s\n' "$load" "$p50" "$p99" "$maxv"
  if [[ -z "$cliff_us" && "$p99" -gt 16666667 ]]; then
    cliff_us="$load"
  fi
  rm -f "$server_out"
  server_out=""
  rows=$((rows + 1))
done
echo "# jitter_cliff_sim_load_us=${cliff_us:-none}"

echo "## group3: queue_handoff_latency_ns"
printf 'ring\trate\tns_per_handoff_p50\tns_per_handoff_p99\n'
for ring in spsc mutex; do
  for rate in 0 640; do
    items=$bench_items
    if [[ "$rate" -ne 0 ]]; then
      items=$bench_items_paced
    fi
    out=$("$bench_queue_bin" --ring "$ring" --items "$items" --rate "$rate")
    p50=$(echo "$out" | grep -o 'ns_per_handoff_p50=[0-9]*' | cut -d= -f2)
    p99=$(echo "$out" | grep -o 'ns_per_handoff_p99=[0-9]*' | cut -d= -f2)
    printf '%s\t%s\t%s\t%s\n' "$ring" "$rate" "$p50" "$p99"
    rows=$((rows + 1))
  done
done

echo "## group4: packet_throughput_batch_ingest"
printf 'batch_ingest\tpackets_ingested\n'
for batch in 0 1; do
  args=(--port 0 --ticks "$ticks_throughput" --threads 2 --ring spsc)
  if [[ $batch -eq 1 ]]; then
    args+=(--batch-ingest)
  fi
  start_server "${args[@]}"
  "$loadclient_bin" --host 127.0.0.1 --port "$port" --players 32 \
    --ticks "$ticks_throughput" >/dev/null
  wait "$server_pid" 2>/dev/null || true
  server_pid=""
  pi=$(parse_field "$server_out" packets_ingested)
  printf '%s\t%s\n' "$batch" "$pi"
  rm -f "$server_out"
  server_out=""
  rows=$((rows + 1))
done

echo "rows=$rows"
