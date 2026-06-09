#include "zswap_tuner.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>


static int write_sysfs(const char *path, const char *val) {
    FILE *f = fopen(path, "w");
    if(!f) {
        ZP_ERROR("zswap_tuner: cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    fprintf(f, "%s\n", val);
    fclose(f);
    return 0;
}

int zswap_available(void) {
    FILE *f = fopen(ZSWAP_ENABLED, "r");
    if(!f) return 0;
    char buf[8] = {0};
    fgets(buf, sizeof(buf), f);
    fclose(f);
    return (buf[0] == 'Y' || buf[0] == '1') ? 1 : 0;
}

int zswap_read_state(ZswapState *zs) {
    memset(zs, 0, sizeof(*zs));
    FILE *f;
    /* compressor */
    f = fopen(ZSWAP_COMPRESSOR, "r");
    if(f){ fgets(zs->current_compressor, sizeof(zs->current_compressor), f); fclose(f); }
    /* strip trailing newline */
    char *n1 = strchr(zs->current_compressor, '\n');
    if(n1) *n1 = '\0';
    /* max_pool_percent */
    f = fopen(ZSWAP_MAX_POOL, "r");
    if (f) { fscanf(f, "%d", &zs->current_max_pool_pct); fclose(f); }
    zs->zswap_enabled = zswap_available();
    return 0;
}


/* Hysteresis: track when we last changed */
static struct timespec s_last_tune = {0, 0};
static PressureLevel s_last_level = PRESSURE_NONE;

int zswap_tune(ZswapState *zs, PressureLevel level, int dry_run) {
    if(!zs->zswap_enabled) {
        // ZP_WARN("zswap_tune: zswap not enabled on this system"); //added this extra
        return 0;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Hysteresis: don't thrash zswap params */
    if (s_last_level == level && s_last_tune.tv_sec > 0) {
        double held = difftime(now.tv_sec, s_last_tune.tv_sec);
        if (held < 5.0) return 0;   /* only retune if level held for 5 seconds */   
    }

    const char *compressor;
    int pool_pct;

    switch(level) {
        case PRESSURE_CRITICAL:
            compressor = "zstd"; pool_pct = 50; break;
        case PRESSURE_HIGH:
            compressor = "lz4hc"; pool_pct = 40; break;
        case PRESSURE_MODERATE:
            compressor = "lz4"; pool_pct = 30; break;
        default:   /* LOW or NONE -> revert */
            compressor = "lz4"; pool_pct = 20; break;
    }

    char pool_str[8];
    snprintf(pool_str, sizeof(pool_str), "%d", pool_pct);

    if(dry_run) {
        ZP_INFO("[DRY-RUN] zswap: would set compressor=%s pool=%d%%",compressor, pool_pct);
        return 0;
    }

    int changed = 0;
    if(strcmp(zs->current_compressor, compressor) != 0) {
        write_sysfs(ZSWAP_COMPRESSOR, compressor);
        ZP_INFO("zswap: compressor %s -> %s", zs->current_compressor, compressor);
        strncpy(zs->current_compressor, compressor, sizeof(zs->current_compressor)-1);
        changed = 1;
    }
    if(zs->current_max_pool_pct != pool_pct) {
        write_sysfs(ZSWAP_MAX_POOL, pool_str);
        ZP_INFO("zswap: max_pool_percent %d%% -> %d%%", zs->current_max_pool_pct, pool_pct);
        zs->current_max_pool_pct = pool_pct;
        changed = 1;
    }
    if(changed) {
        s_last_tune = now; 
        s_last_level = level;
    }
    return 0;
}