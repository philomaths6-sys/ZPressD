#!/bin/bash
# Scenario B: compile + 2G stress

TIMEOUT=${DURATION:-120}

echo "[Workload: Medium] Starting..."

# 1. Start a compile job
echo "[Workload: Medium] Starting background compilation..."
# We will compile the current ZPressD project to simulate compile load
if [ -f "../../Makefile" ]; then
    (cd ../../ && make clean > /dev/null && make -j$(nproc) > /dev/null) &
    MAKE_PID=$!
else
    echo "Makefile not found in root, skipping compile step."
fi

# 2. 2G stress test
echo "[Workload: Medium] Running 2G memory stress test for $TIMEOUT seconds..."
if command -v stress &> /dev/null; then
    stress --vm 2 --vm-bytes 1G --timeout $TIMEOUT &
    STRESS_PID=$!
else
    echo "stress tool not found. Using python to allocate 2G memory..."
    python3 -c "a = 'a' * (2048 * 1024 * 1024); import time; time.sleep($TIMEOUT)" &
    STRESS_PID=$!
fi

# Setup cleanup on termination
cleanup() {
    echo "[Workload: Medium] Cleaning up..."
    [ -n "$MAKE_PID" ] && kill -9 $MAKE_PID 2>/dev/null || true
    [ -n "$STRESS_PID" ] && kill -9 $STRESS_PID 2>/dev/null || true
}
trap cleanup SIGTERM SIGINT EXIT

wait $STRESS_PID 2>/dev/null || true
wait $MAKE_PID 2>/dev/null || true

echo "[Workload: Medium] Completed."
