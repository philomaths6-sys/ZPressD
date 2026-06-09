#!/bin/bash
# Scenario: Heavy — The canonical ZPressD demonstration workload
#
# What this simulates:
#   A system with several idle background daemons/services consuming large amounts
#   of RAM (like chrome, electron, java services). Then a new foreground task
#   demands memory, causing pressure.
#
# What ZPressD does:
#   In the 30s idle window, ZPressD's cold_score_all() identifies the python
#   background processes (no TTY, idle → high cold score), calls
#   process_madvise(MADV_PAGEOUT) on their anonymous pages, pushing them
#   into zswap compressed pool. When stress-ng fires, the RAM is already freed.
#
# BASELINE result: kernel scrambles to evict pages synchronously to disk swap
#   → high pswpout_rate, high PSI full stall, degraded interactive latency
# ZPRESSD result: background pages already in zswap → stress-ng gets memory
#   from freed RAM, pswpout_rate stays low, PSI stays near 0

echo "[Workload: Heavy] Starting idle background processes (cold target for ZPressD)..."

# Spawn 3 idle background workers — no TTY, so ZPressD classifies them as CLASS_BACKGROUND.
# They allocate compressible zero-filled memory (best case for zswap, realistic for idle daemons).
# Total allocation: ~60% of system RAM split across 3 processes.
# Use awk for the division to avoid bash integer overflow on large RAM systems
ALLOC_BYTES=$(awk '/MemTotal/ {printf "%d", $2 * 1024 / 5}' /proc/meminfo)

# Write the python worker script to a temp file to avoid quoting nightmares
WORKER_SCRIPT=$(mktemp /tmp/zpressd_worker_XXXX.py)
cat > "$WORKER_SCRIPT" << 'PYEOF'
import sys, time, mmap
size = int(sys.argv[1])
buf = mmap.mmap(-1, size)
# Write zeros — highly compressible, realistic for idle daemon heap
buf.write(b'\x00' * size)
print(f'[bg-worker] allocated {size//1024//1024}MB, now idle', flush=True)
time.sleep(600)
PYEOF

python3 "$WORKER_SCRIPT" "$ALLOC_BYTES" &
BG1=$!
python3 "$WORKER_SCRIPT" "$ALLOC_BYTES" &
BG2=$!
python3 "$WORKER_SCRIPT" "$ALLOC_BYTES" &
BG3=$!

echo "[Workload: Heavy] Background workers running (PIDs: $BG1 $BG2 $BG3)."
echo "[Workload: Heavy] Sleeping 30s — ZPressD should compress these pages during this window..."
# This 30-second window is where ZPressD acts:
# cooling_period_secs=30, poll_interval_active=100ms → multiple compression cycles happen here
sleep 30

echo "[Workload: Heavy] Launching foreground memory pressure (stress-ng)..."
# Now demand 80% of RAM from the foreground.
# Total committed = 60% (background) + 80% (stress-ng) = 140% of RAM.
# Baseline: kernel must synchronously evict → disk swap spike + PSI spike.
# ZPressD: background already in zswap → stress-ng gets physical RAM → no disk spike.
stress-ng --vm 2 --vm-bytes 40% --vm-keep \
          --timeout 0 \
          --metrics-brief &
STRESSNG=$!

# Cleanup temp script and processes on exit
cleanup() {
    kill $BG1 $BG2 $BG3 $STRESSNG 2>/dev/null || true
    pkill -9 stress-ng 2>/dev/null || true
    pkill -9 python3   2>/dev/null || true
    rm -f "$WORKER_SCRIPT"
}
trap cleanup SIGTERM SIGINT EXIT

wait $STRESSNG