#ifndef ZPRESSD_COMPRESSION_CTRL_H
#define ZPRESSD_COMPRESSION_CTRL_H

#include "process.h"
#include "pressure.h"
#include "config.h"
#include <stdint.h>


#define MADVISE_BUDGET_BYTES  (500ULL * 1024 * 1024)     /*  500 MB per cycle */
#define COOLING_PERIOD_SECS   30

typedef struct {
    uint64_t total_hinted_bytes;          /*  bytes hinted this cycle */
    int      processes_hinted;            /* number of processes acted on */
    int      regions_hinted;              /* number of VMA regions hinted */
    int      skipped_cooling;             /* process skipped due to cooldown */
    int      skipped_interactive;         /* process skipped  (classified INTERACTIVE ) */
} HintResult;


/*
 * Issue MADV_PAGEOUT on all anonymous VMA regions of proc.
 * Reads /proc/pid/maps, filters anon regions, calls madvise().
 * budget_remaining: in/out - decremented by bytes hinted.
 * dry_run: if 1, log but dont call madvise.
 * Returns bytes hinted (0 on error or dry-run).
*/
uint64_t hint_process(ProcessInfo *proc, uint64_t *budget_remaining, int dry_run);

/*
 * Run a full compression cycle over top-N candidates.
 * Fills result. Returns 0 on success.
*/

int run_compression_cycle(ProcessList *p1, const PressureState *ps, const Config *cfg, HintResult *result);

#endif /* ZPRESSD_COMPRESSION_CTRL_H */