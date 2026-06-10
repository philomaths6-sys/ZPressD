# ZPressD — System Architecture

## Overview

ZPressD is a Linux userspace daemon written in C11. It sits between the kernel's memory subsystem and the applications running on the system, acting as an intelligence layer that the kernel itself cannot provide: process-aware, priority-conscious memory pressure relief.

The daemon does not implement compression. zswap and zram handle all compression in kernel space. ZPressD's role is purely decisional — detect pressure early, identify which processes are the best candidates to compress, and instruct the kernel to move their cold pages into compressed storage via `process_madvise(MADV_PAGEOUT)`.

---

## Component Map

```
 ┌──────────────────────────────────────────────────────┐
 │                      main.c                          │
 │              Daemon entry & state machine             │
 │   IDLE → MONITORING → COMPRESSING → RECOVERY         │
 │                      ↕ EMERGENCY                     │
 └────┬──────────┬───────────┬──────────────┬───────────┘
      │          │           │              │
      ▼          ▼           ▼              ▼
mem_pressure  process.c  cold_page.c  compression_ctrl.c
   .c         process_   zswap_       proc_utils.c
              classifier tuner.c      config.c
              .c                      logger.c
```

---

## Module Responsibilities

### `main.c` — Daemon Entry and State Machine

Owns the main loop and the five-state FSM. On startup it:
1. Parses CLI flags (`-f` foreground, `-n` dry-run, `-c` config path)
2. Calls `config_load()` and `logger_init()`
3. Optionally daemonizes: `fork()` → parent exits, child calls `setsid()`, writes PID to `/var/run/zpressd.pid`, redirects stdio to `/dev/null`
4. Installs signal handlers: `SIGTERM`/`SIGINT` → set `g_running=0`; `SIGHUP` → set `g_reload_config=1`
5. Enters the main loop

The state machine drives all work. Each state controls the poll interval and what subsystems are invoked:

| State | Poll interval | Actions |
|---|---|---|
| `IDLE` | 500ms | Sample pressure only |
| `MONITORING` | 100ms | Refresh process list, classify, score |
| `COMPRESSING` | 100ms | Score, hint top-N, tune zswap |
| `RECOVERY` | 100ms | Hysteresis: 5 consecutive LOW before returning to IDLE |
| `EMERGENCY` | 100ms | top_candidates_n=20, budget=2GB, hint immediately |

Transitions are guarded: IDLE→MONITORING requires 3 consecutive pressure readings above threshold to filter transient spikes. COMPRESSING→RECOVERY requires pressure to drop below MODERATE. RECOVERY→IDLE requires 5 consecutive LOW or NONE readings.

---

### `mem_pressure.c` — PSI and meminfo Poller

Fills a `PressureState` struct each cycle by reading:
- `/proc/meminfo` — `MemTotal`, `MemAvailable`, `SwapTotal`, `SwapFree`, `Dirty`
- `/proc/vmstat` — `pswpin`, `pswpout`, `pgmajfault` (delta since last sample, divided by elapsed seconds to get rates)
- `/proc/pressure/memory` — `some avg10/avg60/avg300`, `full avg10/avg60/avg300`
- `/sys/block/zram0/mm_stat` — `orig_data_size`, `compr_data_size` for compression ratio

After filling the struct, calls `pressure_classify()` which maps the raw metrics to a `PressureLevel` enum:

```
CRITICAL  : psi_full_avg10 > 20.0  OR  mem_avail_pct < 3%
HIGH      : psi_full_avg10 > 5.0   OR  mem_avail_pct < 10%
MODERATE  : psi_some_avg10 > 20.0  OR  pswpout_rate > 20 pg/s
LOW       : psi_some_avg10 > 5.0   OR  mem_avail_pct < 40%
NONE      : all below thresholds
```

vmstat deltas are computed using wall-clock time between samples (`CLOCK_MONOTONIC`) to produce rates in pages/second.

---

### `process.c` — ProcessList Management

Maintains a `ProcessList` — a dynamic array of `ProcessInfo` structs, one per live process. `proclist_refresh()` runs each cycle in MONITORING and COMPRESSING states:

