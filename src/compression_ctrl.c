#include "compression_ctrl.h"
#include "cold_page.h"
#include "proc_utils.h"
#include "logger.h"
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21        /* Linux 5.4+ */
#endif

#ifndef __NR_process_madvise
#define __NR_process_madvise 440    /* Linux 5.10+, x86_64 */
#endif

#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif

#define MAX_CANDIDATES 32

/* Open a pidfd for the given PID. Returns fd >= 0 on success, -1 on error. */
static int pidfd_open_for(pid_t pid) {
    int fd = (int)syscall(__NR_pidfd_open, pid, 0);
    return fd;
}

/*
 * hint_process: use process_madvise(2) (SYS 440) to MADV_PAGEOUT pages
 * in the TARGET process's address space.
 *
 * madvise(2) only works on the CALLING process's own VAS — passing
 * addresses from /proc/<pid>/maps to madvise() in the daemon always
 * fails with ENOMEM because those addresses are not mapped here.
 * process_madvise(2) accepts a pidfd and an iovec of address ranges
 * from the target process's VAS — this is the correct API.
 */
uint64_t hint_process(ProcessInfo *proc, uint64_t *budget_remaining, int dry_run) {
    if(!proc || !budget_remaining || *budget_remaining == 0) return 0;

    VmaRegion *regions = NULL;
    int region_count = 0;
    if(proc_read_maps(proc->pid, &regions, &region_count) != 0) {
        ZP_WARN("hint_process: cannot read maps for PID %d", proc->pid);
        return 0;
    }

    uint64_t hinted = 0;

    /* Open pidfd once for this process */
    int pidfd = -1;
    if (!dry_run) {
        pidfd = pidfd_open_for(proc->pid);
        if (pidfd < 0) {
            ZP_WARN("hint_process: pidfd_open PID %d (%s) failed: %s", proc->pid, proc->comm, strerror(errno));
            free(regions);
            return 0;
        }
    }

    /* Build iovec of anonymous writable regions, batch-submit via process_madvise */
    int anon_regions = 0;
    for (int i = 0; i < region_count && *budget_remaining > 0; i++) {
        VmaRegion *r = &regions[i];
        if (!r->is_anon) continue;   /* skip file-backed */
        size_t len = r->end - r->start;
        if (len == 0) continue;
        anon_regions++;

        /* clamp to remaining budget */
        if (len > *budget_remaining) len = *budget_remaining;

        if (dry_run) {
            ZP_INFO("[DRY-RUN] PID %d (%s) : would hint 0x%lx+%zu (%zu kB)",
                    proc->pid, proc->comm, r->start, len, len / 1024);
        } else {
            struct iovec iov = { .iov_base = (void *)r->start, .iov_len = len };
            long ret = syscall(__NR_process_madvise, pidfd, &iov, 1UL,
                               MADV_PAGEOUT, 0UL);
            if (ret < 0) {
                ZP_WARN("process_madvise PAGEOUT PID %d (%s) addr 0x%lx len %zu: %s",
                         proc->pid, proc->comm, r->start, len, strerror(errno));
                /* EPERM/EINVAL for protected pages – not fatal, continue */
            } else {
                hinted += ret;
                ZP_DEBUG("process_madvise hinted PID %d 0x%lx+%zu (%zu kB)",
                         proc->pid, r->start, (size_t)ret, (size_t)ret / 1024);
            }
        }
        *budget_remaining -= (len < *budget_remaining) ? len : *budget_remaining;
    }
    if (!dry_run && anon_regions == 0) {
        ZP_WARN("hint_process: PID %d (%s) has no anonymous regions in %d mapped regions",
                proc->pid, proc->comm, region_count);
    }

    if (pidfd >= 0) close(pidfd);
    free(regions);

    if (hinted > 0 || dry_run) {
        clock_gettime(CLOCK_MONOTONIC, &proc->last_hinted_at);
        ZP_INFO("Hinted PID %d (%s): %.1f MB total",
                proc->pid, proc->comm, (double)hinted / (1024 * 1024));
    }
    return hinted;
}

int run_compression_cycle(ProcessList *p1, const PressureState *ps, const Config *cfg, HintResult *result) {
    memset(result, 0, sizeof(*result));
    uint64_t budget = cfg->madvise_budget_mb * 1024 * 1024;

    /* Get Top-N candidates */
    size_t top_n = cfg->top_candidates_n;
    if (top_n > MAX_CANDIDATES) {
        ZP_WARN("top_candidates_n %d exceeds max %d, clamping", (int)top_n, MAX_CANDIDATES);
        top_n = MAX_CANDIDATES;
    }

    ProcessInfo *candidates[MAX_CANDIDATES];
    int n = cold_top_candidates(p1, top_n, candidates, cfg);

    if (n==0) {
        // ZP_INFO("Compression cycle: no eligible candidates");
        ZP_DEBUG("Compression cycle: no eligible candidates");
        return 0;
    }

    ZP_INFO("Compression cycle: level=%s, %d candidates, budget=%lu MB",pressure_level_name(ps->level), n, 
            cfg->madvise_budget_mb);

    for( int i=0; i<n; i++){
        ProcessInfo *p = candidates[i];
        if(p->cooling_off) { result->skipped_cooling++; continue; }
        uint64_t hinted = hint_process(p, &budget, cfg->dry_run);
        if(hinted > 0 || cfg->dry_run) {
            result->total_hinted_bytes += hinted;
            result->processes_hinted++;
        }  
    }
    ZP_INFO("Cycle done: hinted %d processes, %.1f MB total", result->processes_hinted,
             (double)result->total_hinted_bytes / (1024 * 1024));
    return 0;
}