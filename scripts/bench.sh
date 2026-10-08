#!/usr/bin/env bash
# Load tests on loopback. Backends, proxy and load generator are pinned to
# separate CPUs, at most eight busy at once, so each can be the bottleneck on
# its own. Every number is the median of three runs. Prints one line per
# measurement: a label, a tab, then JSON.
#
#   scripts/bench.sh [build-dir]        # Linux, at least 8 CPUs
set -euo pipefail
BIN=${1:-build}
RUN_SECONDS=${RUN_SECONDS:-5}
BACKEND_CPUS=0-1
PROXY_CPUS=2-4
LOAD_CPUS=5-7
TMP=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$TMP"' EXIT

backend() {  # port [extra echo-server flags]
    local port=$1; shift
    taskset -c $BACKEND_CPUS "$BIN/echo-server" --listen 127.0.0.1:$port --threads 2 "$@" &
    sleep 0.2
}

stop_backends() { kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null || true; }

start_proxy() {  # workers buffer_kb strategy retries eject_for_s backend-ports...
    local workers=$1 buffer=$2 strategy=$3 retries=$4 eject=$5; shift 5
    {
        echo "workers $workers"
        echo "buffer_kb $buffer"
        echo "retries $retries"
        echo "eject_for_s $eject"
        echo "log $TMP/telemetry.jsonl"
        echo "pool p $strategy $(printf '127.0.0.1:%s ' "$@")"
        echo "listen 127.0.0.1:8000 p"
    } > "$TMP/proxy.conf"
    : > "$TMP/telemetry.jsonl"
    taskset -c $PROXY_CPUS "$BIN/tcp-proxy" -c "$TMP/proxy.conf" 2> "$TMP/proxy.err" &
    PROXY_PID=$!
    sleep 0.2
}

stop_proxy() { kill -INT $PROXY_PID; wait $PROXY_PID 2>/dev/null || true; }

loadgen() {  # port mode connections size [seconds]
    taskset -c $LOAD_CPUS "$BIN/loadgen" --target 127.0.0.1:$1 --mode $2 --connections $3 \
        --size $4 --seconds ${5:-$RUN_SECONDS} --warmup 1 --threads 3 --json
}

# The proxy's CPU time over a run, in microseconds per request, from /proc.
cpu_per_request() {  # loadgen args...
    loadgen "$@" > "$TMP/run.json" &
    local load=$!
    sleep 1.5
    local before after
    before=$(awk '{print $14, $15}' /proc/$PROXY_PID/stat)
    sleep 3
    after=$(awk '{print $14, $15}' /proc/$PROXY_PID/stat)
    wait $load
    local hz rps
    hz=$(getconf CLK_TCK)
    rps=$(grep -o '"rps":[0-9]*' "$TMP/run.json" | cut -d: -f2)
    echo "$before $after $rps $hz" | awk '{ printf "{\"rps\":%d,\"user_us\":%.2f,\"sys_us\":%.2f}\n",
        $5, ($3 - $1) / $6 / 3 / $5 * 1e6, ($4 - $2) / $6 / 3 / $5 * 1e6 }'
}

# Runs "$@" three times and prints the median line by its first number field
# among rps and gbit_per_s.
median3() {
    for _ in 1 2 3; do "$@"; done | awk '{
        v = $0; if (match(v, /"gbit_per_s":[0-9.]+/) && $0 ~ /"stream"/) k = substr(v, RSTART + 13, RLENGTH - 13);
        else { match(v, /"rps":[0-9.]+/); k = substr(v, RSTART + 6, RLENGTH - 6) }
        print k "\t" $0 }' | sort -n | sed -n 2p | cut -f2-
}

proxied() {  # workers buffer strategy retries eject ports -- loadgen args
    local args=()
    while [ "$1" != "--" ]; do args+=("$1"); shift; done
    shift
    start_proxy "${args[@]}"
    loadgen "$@"
    stop_proxy
}

say() { printf '%s\t%s\n' "$1" "$2"; }

echo "# 1. Latency added, one connection, 64-byte request/response"
backend 9001
say "direct" "$(median3 loadgen 9001 rr 1 64)"
say "proxy" "$(median3 proxied 1 16 round-robin 2 5 9001 -- 8000 rr 1 64)"

echo "# 2. Throughput and CPU per request against workers, 256 connections"
say "direct" "$(median3 loadgen 9001 rr 256 64)"
for workers in 1 2 3; do
    say "proxy w=$workers" "$(median3 proxied $workers 16 round-robin 2 5 9001 -- 8000 rr 256 64)"
    start_proxy $workers 16 round-robin 2 5 9001
    say "cpu w=$workers" "$(cpu_per_request 8000 rr 256 64 5)"
    stop_proxy
done

echo "# 3. Bulk throughput against buffer size: 8 streams of 64 KiB writes"
for kb in 4 16 64 256; do
    say "buffer ${kb}KiB" "$(median3 proxied 3 $kb round-robin 2 5 9001 -- 8000 stream 8 65536)"
    start_proxy 3 $kb round-robin 2 5 9001
    loadgen 8000 stream 8 65536 3 > /dev/null
    stop_proxy
    say "syscalls ${kb}KiB" "$(awk -F'[:,]' '{ for (i = 1; i < NF; i++) {
            if ($i == "\"bytes_up\"" || $i == "\"bytes_down\"") b += $(i+1);
            if ($i == "\"reads\"") r += $(i+1); if ($i == "\"writes\"") w += $(i+1) } }
        END { printf "{\"reads_per_mb\":%.1f,\"writes_per_mb\":%.1f}", r / (b / 1e6), w / (b / 1e6) }' "$TMP/telemetry.jsonl")"
done

echo "# 4. Connection churn: a new connection for every 64-byte request, 64 at a time"
say "direct" "$(median3 loadgen 9001 connect 64 64)"
say "proxy w=3" "$(median3 proxied 3 16 round-robin 2 5 9001 -- 8000 connect 64 64)"
stop_backends

echo "# 5. One slow backend (+20 ms) among three, a new connection per request"
backend 9101; backend 9102; backend 9103 --delay-ms 20
for strategy in round-robin least-connections; do
    say "$strategy" "$(median3 proxied 3 16 $strategy 2 5 9101 9102 9103 -- 8000 connect 48 64)"
done
stop_backends

echo "# 6. A backend is killed 3 s into a 10 s run, a new connection per request"
for setting in "0 0" "2 0" "2 5"; do
    read -r retries eject <<< "$setting"
    backend 9201; backend 9202; backend 9203
    victim=$!
    start_proxy 3 16 round-robin $retries $eject 9201 9202 9203
    (sleep 4; kill $victim) &
    say "retries=$retries eject_for_s=$eject" "$(loadgen 8000 connect 48 64 10)"
    stop_proxy
    say "proxy" "\"$(tail -1 "$TMP/proxy.err")\""
    stop_backends
done