1. `proc_enum_pids()` enumerates `/proc` for numeric directory entries
2. For each PID: `proc_read_stat()` reads `/proc/[pid]/stat`, `proc_read_smaps_rollup()` reads `/proc/[pid]/smaps_rollup`
3. Computes per-cycle deltas (`minflt_delta`, `majflt_delta`) by comparing against the previous snapshot via `proclist_find_pid()`
4. Carries forward `idle_seconds` and `last_hinted_at` from the previous snapshot
5. `memcpy`s the new snapshot over the old one

The list is pre-allocated at startup (`MAX_PROCESSES = 4096`) and reallocated only if the process count exceeds capacity.

---

### `process_classifier.c` — INTERACTIVE / BACKGROUND Labeling

`classify_process()` assigns each process a `ProcessClass` using a priority-ordered rule chain:

1. PID ≤ 1 or empty comm → `CLASS_KERNEL`
2. RSS = 0 → `CLASS_KERNEL` (kthread)
3. `comm` matches built-in protected list → `CLASS_INTERACTIVE`
4. `comm` matches user-configured `protect =` entries → `CLASS_INTERACTIVE`
5. `tty_nr != 0` (has controlling terminal) → `CLASS_INTERACTIVE`
6. Default → `CLASS_BACKGROUND`

Built-in protected names: `bash`, `zsh`, `fish`, `sh`, `dash`, `vim`, `vi`, `nano`, `emacs`, `nvim`, `code`, `code-server`, `gnome-terminal`, `konsole`, `xterm`, `alacritty`, `kitty`, `wezterm`, `tmux`, `screen`, `sway`, `i3`, `zpressd`.

Only `CLASS_BACKGROUND` processes are eligible for compression hints. `CLASS_KERNEL` and `CLASS_INTERACTIVE` processes are never touched.

---

### `cold_page.c` — Coldness Scoring

`cold_score_compute()` computes a scalar score for each background process:

```
score = (1 / (fault_delta + 1)) × rss_weight × idle_weight

rss_weight  = clamp(rss_kb / (100 × 1024),  0.01, 10.0)
idle_weight = min(idle_seconds, 60) / 60.0
```

`fault_delta` is the sum of `minflt_delta + majflt_delta` since the last sample. A process with zero faults scores `1/(0+1) = 1.0` on the fault term. `rss_weight` gives larger-RSS processes a higher score, normalised to 100 MB with a cap at 10 GB. `idle_weight` ramps from 0 to 1.0 over 60 seconds of inactivity, seeded to 1 on the first cycle so no process starts with a zero score.

A 1.2× bonus is applied if `swap_kb > 0` — processes already partially evicted are better candidates for further eviction.

`cold_score_all()` scores every background process then calls `qsort()` on the full list (descending), so `cold_top_candidates()` only needs to walk from the front.

`cold_top_candidates()` enforces the per-process cooldown: if `last_hinted_at` was within `cooling_period_secs` seconds (default 30), the process is marked `cooling_off=1` and skipped.

---

### `compression_ctrl.c` — Hint Engine

`hint_process()` is the core action function. For each selected candidate:

1. Calls `proc_read_maps()` to enumerate VMA regions from `/proc/[pid]/maps`
2. Opens a `pidfd` via `syscall(__NR_pidfd_open, pid, 0)` — one fd per process, opened once and closed after all regions are processed
3. For each anonymous, writable VMA region (`is_anon=1`, `perms[1]=='w'`), clamps the region length to the remaining budget, then calls:
   ```c
   struct iovec iov = { .iov_base = (void *)r->start, .iov_len = len };
   long ret = syscall(__NR_process_madvise, pidfd, &iov, 1UL, MADV_PAGEOUT, 0UL);
   ```
4. `EINVAL` and `EPERM` on individual regions are non-fatal — the kernel rejects locked or special-purpose pages. The daemon logs a WARN and continues.
5. Accumulates the bytes reported as hinted by the kernel (`ret`), updates `last_hinted_at`, and returns the total.

`run_compression_cycle()` drives a full cycle: budget initialisation, candidate selection via `cold_top_candidates()`, and iteration with cooldown enforcement.

---

### `zswap_tuner.c` — Dynamic Compressor Tuning

Writes to `/sys/module/zswap/parameters/compressor` and `/sys/module/zswap/parameters/max_pool_percent` based on the current pressure level:

