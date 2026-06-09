#!/bin/bash
# run_bench.sh — ZPressD benchmark orchestrator
# Must run as root. Usage: sudo ./run_bench.sh
#
# Architecture:
#   1. Baseline run: workload only, no daemon → measures worst-case kernel behavior
#   2. Optimized run: daemon started first → ZPressD proactively compresses cold pages
#      before foreground pressure arrives
#
# Key metrics compared:
#   pswpout_rate     → disk swap I/O (ZPressD should drive this to near 0)
#   zswap_stored     → pages ZPressD pushed into compressed pool (should be high)
#   psi_full_avg10   → CPU stall time waiting on memory (ZPressD should reduce this)
#   latency_ms       → interactive command latency under pressure (should be unaffected)

set -eo pipefail

SCRIPT_DIR=$(dirname "$(realpath "$0")")
RESULTS_DIR="$SCRIPT_DIR/results"

# Find daemon binary
if [ -x "/usr/local/sbin/zpressd" ]; then
    DAEMON="/usr/local/sbin/zpressd"
elif [ -x "$SCRIPT_DIR/../bin/zpressd" ]; then
    DAEMON="$SCRIPT_DIR/../bin/zpressd"
else
    echo "ERROR: zpressd binary not found. Build with 'make' first." >&2
    exit 1
fi

# Duration for the stress phase (after background procs are established)
export DURATION=90  # seconds of stress phase data collection

mkdir -p "$RESULTS_DIR"

run_scenario() {
    local NAME=$1
    local WORKLOAD=$2
    local MODE=$3    # baseline or optimized

    local SUFFIX="baseline"
    [ "$MODE" = "optimized" ] && SUFFIX="daemon"
    local OUTFILE="$RESULTS_DIR/${NAME}_${SUFFIX}.csv"
    local LATENCY_FILE="$RESULTS_DIR/${NAME}_${SUFFIX}_latency.txt"

    echo "[bench] scenario=$NAME mode=$MODE"

    # Ensure no leftover stress/python from prior run
    pkill -9 stress-ng 2>/dev/null || true
    pkill -9 python3   2>/dev/null || true
    sleep 2

    # Drop caches for clean baseline
    sync
    echo 3 > /proc/sys/vm/drop_caches
    sleep 3

    # Record zswap writeback baseline so we measure delta, not cumulative
    ZSWAP_WB_START=0
    [ -f /sys/kernel/debug/zswap/written_back_pages ] && \
        ZSWAP_WB_START=$(cat /sys/kernel/debug/zswap/written_back_pages)

    # Start metrics collector (2s granularity, captures all ZPressD-relevant signals)
    bash "$SCRIPT_DIR/collect_metrics.sh" "$OUTFILE" &
    COLLECTOR_PID=$!

    # Start daemon BEFORE workload in optimized mode.
    # This is critical: the daemon needs to be alive during the 30s idle window
    # in heavy.sh so it can run compression cycles on the background workers.
    DAEMON_PID=""
    if [ "$MODE" = "optimized" ]; then
        $DAEMON -f &
        DAEMON_PID=$!
        echo "[bench] zpressd started (PID $DAEMON_PID)"
        sleep 2  # let daemon warm up (initial proclist_refresh + classify)
    fi

    # Start workload
    bash "$SCRIPT_DIR/workloads/${WORKLOAD}.sh" &
    WORKLOAD_PID=$!
    echo "[bench] workload started (PID $WORKLOAD_PID)"

    # heavy.sh has a 30s idle background phase before stress-ng starts.
    # medium/light start generating pressure immediately — wait 5s.
    local IDLE_WAIT=5
    [ "$NAME" = "heavy" ] && IDLE_WAIT=35
    echo "[bench] waiting ${IDLE_WAIT}s for workload to reach peak pressure"
    sleep $IDLE_WAIT

    # Measure interactive latency WHILE UNDER PRESSURE
    # This measures whether ZPressD kept interactive processes unaffected
    echo "[bench] measuring interactive latency under pressure"
    rm -f "$LATENCY_FILE"
    for i in 1 2 3 4 5 6 7 8 9 10; do
        # time a real interactive operation: find (touches VFS, similar to terminal usage)
        MS=$( { time find /usr/bin -maxdepth 1 -name 'ls' > /dev/null; } 2>&1 | \
              awk '/real/ {
                  split($2, a, "m");
                  sec = a[2]; gsub("s", "", sec);
                  printf "%.0f", a[1]*60000 + sec*1000
              }' )
        MS=${MS:-0}
        echo "$MS" >> "$LATENCY_FILE"
        echo "[bench] latency sample $i: ${MS}ms"
        sleep 2
    done

    # Collect data during stress phase
    echo "[bench] collecting stress-phase data for ${DURATION}s"
    sleep $DURATION

    # Final zswap writeback delta
    ZSWAP_WB_END=0
    [ -f /sys/kernel/debug/zswap/written_back_pages ] && \
        ZSWAP_WB_END=$(cat /sys/kernel/debug/zswap/written_back_pages)
    ZSWAP_WB_DELTA=$(( ZSWAP_WB_END - ZSWAP_WB_START ))
    echo "$ZSWAP_WB_DELTA" > "$RESULTS_DIR/${NAME}_${SUFFIX}_zswap_wb_delta.txt"
    echo "[bench] zswap writeback delta: $ZSWAP_WB_DELTA pages"

    # Cleanup
    kill -9 $WORKLOAD_PID 2>/dev/null || true
    pkill -9 stress-ng 2>/dev/null || true
    pkill -9 python3   2>/dev/null || true
    kill    $COLLECTOR_PID 2>/dev/null || true
    if [ -n "$DAEMON_PID" ]; then
        kill $DAEMON_PID 2>/dev/null || true
    fi
    wait 2>/dev/null || true

    echo "[bench] done: $OUTFILE"
}

# Run scenarios — heavy is the primary showcase, run it first
for SCENARIO in heavy medium light; do
    run_scenario "$SCENARIO" "$SCENARIO" "baseline"
    echo "[bench] cooldown 15s"
    sleep 15
    run_scenario "$SCENARIO" "$SCENARIO" "optimized"
    echo "[bench] cooldown 15s"
    sleep 15
done

echo "[bench] all scenarios complete. run: python3 benchmark/plot_results.py"
