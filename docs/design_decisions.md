# ZPressD — Design Decisions

This document explains the non-obvious choices made in ZPressD's implementation, why alternatives were considered and rejected, and where trade-offs were accepted.

---

## `process_madvise(2)` instead of `madvise(2)`

**Decision:** Use `syscall(__NR_process_madvise, pidfd, &iov, 1, MADV_PAGEOUT, 0)` rather than `madvise(addr, len, MADV_PAGEOUT)`.

**Why:** `madvise(2)` only operates on the calling process's own virtual address space. A daemon reading addresses from `/proc/<pid>/maps` and passing them directly to `madvise()` always fails with `ENOMEM` — those virtual addresses are not mapped in the daemon's VAS. This is a common mistake when implementing cross-process memory management.

`process_madvise(2)` (Linux 5.10+, syscall 440) was introduced specifically to solve this. It accepts a `pidfd` identifying the target process and an `iovec` of address ranges from *that process's* VAS. The kernel validates and applies the hint in the target's address space. This is the correct, documented API.

**Alternative considered:** Parsing the target process's `/proc/[pid]/maps`, re-mapping the pages into the daemon's own address space with `ptrace(PTRACE_PEEKDATA)` or a `/proc/[pid]/mem` read, and then calling `madvise()`. This was rejected as needlessly complex, fragile, and requiring `PTRACE_ATTACH` with its associated latency and signal interruptions.

---

## `pidfd` via `pidfd_open(2)` for stable process references

**Decision:** Open a `pidfd` for each target process before iterating its VMA regions.

**Why:** PIDs are reused on Linux. Between reading `/proc/<pid>/maps` and calling `process_madvise`, the process could exit and its PID could be reassigned to an unrelated process. A `pidfd` is a file descriptor that refers to a specific process instance, not just a PID number. If the original process exits, the `pidfd` becomes invalid and subsequent `process_madvise` calls fail cleanly with `ESRCH` rather than silently operating on the wrong process.

The `pidfd` is opened once per `hint_process()` call and closed before the function returns, keeping the fd count bounded regardless of how many candidates are processed.

---

## PSI as the primary pressure signal

**Decision:** Use `/proc/pressure/memory` (PSI — Pressure Stall Information) as the primary trigger, with `/proc/meminfo` as a secondary threshold.

**Why:** Traditional memory monitoring uses "free memory" thresholds. Free memory is a poor signal on Linux because the kernel aggressively uses available RAM for page cache — a system with 100 MB of "free" memory but 2 GB of droppable cache is not in trouble. `MemAvailable` is better but still does not capture whether processes are actually stalling.

PSI measures the fraction of time processes spent waiting for memory. `some.avg10` captures when *any* process was stalled; `full.avg10` captures when *all* runnable processes were stalled. This directly reflects user-visible impact. A high `full.avg10` means the system was frozen from the application's perspective, which is the condition ZPressD is designed to prevent.

PSI was merged in Linux 4.20 and is present on any modern kernel with `CONFIG_PSI=y`. The daemon checks for `/proc/pressure/memory` at startup and warns if it is absent.

**Alternative considered:** Using `/proc/vmstat`'s `pswpout` rate alone. This was kept as a secondary threshold (swap-out rate > 20 pages/second triggers MODERATE) but is insufficient as a primary signal because it measures an already-occurring event rather than predicting one.

---

## Five-state machine with hysteresis guards

**Decision:** IDLE → MONITORING → COMPRESSING → RECOVERY → EMERGENCY, with consecutive-reading guards on transitions.

**Why:** A simple threshold trigger (compress when PSI > X, stop when PSI < X) creates oscillation. If compression itself momentarily reduces pressure below the threshold, the daemon stops, pressure rises, the daemon starts again — thrashing in and out of the compressing state every few seconds.

The guards address this:
- **IDLE→MONITORING** requires 3 consecutive readings above the LOW threshold, filtering transient spikes from a single heavy allocation.
- **RECOVERY→IDLE** requires 5 consecutive LOW or NONE readings, ensuring pressure has genuinely subsided before the daemon backs off and reverts zswap parameters.

The 30-second per-process cooldown in `cold_top_candidates()` prevents the same process from being hinted repeatedly within a cycle. Without it, a large idle process would be re-hinted on every compression cycle, potentially triggering excessive page-in/page-out churn if the process becomes active again.

---

## Cold score formula design

**Decision:**
```
score = (1 / (fault_delta + 1)) × rss_weight × idle_weight
```

**Why each term:**

`1 / (fault_delta + 1)` — Page faults are the most direct signal of whether a process is actively using its memory. A process with zero faults in the last sampling interval is a strong compression candidate. Adding 1 to the denominator prevents division by zero and ensures active processes (fault_delta > 0) score strictly below idle ones.

