#include "compression_ctrl.h"
#include "cold_page.h"
#include "proc_utils.h"
#include "logger.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21   /* Linux 5.4+ */
#endif


uint64_t hint_process(ProcessInfo *proc, uint64_t *budget_remaining, int dry_run) {
    if(!proc || !budget_remaining || *budget_remaining == 0) return 0;
     VmaRegion *regions = NULL;
    int region_count = 0;
    if(proc_read_maps(proc->pid, &regions, &region_count) != 0) {
        LOG_WARN("hint_process: cannot read maps for PID %d", proc->pid);
        return 0;
    }
    uint64_t hinted = 0;
    for ( int i = 0; i < region_count && *budget_remaining > 0; i++) {
        VmaRegion *r = &regions[i];
        if (!r->is_anon) continue;   /* skip file-backed  */
        size_t len = r->end - r->start;
        if(len == 0)continue;

        /* clamp to remaining budget */
        if(len > *budget_remaining) len = *budget_remaining;

        if(dry_run) {
            LOG_INFO("[DRY-RUN] PID %d (%s) : would hint 0x%lx+%zu (%zu kB)", proc->pid, proc->comm, r->start, len, 
                len/1024);
        } else {
            void *addr = (void *)r->start;
            if(madvise(addr, len, MADV_PAGEOUT) != 0) {
                LOG_DEBUG("madvise PAGEOUT PID %d adrr %p len %zu: %s",
                    proc->pid, addr, len, strerror(errno));
                /* EINVAL/EPERM are common for protected pages - not fatal */
            } else {
                hinted += len;
                LOG_DEBUG("Hinted PID %d region 0x%lx+%zu (%zu kB)", proc->pid, r->start, len, len/1024);
            }
        }
        *budget_remaining -= (len < *budget_remaining) ? len : *budget_remaining;
    }
    free(regions);
    if(hinted > 0 || dry_run) {
        clock_gettime(CLOCK_MONOTONIC, &proc->last_hinted_at);
        LOG_INFO("Hinted PID %d (%s): %.1f MB total", proc->pid, proc->comm, (double) hinted / (1024 * 1024));
    }
    return hinted;
}

int run_compression_cycle(ProcessList *p1, const PressureState *ps, const Config *cfg, HintResult *result) {
    memset(result, 0, sizeof(*result));
    uint64_t budget = cfg->madvise_budget_mb * 1024 * 1024;

    /* Get Top-N candidates */
    ProcessInfo *candidates[32];
    int n = cold_top_candidates(p1,cfg->top_candidates_n, candidates);

    if (n==0) {
        LOG_INFO("Compression cycle: no eligible candidates");
        return 0;
    }

    LOG_INFO("Compression cycle: level=%s, %d candidates, budget=%lu MB",pressure_level_name(ps->level), n, 
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
    LOG_INFO("Cycle done: hinted %d processes, %.1f MB total", result->processes_hinted,
             (double)result->total_hinted_bytes / (1024 * 1024));
    return 0;
}
