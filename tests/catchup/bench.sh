#!/bin/sh
# Catch-up benchmarks for H1 to H3. See README.md in this folder.
#   bench.sh gap <gap-mb> [replicas]   disconnect replicas, write a gap, time the reconnect
#   bench.sh steady [requests]         primary write throughput and latency with replicas attached
set -eu

: "${VALKEY_SRC:?set VALKEY_SRC to the src folder with valkey-server, valkey-cli and valkey-benchmark}"
PORT=${BASE_PORT:-21600}
DATASET_MB=${DATASET_MB:-256}
BACKLOG=${BACKLOG:-10mb}
VALUE=${VALUE:-1024}
PRIMARY_ARGS=${PRIMARY_ARGS:-}

work=$(mktemp -d)
pids=""
cleanup() {
    for p in $pids; do kill -CONT "$p" 2>/dev/null || true; done
    for p in $pids; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

cli() { port=$1; shift; "$VALKEY_SRC/valkey-cli" -p "$port" "$@"; }
info() { cli "$1" info "$2" | tr -d '\r' | awk -F: -v k="$3" '$1 == k { print $2 }'; }
now_ms() { date +%s%3N; }

start() {
    port=$1; shift
    mkdir -p "$work/$port"
    "$VALKEY_SRC/valkey-server" --port "$port" --dir "$work/$port" --save '' --appendonly no \
        --enable-debug-command yes --logfile "$work/$port/log" "$@" &
    pids="$pids $!"
    last=$!
    until cli "$port" ping >/dev/null 2>&1; do sleep 0.05; done
}

# Replica caught up: link up and its offset equals the primary's.
synced() {
    want=$(info "$PORT" replication master_repl_offset)
    [ "$(info "$1" replication master_link_status)" = up ] &&
        [ "$(info "$1" replication slave_repl_offset)" = "$want" ]
}

setup() {
    n=$1
    # shellcheck disable=SC2086
    start "$PORT" --repl-backlog-size "$BACKLOG" $PRIMARY_ARGS
    replica_pids=""
    cli "$PORT" debug populate $((DATASET_MB * 1024 * 1024 / VALUE)) key "$VALUE" >/dev/null
    i=1
    while [ "$i" -le "$n" ]; do
        start $((PORT + i)) --replicaof 127.0.0.1 "$PORT"
        replica_pids="$replica_pids $last"
        i=$((i + 1))
    done
    i=1
    while [ "$i" -le "$n" ]; do
        until synced $((PORT + i)); do sleep 0.1; done
        i=$((i + 1))
    done
}

gap() {
    gap_mb=$1 n=${2:-1}
    setup "$n"
    for p in $replica_pids; do kill -STOP "$p"; done
    cli "$PORT" client kill type replica >/dev/null
    off0=$(info "$PORT" replication master_repl_offset)
    "$VALKEY_SRC/valkey-benchmark" -p "$PORT" -q -t set -d "$VALUE" -r 1000000 \
        -n $((gap_mb * 1024 * 1024 / VALUE)) >/dev/null
    gap_bytes=$(($(info "$PORT" replication master_repl_offset) - off0))
    full0=$(info "$PORT" stats sync_full)
    part0=$(info "$PORT" stats sync_partial_ok)
    out0=$(info "$PORT" stats total_net_repl_output_bytes)
    disk0=$(info "$PORT" replication catchup_disk_bytes_read)
    t0=$(now_ms)
    for p in $replica_pids; do kill -CONT "$p"; done
    i=1
    while [ "$i" -le "$n" ]; do
        until synced $((PORT + i)); do sleep 0.02; done
        i=$((i + 1))
    done
    t1=$(now_ms)
    disk1=$(info "$PORT" replication catchup_disk_bytes_read)
    echo "gap_bytes,replicas,backlog,dataset_mb,full_syncs,partial_syncs,repl_out_bytes,disk_bytes_read,recovery_ms"
    echo "$gap_bytes,$n,$BACKLOG,$DATASET_MB,$(($(info "$PORT" stats sync_full) - full0)),$(($(info "$PORT" stats sync_partial_ok) - part0)),$(($(info "$PORT" stats total_net_repl_output_bytes) - out0)),${disk1:+$((disk1 - ${disk0:-0}))},$((t1 - t0))"
}

steady() {
    requests=${1:-1000000}
    setup "${REPLICAS:-2}"
    echo "used_memory=$(info "$PORT" memory used_memory) repl_backlog_size=$(info "$PORT" replication repl_backlog_size)"
    "$VALKEY_SRC/valkey-benchmark" -p "$PORT" -t set -d "$VALUE" -r 1000000 -n "$requests" -c 50 --csv
    echo "suffix_bytes_read=$(info "$PORT" replication catchup_disk_bytes_read)"
}

cmd=${1:?usage: bench.sh gap <gap-mb> [replicas] | steady [requests]}
shift
"$cmd" "$@"