| Pressure | Compressor | Pool % | Rationale |
|---|---|---|---|
| MODERATE | `lz4` | 30% | Fast decompression; low CPU overhead |
| HIGH | `lz4hc` | 40% | Better ratio; acceptable latency |
| CRITICAL | `zstd` | 50% | Best ratio; CPU cost justified |
| LOW / NONE | `lz4` | 20% | Recovery: free pool, reduce memory overhead |

A 5-second hysteresis prevents parameter thrashing when pressure oscillates near a threshold boundary. Changes are only written if the value actually differs from the current state (read once at startup via `zswap_read_state()`).

---

### `proc_utils.c` — `/proc` Parsing Helpers

Shared parsing functions used by all modules:

- `proc_meminfo_get_kb(field)` — single field lookup in `/proc/meminfo`
- `proc_vmstat_get(field)` — single field lookup in `/proc/vmstat`
- `proc_parse_psi(path, out)` — parses both `some` and `full` lines from a PSI file
- `proc_read_stat(pid, out)` — parses `/proc/[pid]/stat`, handling process names with spaces by locating the last `)` rather than the first
- `proc_read_smaps_rollup(pid, out)` — reads `Rss`, `Pss`, `Swap`, `Private_Dirty` from `/proc/[pid]/smaps_rollup`
- `proc_read_maps(pid, &out, &count)` — reads all VMA lines, filters to writable regions (`perms[1]=='w'`), sets `is_anon=1` for anonymous mappings (no pathname, or `[heap]`/`[stack]`)
- `proc_enum_pids(pids, max)` — opens `/proc`, reads all numeric directory entries
- `proc_read_zram_mmstat(dev, out)` — reads `/sys/block/<dev>/mm_stat`

---

### `config.c` — Configuration

`config_defaults()` fills a `Config` struct with safe values. `config_load()` calls `config_defaults()` first, then opens the file and overrides keys it finds. Unrecognised keys are silently ignored. Missing config file is not an error — defaults are valid for running.

`SIGHUP` triggers a live reload: the main loop checks `g_reload_config`, calls `config_load()` again, and continues without restarting.

---

### `logger.c` — Structured Logging

`_log_write()` formats a timestamped line:
```
[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] (file:line) message
```
and writes it to either the log file (default `/var/log/zpressd.log`) or stderr, plus optionally to syslog. Log level is runtime-configurable; `DEBUG` lines have zero cost when the level is `INFO` or above (the check is the first thing in `_log_write`).

---

## Data Flow — One Compression Cycle

```
pressure_sample(&ps)
        │
        ▼
proclist_refresh(p1)          ← reads /proc for every live PID
        │
        ▼
classify_all(p1, &cfg)        ← labels each process INTERACTIVE / BACKGROUND / KERNEL
        │
        ▼
cold_score_all(p1)            ← scores background processes, qsorts list descending
        │
        ▼
zswap_tune(&zs, ps.level)     ← updates /sys/module/zswap/parameters/* if needed
        │
        ▼
run_compression_cycle(p1, &ps, &cfg, &hr)
        │
        ├── cold_top_candidates()   ← walks sorted list, enforces cooldown
        │
        └── for each candidate:
                hint_process(p, &budget, dry_run)
                        │
                        ├── proc_read_maps()          ← /proc/[pid]/maps
                        ├── pidfd_open_for(pid)        ← syscall 434
                        └── for each anon writable VMA:
                                process_madvise(pidfd, &iov, MADV_PAGEOUT)   ← syscall 440
```

---

## Threading and Concurrency

ZPressD is single-threaded. All work happens in the main loop. Signal handlers set only two `volatile int` flags (`g_running`, `g_reload_config`) and return immediately. The main loop checks these flags at the top of each iteration. There is no shared mutable state between any concurrent execution contexts.

---

## Memory Footprint

The dominant allocation is the `ProcessList`: up to 4096 `ProcessInfo` structs. `sizeof(ProcessInfo)` is approximately 200 bytes, giving a worst-case footprint of ~800 KB for process tracking. VMA region arrays are allocated and freed within each `hint_process()` call. Total daemon RSS in practice is under 3 MB.
