# ZPressD — Adaptive Memory Compression Daemon

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Kernel](https://img.shields.io/badge/Linux%20kernel-%E2%89%A55.10-orange.svg)]()
[![Language](https://img.shields.io/badge/language-C11-lightgrey.svg)]()

> A Linux userspace daemon written in pure C that adds **process-aware intelligence** on top of the kernel's existing zswap/zram infrastructure — detecting memory pressure via PSI, identifying cold background processes by page-fault analysis, and issuing `process_madvise(MADV_PAGEOUT)` hints to move their cold working sets into compressed RAM, without ever touching interactive processes.

---

## The Problem

On a loaded Linux desktop or server, the kernel's memory reclaim is blind to process importance. It has no concept of "this process is actively serving a user" vs "this daemon has been idle for 10 minutes." The result is predictable and painful:

- A background print spooler or Bluetooth daemon sits fully resident in RAM
- Meanwhile, your active browser or IDE gets its pages pushed to disk swap
- Disk swap I/O spikes. Latency jumps. The system feels sluggish or freezes entirely.

The in-kernel OOM killer makes this worse — it only intervenes at the very last moment, after the kernel has already exhausted every other option: swapping out the desktop environment, dropping the entire page cache, emptying every buffer. By that point, the system is already unresponsive.

Existing userspace solutions like [earlyoom](https://github.com/rfjakob/earlyoom) solve a related but different problem: they kill processes when memory is critically low. ZPressD takes a softer, earlier approach — **compress cold memory before it becomes a crisis**, without killing anything.

The gap no existing tool fills:

| Tool | Approach | Limitation |
|---|---|---|
| Kernel OOM killer | Kills processes when memory is exhausted | Too late; system already unresponsive |
| earlyoom / nohang | Kills largest process at a configurable threshold | Still reactive; kills rather than compresses |
| zswap / zram | Compresses pages transparently in-kernel | Blind to process importance; no prioritization |
| **ZPressD** | Proactively compresses *cold background* processes | Preserves interactive responsiveness |

---

## What ZPressD Does

ZPressD runs as a background daemon and continuously monitors system memory health. When it detects rising pressure, it:

1. **Reads PSI** (`/proc/pressure/memory`) to detect memory stall before it becomes critical
2. **Scores every background process** for "coldness" — how idle it is, how much RAM it holds, how long since it last faulted pages
3. **Skips interactive processes** — shells, editors, terminals, compositors, and any user-configured names are never touched
4. **Issues `process_madvise(MADV_PAGEOUT)`** on the anonymous VMA regions of the coldest candidates, telling the kernel to move those pages into zswap/zram compressed storage
5. **Tunes the zswap compressor** dynamically — switching from `lz4` (fast) to `lz4hc` to `zstd` (best ratio) as pressure escalates
6. **Backs off automatically** — a 30-second per-process cooldown prevents thrashing; the daemon returns to idle once pressure clears

The daemon never implements compression itself. zswap/zram handle all compression in kernel space. ZPressD's value is entirely in **which pages to compress, when, and in what order**.

---

## Architecture

```
 /proc/pressure/memory        /proc/[pid]/stat
 /proc/meminfo                /proc/[pid]/smaps_rollup
 /proc/vmstat                 /proc/[pid]/maps
        │                              │
  mem_pressure.c           cold_page.c + process_classifier.c
  (PressureState)          (ColdScore + ProcessClass)
        │                              │
        └──────────── main.c ──────────┘
                    (State Machine)
                          │
         compression_ctrl.c + zswap_tuner.c
         process_madvise(MADV_PAGEOUT)    /sys/module/zswap/
```

### Components

| Component | Responsibility |
|---|---|
| `mem_pressure.c` | Polls `/proc/meminfo`, `/proc/vmstat`, `/proc/pressure/memory` every 100–500ms. Emits `PressureLevel` enum: NONE / LOW / MODERATE / HIGH / CRITICAL. |
| `cold_page.c` | Tracks per-process page-fault deltas. Scores coldness: `(1/(fault_delta+1)) × rss_weight × idle_weight`. Sorts candidates descending. |
| `process_classifier.c` | Labels each process `INTERACTIVE` or `BACKGROUND` using `tty_nr`, `comm` name against a protected list, and voluntary context-switch rate. |
| `compression_ctrl.c` | Opens `pidfd` via `pidfd_open(2)`, reads VMA layout from `/proc/[pid]/maps`, calls `process_madvise(MADV_PAGEOUT)` on anonymous writable regions with budget enforcement. |
| `zswap_tuner.c` | Writes compressor and `max_pool_percent` to `/sys/module/zswap/parameters/` with 5-second hysteresis to avoid thrashing. |
| `proc_utils.c` | Shared `/proc` parsing helpers used by all modules. |
| `config.c` | Loads `/etc/zpressd.conf`; all keys have safe defaults so the file is optional. |

### State Machine

```
  ┌─────────────────────────────────────────────────┐
  │  STATE_IDLE                                      │
  │  Poll every 500ms. No action.                    │
  │  → MONITORING after 3 consecutive PSI some > 5%  │
  └────────────────────┬────────────────────────────┘
                       │
  ┌────────────────────▼────────────────────────────┐
  │  STATE_MONITORING                                │
  │  Poll every 100ms. Refresh + score processes.    │
  │  → COMPRESSING when PSI some.avg10 > 20%         │
  │  → IDLE if pressure fully clears                 │
  └────────────────────┬────────────────────────────┘
                       │
  ┌────────────────────▼────────────────────────────┐
  │  STATE_COMPRESSING                               │
  │  Hint top-N cold background processes.           │
  │  Budget: 500 MB/cycle. Cooldown: 30s/process.    │
  │  Tune zswap compressor to match pressure level.  │
  │  → RECOVERY when level drops below MODERATE      │
  │  → EMERGENCY when PSI full.avg10 > 20%           │
  └────────────────────┬────────────────────────────┘
                       │
  ┌────────────────────▼────────────────────────────┐
  │  STATE_RECOVERY                                  │
  │  Require 5 consecutive LOW readings.             │
  │  Revert zswap to lz4 / pool 20%.                 │
  │  → IDLE after hysteresis satisfied               │
  │  → COMPRESSING if pressure climbs again          │
  └────────────────────┬────────────────────────────┘
                       │
  ┌────────────────────▼────────────────────────────┐
  │  STATE_EMERGENCY                                 │
  │  top_candidates_n = 20. Budget = 2 GB.           │
  │  Hint all eligible background processes now.     │
  │  → RECOVERY when level drops below CRITICAL      │
  └─────────────────────────────────────────────────┘
```

---

## Key Design Decisions

### `process_madvise(2)` — not `madvise(2)`

The daemon calls `syscall(__NR_process_madvise, pidfd, &iov, 1, MADV_PAGEOUT, 0)` rather than `madvise()`. This distinction matters:

Plain `madvise(2)` only operates on the **calling process's own** virtual address space. Passing addresses read from `/proc/<pid>/maps` into `madvise()` inside the daemon always fails with `ENOMEM` — those virtual addresses are not mapped in the daemon's VAS. `process_madvise(2)` (Linux 5.10+, syscall 440) accepts a `pidfd` and an iovec of addresses from the **target process's** VAS. This is the correct, intended API for cross-process memory hints.

### Cold score formula

```
score = (1 / (fault_delta + 1)) × rss_weight × idle_weight

rss_weight  = clamp(rss_kb / (100 × 1024),  0.01, 10.0)
idle_weight = min(idle_seconds, 60) / 60.0
```

A process with zero page faults, large RSS, and long idle time scores high — a strong compression target. An active process with frequent faults scores near zero. A 1.2× bonus applies if the process is already partially in swap.

### Protected process list

Built-in protected names: `bash`, `zsh`, `vim`, `nano`, `emacs`, `code`, `gnome-terminal`, `konsole`, `xterm`, `alacritty`, `tmux`, `sway`, `i3`, `zpressd`. Additional names are configurable via `protect =` in `zpressd.conf`. A process with `tty_nr != 0` (controlling terminal) is always marked INTERACTIVE regardless of name.

---

## Prerequisites

| Requirement | How to satisfy |
|---|---|
| Linux kernel ≥ 5.10 | `process_madvise(2)` (syscall 440). `MADV_PAGEOUT` requires ≥ 5.4. |
| PSI enabled | `cat /proc/pressure/memory` must return two lines. Needs `CONFIG_PSI=y`. |
| GCC + build tools | `sudo apt install build-essential` |
| zswap at boot | Add `zswap.enabled=1 zswap.compressor=lz4` to `GRUB_CMDLINE_LINUX` in `/etc/default/grub`, then `sudo update-grub && reboot` |
| zram (recommended) | `sudo apt install zram-config` |
| stress-ng (benchmarks only) | `sudo apt install stress-ng sysstat` |
| Python + matplotlib (graphs only) | `pip3 install matplotlib` |

Verify PSI is available:
```bash
cat /proc/pressure/memory
# Must show: some avg10=... and full avg10=...
```

---

## Build

```bash
git clone https://github.com/yourusername/zpressd
cd zpressd
make
bin/zpressd --version    # → zpressd 1.0.0
```

All `make` targets:

```
make            # build bin/zpressd
make install    # install binary + config + systemd unit
make uninstall  # remove all installed files
make test       # run unit tests (pressure classify, cold score)
make check      # cppcheck static analysis (requires cppcheck)
make fmt        # clang-format all sources (requires clang-format)
make clean      # remove build/ and bin/
```

---

## Configuration

`make install` copies `zpressd.conf.example` to `/etc/zpressd.conf` automatically. Edit it after installing:

```bash
sudoedit /etc/zpressd.conf
```

All keys are optional — missing keys use the defaults shown below.

```ini
# Polling intervals (milliseconds)
poll_idle_ms        = 500    # how often to sample when idle
poll_active_ms      = 100    # how often to sample when monitoring/compressing

# Per-cycle madvise budget
madvise_budget_mb   = 500    # max bytes hinted per compression cycle (MB)
top_candidates_n    = 5      # how many cold processes to hint per cycle

# Cooldown: don't re-hint the same process within N seconds
cooling_period_secs = 30

# zswap dynamic tuning (0 = disabled)
tune_zswap          = 1

# Dry-run: log decisions but issue no madvise calls (safe for testing)
dry_run             = 0

# Logging: 0=DEBUG 1=INFO 2=WARN 3=ERROR
log_level           = 1
log_file            = /var/log/zpressd.log

# Protect additional process names from compression (one per line)
# protect = myserver
# protect = custom-app
```

Hot-reload config without restarting the daemon:
```bash
sudo kill -HUP $(cat /var/run/zpressd.pid)
```

---

## Run

### Step 1 — Dry-run (recommended first)

No `madvise` calls are issued. The daemon logs exactly what it *would* do:

```bash
sudo bin/zpressd -f -n
```

### Step 2 — Install and enable

```bash
sudo make install
sudo systemctl enable --now zpressd
sudo journalctl -u zpressd -f
```

### CLI reference

| Flag | Meaning |
|---|---|
| `-f` | Foreground mode (don't daemonize) |
| `-n` | Dry-run (log but don't call `process_madvise`) |
| `-c <path>` | Alternate config file |
| `--version` | Print version and exit |
| `--help` | Print usage |

### Quick smoke test (60 seconds)

Open three terminals:

```bash
# Terminal 1 — daemon, foreground, dry-run
sudo bin/zpressd -f -n 2>&1 | grep -E 'IDLE|MONITORING|COMPRESSING|EMERGENCY|Hinted'

# Terminal 2 — generate memory pressure
stress-ng --vm 2 --vm-bytes 80% --timeout 60

# Terminal 3 — watch PSI in real time
watch -n1 'cat /proc/pressure/memory'
```

Expected Terminal 1 output:
```
[INFO] IDLE → MONITORING (PSI=8.42)
[INFO] MONITORING → COMPRESSING (PSI=23.17/0.00)
[INFO] Compression cycle: level=MODERATE, 5 candidates, budget=500 MB
[INFO] [DRY-RUN] PID 1842 (cups-browsed): would hint 0x7f...+134217728 (131072 kB)
[INFO] Cycle done: hinted 2 processes, 256.0 MB total
```

---

## Sample Log Output

Normal EMERGENCY cycle (from a real run):

```
[2026-06-10 03:41:32.981] [INFO]  Hinted PID 892  (cups-browsed):        17.4 MB total
[2026-06-10 03:41:32.987] [INFO]  Hinted PID 1376 (gsd-print-notif):     25.2 MB total
[2026-06-10 03:41:33.010] [INFO]  Hinted PID 1257 (gvfs-udisks2-vo):     33.9 MB total
[2026-06-10 03:41:33.016] [INFO]  Hinted PID 546  (systemd-resolve):      4.1 MB total
[2026-06-10 03:41:33.018] [INFO]  Hinted PID 826  (ModemManager):         33.6 MB total
[2026-06-10 03:41:33.024] [INFO]  Hinted PID 736  (udisksd):              42.2 MB total
[2026-06-10 03:41:33.052] [INFO]  Cycle done: hinted 20 processes, 653.7 MB total
[2026-06-10 03:41:33.052] [WARN]  EMERGENCY cycle: hinted 20 procs 653.7 MB
[2026-06-10 03:41:33.435] [INFO]  Compression cycle: level=CRITICAL, 20 candidates, budget=2048 MB
```

`EINVAL` on individual VMA regions is non-fatal and expected — the kernel rejects locked or special-purpose pages. The daemon logs a WARN and continues to the next region:

```
[2026-06-10 03:41:33.620] [WARN]  process_madvise PAGEOUT PID 972 (gnome-keyring-d)
                                   addr 0x7cf3a87f9000 len 16384: Invalid argument
[2026-06-10 03:41:33.627] [INFO]  Hinted PID 972 (gnome-keyring-d): 41.4 MB total
```

---

## Benchmark Results

For full results, raw CSVs, graphs, and methodology see [`benchmark/`](benchmark/) and [`docs/`](docs/).

---

## Requirements

- Linux kernel ≥ 5.10 (`process_madvise` syscall 440; `MADV_PAGEOUT` requires ≥ 5.4)
- `/proc/pressure/memory` present (`CONFIG_PSI=y`)
- Root, or `CAP_SYS_PTRACE` + `CAP_SYS_ADMIN`
- zswap and/or zram configured
- GCC, `-D_GNU_SOURCE` (for syscall wrappers and `O_CLOEXEC`)

---

## See Also

- [earlyoom](https://github.com/rfjakob/earlyoom) — kills the largest process when available memory drops below a threshold; complementary to ZPressD
- [nohang](https://github.com/hakavlad/nohang) — similar to earlyoom, written in Python, with additional configuration options
- [systemd-oomd](https://www.freedesktop.org/software/systemd/man/systemd-oomd.service.html) — cgroup-aware OOM killer using PSI, ships with systemd 247+
- Linux kernel PSI documentation — `Documentation/accounting/psi.rst`

