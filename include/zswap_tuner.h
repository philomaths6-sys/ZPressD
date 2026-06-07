#ifndef ZPRESSD_ZSWAP_TUNER_H
#define ZPRESSD_ZSWAP_TUNER_H

#include "pressure.h"

#define ZSWAP_PARAM_BASE "/sys/module/zswap/parameters/"
#define ZSWAP_COMPRESSOR  ZSWAP_PARAM_BASE "/compressor"
#define ZSWAP_MAX_POOL    ZSWAP_PARAM_BASE "/max_pool_percent"
#define ZSWAP_ENABLED     ZSWAP_PARAM_BASE "/enabled"

typedef struct {
    char current_compressor[32];
    int  current_max_pool_pct;
    int   zswap_enabled;
    PressureLevel last_tuned_at_level;
}  ZswapState;

/* Read current zswap state from sysfs */
int zswap_read_state(ZswapState *zs);

/*
 * Tune zswap parameters for the given pressure level.
 *MODERATE -> lz4 (fast), pool 30%
 *HIGH -> lz4hc (balanced), pool 40%
 *CRITICAL -> zstd (best ratio), pool 50%
 *Recovery -> revert to lz4, pool 20%
 *Implements 5-second hysteresis : won't change if level held < 5 seconds.
*/
int zswap_tune(ZswapState *zs, PressureLevel level, int dry_run);

/* Check if zswap is available on this kernel */
int zswap_available(void);

#endif /* ZPRESSD_ZSWAP_TUNER_H */