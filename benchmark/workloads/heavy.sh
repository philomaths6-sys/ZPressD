#!/bin/bash
# Scenario: Heavy — ZPressD vs baseline under simultaneous memory pressure
#
# What this does:
#   Spawns 3 idle background processes (CLASS_BACKGROUND — no TTY) holding 60% RAM,
#   then immediately starts stress-ng demanding another 40%. This creates ~100% RAM
#   commitment from the start.
#
# Baseline: kernel must scramble to evict background pages to disk swap reactively
#   → high pswpout_rate, high PSI stall time
#
# ZPressD: daemon detects pressure immediately, cold_score_all() picks the idle
#   python workers (zero faults, high RSS), process_madvise(MADV_PAGEOUT) compresses
#   them into zswap. Disk swap I/O drops. PSI stall drops.

echo "[Workload: Heavy] Starting..."

ALLOC_BYTES=$(awk '/MemTotal/ {printf "%d", $2 * 1024 / 5}' /proc/meminfo)

# Write python worker to temp file (avoids shell quoting issues in heredocs)
WORKER_SCRIPT=$(mktemp /tmp/zpressd_worker_XXXX.py)
cat > "$WORKER_SCRIPT" << 'PYEOF'
import sys, time, mmap
size = int(sys.argv[1])
buf = mmap.mmap(-1, size)
# Zero-filled: highly compressible — zswap gets excellent ratio on this
buf.write(b'\x00' * size)
print(f'[bg-worker] {size//1024//1024}MB allocated, idle', flush=True)
time.sleep(600)
PYEOF

# 3 background workers = 3 × 20% = 60% RAM, all idle (prime ZPressD targets)
python3 "$WORKER_SCRIPT" "$ALLOC_BYTES" &
BG1=$!
python3 "$WORKER_SCRIPT" "$ALLOC_BYTES" &
BG2=$!
python3 "$WORKER_SCRIPT" "$ALLOC_BYTES" &
BG3=$!

echo "[Workload: Heavy] Background workers: PIDs $BG1 $BG2 $BG3 (60% RAM idle)"

# Start foreground pressure immediately — no wait.
# ZPressD begins compressing the background workers as soon as it detects the pressure spike.
# Baseline has no daemon, so kernel must synchronously evict to disk swap instead.
echo "[Workload: Heavy] Starting stress-ng (40% RAM)..."
stress-ng --vm 2 --vm-bytes 20% --vm-keep \
          --timeout 0 \
          --metrics-brief &
STRESSNG=$!

cleanup() {
    kill $BG1 $BG2 $BG3 $STRESSNG 2>/dev/null || true
    pkill -9 stress-ng 2>/dev/null || true
    pkill -9 python3   2>/dev/null || true
    rm -f "$WORKER_SCRIPT"
}
trap cleanup SIGTERM SIGINT EXIT

wait $STRESSNG