`rss_weight = clamp(rss_kb / (100 × 1024), 0.01, 10.0)` — Compressing a 10 MB process yields negligible swap pressure relief. Larger processes are more valuable targets. The normalisation to 100 MB keeps the weight in a sensible range; the 0.01 floor prevents tiny processes from scoring exactly zero (which would make them indistinguishable from kernel threads, which are explicitly excluded).

`idle_weight = min(idle_seconds, 60) / 60.0` — A process that has been idle for one full minute is a better target than one that went idle two seconds ago. The 60-second cap prevents excessively idle processes from dominating the sort by an unbounded margin; after 60 seconds of inactivity, marginal additional idle time is not informative.

**The 1.2× swap bonus:** A process already partially in swap is more likely to tolerate further eviction. It has already demonstrated that it does not fault its swapped pages back quickly. This is a lightweight signal that avoids the cost of reading `/proc/[pid]/smaps_rollup` per-VMA.

**Alternative scoring approaches considered:**
- Using `/proc/[pid]/smaps`'s per-VMA `Referenced` bit to count recently accessed pages. Rejected because reading full `smaps` for all background processes is expensive (one file open per process, kilobytes of text per process).
- Using CPU time delta (`utime` + `stime` delta) as an activity proxy. Rejected because CPU-intensive processes may not be touching their memory (they could be running tight computation loops on stack-resident data), while memory-idle processes may still consume significant CPU. Page faults are a more direct signal.

---

## Anonymous-only VMA filtering

**Decision:** Only hint VMA regions with `is_anon=1` (no pathname, or `[heap]`/`[stack]`).

**Why:** File-backed mappings are already managed by the page cache. The kernel can evict clean file-backed pages on its own without any hint; they can be reloaded from the file on demand with no swap overhead. Sending `MADV_PAGEOUT` on file-backed regions is at best redundant and at worst could cause unnecessary re-reads from slower storage.

Anonymous mappings (heap, stack, `mmap(MAP_ANONYMOUS)`) are where process-private working sets live, and these are the pages that would otherwise go to disk swap. These are the correct compression targets.

Additionally, only writable VMA regions (`perms[1] == 'w'`) are selected. Read-only anonymous mappings (unusual but possible) are not productive compression targets because they typically contain initialisation data that is never modified and would have zero `Dirty` pages.

---

## Single-threaded design

**Decision:** ZPressD is entirely single-threaded. No worker threads, no thread pool.

**Why:** The workload is I/O-bound (reading `/proc` files) and low-frequency (100ms poll interval). The critical path — `proclist_refresh()` + `cold_score_all()` + one compression cycle — completes in well under 10ms on a system with a few hundred processes. There is no computation that would benefit from parallelism at this timescale.

A multi-threaded design would introduce lock contention on the `ProcessList`, complexity in signal handling (signals are delivered to an arbitrary thread unless masked), and harder-to-audit behaviour. The simplicity of single-threaded execution is a deliberate choice that makes the daemon easier to reason about and less likely to introduce latency spikes of its own.

---

## Configuration hot-reload via `SIGHUP`

**Decision:** `SIGHUP` triggers a live config reload without restarting the daemon.

**Why:** This is the Unix convention for daemon reconfiguration (nginx, syslog, etc.). It allows an operator to adjust thresholds, budget, or protected process names without losing the daemon's accumulated state (idle_seconds, last_hinted_at timestamps, cooldown tracking). A restart would reset all per-process cooldowns and force the daemon back to IDLE, causing a gap in coverage.

The signal handler sets only `volatile int g_reload_config = 1`. The actual `config_load()` call happens in the main loop body, not in the signal handler, which is correct: signal handlers must not call `stdio` functions (like `fopen`) directly.

---

## `EINVAL` / `EPERM` on individual VMA regions are non-fatal

**Decision:** `process_madvise` failures on individual regions are logged at WARN level and skipped; the function continues to the next region.

**Why:** Not every anonymous writable mapping is a valid `MADV_PAGEOUT` target. Locked pages (`mlock`), special kernel-managed regions, and certain vDSO-adjacent mappings return `EINVAL` or `EPERM`. gnome-keyring, for example, deliberately locks its key material in RAM. The kernel is the authority on which pages can be evicted; the daemon's job is to suggest, not to insist.

Treating these as fatal errors would cause the daemon to skip the entire process when only a small number of its regions are protected, losing the benefit of hinting the rest. Per-region errors are the expected case on any system with security-conscious applications.

---

## No implementation of compression

**Decision:** ZPressD never compresses pages itself. It only issues hints via `process_madvise`.

**Why:** zswap and zram are mature, kernel-space compression subsystems with hardware-accelerated codecs (`lz4`, `zstd`), per-CPU compression pools, and tight integration with the page reclaim path. Re-implementing any of this in userspace would be slower, less safe, and require invasive kernel interfaces. The daemon's value is entirely in the decision-making layer — which pages, when, in what priority order — not in the mechanics of compression.

This also means ZPressD is safe by design: it cannot corrupt process memory. A faulty madvise hint at worst causes a page-in on next access; it does not modify the data.
