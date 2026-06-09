#!/bin/bash
# run_bench.sh — runs both baseline and optimized benchmarks for all 3 scenarios
# Must run as root.  Usage: sudo ./run_bench.sh

set -euo pipefail

SCRIPT_DIR=$(dirname "$(realpath "$0")")
RESULTS_DIR="$SCRIPT_DIR/results"

if [ -x "/usr/local/sbin/zpressd" ]; then
    DAEMON="/usr/local/sbin/zpressd"
else
    DAEMON="$SCRIPT_DIR/../bin/zpressd"
fi

export DURATION=120  # seconds per scenario

mkdir -p "$RESULTS_DIR"

run_scenario() {
    local NAME=$1
    local WORKLOAD=$2
    local MODE=$3    # baseline or optimized

    local SUFFIX="baseline"
    if [ "$MODE" = "optimized" ]; then
        SUFFIX="daemon"
    fi
    local OUTFILE="$RESULTS_DIR/${NAME}_${SUFFIX}.csv"

    echo "=== Scenario: $NAME ($MODE) ==="

    # Drop caches
    sync; echo 3 > /proc/sys/vm/drop_caches
    sleep 3

    # Start metrics collector
    bash "$SCRIPT_DIR/collect_metrics.sh" "$OUTFILE" &
    COLLECTOR_PID=$!

    # Start daemon if optimized mode
    if [ "$MODE" = "optimized" ]; then
        $DAEMON -f &
        DAEMON_PID=$!
        sleep 2
    fi

    # Run workload
    bash "$SCRIPT_DIR/workloads/${WORKLOAD}.sh" &
    WORKLOAD_PID=$!

    # Measure interactive latency baseline (time a simple command)
    sleep 10
    echo "--- Interactive latency test ($MODE) ---"
    for i in 1 2 3 4 5; do
        { time ls -la /usr/bin > /dev/null; } 2>&1 | grep real
    done

    sleep $DURATION

    # Cleanup
    kill $WORKLOAD_PID 2>/dev/null || true
    kill $COLLECTOR_PID 2>/dev/null || true
    if [ "$MODE" = "optimized" ]; then
        kill $DAEMON_PID 2>/dev/null || true
    fi
    wait 2>/dev/null || true
    echo "Scenario $NAME ($MODE) done → $OUTFILE"
}

# Run all scenarios both ways
for SCENARIO in light medium heavy; do
    run_scenario "$SCENARIO" "$SCENARIO" "baseline"
    sleep 10   # cooldown between runs
    run_scenario "$SCENARIO" "$SCENARIO" "optimized"
    sleep 10
done

echo '=== All benchmarks done. Run plot_results.py to visualize ==='

