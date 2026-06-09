#!/bin/bash
# Scenario A: firefox + vim + 1G stress

TIMEOUT=${DURATION:-120}

echo "[Workload: Light] Starting..."

# 1. Start Firefox
if command -v firefox &> /dev/null; then
    echo "Starting firefox in background..."
    if [ -n "${SUDO_USER:-}" ]; then
        sudo -u "$SUDO_USER" firefox --headless https://www.wikipedia.org &
    else
        firefox --headless https://www.wikipedia.org &
    fi
    FIREFOX_PID=$!
else
    echo "firefox not found, skipping."
fi

# 2. Start vim
if command -v vim &> /dev/null; then
    echo "Starting vim in background..."
    # Run vim without UI taking over, just reading a file
    if [ -n "${SUDO_USER:-}" ]; then
        sudo -u "$SUDO_USER" vim -u NONE -i NONE -n -c "set noswapfile" -c "e /var/log/syslog" &
    else
        vim -u NONE -i NONE -n -c "set noswapfile" -c "e /var/log/syslog" &
    fi
    VIM_PID=$!
else
    echo "vim not found, skipping."
fi

# 3. 1G stress test
echo "[Workload: Light] Running 1G memory stress test for $TIMEOUT seconds..."
if command -v stress &> /dev/null; then
    stress --vm 1 --vm-bytes 1G --timeout $TIMEOUT &
    STRESS_PID=$!
else
    echo "stress tool not found. Using python to allocate 1G memory..."
    python3 -c "a = 'a' * (1024 * 1024 * 1024); import time; time.sleep($TIMEOUT)" &
    STRESS_PID=$!
fi

# Setup cleanup on termination
cleanup() {
    echo "[Workload: Light] Cleaning up..."
    [ -n "$FIREFOX_PID" ] && kill $FIREFOX_PID 2>/dev/null || true
    [ -n "$VIM_PID" ] && kill -9 $VIM_PID 2>/dev/null || true
    [ -n "$STRESS_PID" ] && kill -9 $STRESS_PID 2>/dev/null || true
}
trap cleanup SIGTERM SIGINT EXIT

wait $STRESS_PID 2>/dev/null || true

echo "[Workload: Light] Completed."
