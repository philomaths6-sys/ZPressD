#include "pressure.h"
#include "proc_utils.h"
#include "logger.h"
#include <string.h>
#include <time.h>


/* Previous vmstat sample for computation */
static uint64_t s_prev_pswpin = 0;
static uint64_t s_prev_pswpout = 0;
static uint64_t s_prev_majflt = 0;
static struct timespec s_prev_ts = {0, 0};


int pressure_sample(PressureState *ps) {
    if(!ps) return -1;
    memset(ps, 0, sizeof(*ps));
    clock_gettime(CLOCK_MONOTONIC, &ps->sampled_at);

    /* /proc/meminfo */
    ps->mem_total_kb = proc_meminfo_get_kb("MemTotal");
    ps->mem_available_kb = proc_meminfo_get_kb("MemAvailable");
    ps->swap_total_kb = proc_meminfo_get_kb("SwapTotal");
    ps->swap_free_kb = proc_meminfo_get_kb("SwapFree");
    ps->swap_cached_kb = proc_meminfo_get_kb("SwapCached");
    ps->dirty_kb = proc_meminfo_get_kb("Dirty");

    /* Derived */
    if(ps->mem_total_kb > 0) {
        ps->mem_avail_pct = 100.0 * ps->mem_available_kb / ps->mem_total_kb;
    }
    if(ps->swap_total_kb > 0) {
        ps->swap_used_pct = 100.0 * (ps->swap_total_kb - ps->swap_free_kb) / ps->swap_total_kb;
    }

    /* /proc/vmstat deltas */
    uint64_t cur_pswpin = proc_vmstat_get("pswpin");
    uint64_t cur_pswpout = proc_vmstat_get("pswpout");
    uint64_t cur_majflt = proc_vmstat_get("pgmajfault");

    if(s_prev_ts.tv_sec > 0) {
        double dt = (ps->sampled_at.tv_sec - s_prev_ts.tv_sec) + 
                    (ps->sampled_at.tv_nsec - s_prev_ts.tv_nsec) * 1e-9;
        if(dt > 0) {
            ps->pswpin_rate = (uint64_t)((cur_pswpin - s_prev_pswpin) / dt);
            ps->pswpout_rate = (uint64_t)((cur_pswpout - s_prev_pswpout) / dt);
            ps->pgmajfault_rate = (uint64_t)((cur_majflt - s_prev_majflt) / dt);
        }
    }
    s_prev_pswpin = cur_pswpin;
    s_prev_pswpout = cur_pswpout;
    s_prev_majflt = cur_majflt;
    s_prev_ts = ps->sampled_at;

    /* /proc/pressure/memory */
    PsiMetrics psi;
    if(proc_parse_psi("/proc/pressure/memory", &psi) == 0) {
        ps->psi_some_avg10 = psi.some_avg10;
        ps->psi_some_avg60 = psi.some_avg60;
        ps->psi_some_avg300 = psi.some_avg300;
        ps->psi_full_avg10 = psi.full_avg10;
        ps->psi_full_avg60 = psi.full_avg60;
        ps->psi_full_avg300 = psi.full_avg300;
    }

    /* zram stats */
    ZramMmStat zs;
    if (proc_read_zram_mmstat("zram0", &zs) == 0) {
        ps->zram_orig_bytes = zs.orig_data_size;
        ps->zram_compr_bytes = zs.compr_data_size;
        if(zs.compr_data_size > 0) {
            ps->zram_ratio = (double)zs.orig_data_size / zs.compr_data_size;
        }
    }
    ps->level = pressure_classify(ps);
    return 0;
}

PressureLevel pressure_classify(const PressureState *ps) {
    if(ps->psi_full_avg10 > 20.0 || ps->mem_avail_pct < 3.0) 
        return PRESSURE_CRITICAL;
    if (ps->psi_full_avg10 > 5.0 || ps->mem_avail_pct < 10.0)
        return PRESSURE_HIGH;
    if (ps->psi_some_avg10 > 20.0 || ps->pswpout_rate > 20)
        return PRESSURE_MODERATE;
    if (ps->psi_some_avg10 > 5.0 || ps->mem_avail_pct < 40.0)
        return PRESSURE_LOW;
    return PRESSURE_NONE;
}

const char *pressure_level_name(PressureLevel lv){
    static const char *names[] = {"NONE", "LOW", "MODERATE", "HIGH", "CRITICAL"};
    return (lv < 5) ? names[lv] : "UNKNOWN";
}