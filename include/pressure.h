#ifndef ZPRESSD_PRESSURE_H
#define ZPRESSD_PRESSURE_H


#include <stdint.h>
#include <time.h>

/* Pressure levels - derives state machine transitions */
typedef enum {
    PRESSURE_NONE      = 0,    // PSI some.avg10 < 5% , MemAvail > 40% 
    PRESSURE_LOW       = 1,    // PSI some.avg10 > 5%    OR Memavail < 40%
    PRESSURE_MODERATE  = 2,    // PSI some.avg10 > 20%   OR swap_out > 20 pg/s
    PRESSURE_HIGH      = 3,    // PSI full.avg10 > 5%    OR MemAvail < 10%
    PRESSURE_CRITICAL =  4,    // PSI full.avg10 > 20%   OR MemAvail < 3%
} PressureLevel;

/* Daemon state machine */
typedef enum {
    STATE_IDLE         = 0,
    STATE_MONITORING   = 1,
    STATE_COMPRESSING =  2,
    STATE_RECOVERY     = 3,
    STATE_EMERGENCY    = 4,
} DaemonState;

/* Live snapshot of all memory metrics */
typedef struct {
    /* /proc/meminfo fields (kB)*/
    uint64_t mem_total_kb;
    uint64_t mem_available_kb;
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;
    uint64_t swap_cached_kb;
    uint64_t dirty_kb;

    /* Derived */
    double mem_avail_pct;      /* MemAvailable / MemTotal * 100 */
    double swap_used_pct;      /* (SwapTotal - SwapFree) / SwapTotal * 100 */

    /* /proc/vmstat deltas (pages/sec since last sample) */
    uint64_t pswpin_rate;      /* swapin rate (pages/sec) */
    uint64_t pswpout_rate;     /* swapout rate (pages/sec) */
    uint64_t pgmajfault_rate;  /* major faults/sec */

    /* /proc/pressure/memory */
    double psi_some_avg10;
    double psi_some_avg60;
    double psi_some_avg300;
    double psi_full_avg10;
    double psi_full_avg60;
    double psi_full_avg300;

    /* zram stats (from /sys/block/zram0/mm_stat)*/
    uint64_t zram_orig_bytes;
    uint64_t zram_compr_bytes;
    double   zram_ratio;           /* orig_bytes / compr_bytes */


    PressureLevel level;           /* computed after fill */
    struct timespec sampled_at;     /* when snapshot was taken */
} PressureState;

/* Compute level from filled PressureState */
PressureLevel pressure_classify(const PressureState *ps);

/* Sample all source and fill ps. Returns 0 on success */
int pressure_sample(PressureState *ps);

/* Human-readable level name */
const char * pressure_level_name(PressureLevel lv);


#endif /* ZPRESSD_PRESSURE_H */