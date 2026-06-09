#include "config.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>



void config_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->poll_interval_idle_ms = 500;
    cfg->poll_interval_active_ms = 100;
    cfg->threshold_low_psi_some = 5.0;
    cfg->threshold_mod_psi_some = 20.0;
    cfg->threshold_high_psi_full = 5.0;
    cfg->threshold_crit_psi_full = 20.0;
    cfg->threshold_low_mem_pct = 40.0;
    cfg->threshold_high_mem_pct = 10.0;
    cfg->threshold_crit_mem_pct = 3.0;
    cfg->madvise_budget_mb = 500;
    cfg->top_candidates_n = 5;
    cfg->cooling_period_secs = 30;
    cfg->hysteresis_secs = 5;
    cfg->dry_run = 0;
    cfg->tune_zswap = 1;
    cfg->log_level = 1;         /* LOG_LEVEL_INFO */
    strncpy(cfg->log_file, "/var/log/zpressd.log", sizeof(cfg->log_file)-1);
}


int config_load(Config *cfg, const char *path) {
    config_defaults(cfg);
    FILE *f = fopen(path, "r");
    if(!f) {
        ZP_WARN("config: cannot open  %s, using defaults", path);
        return 0;  /* Defaults are valid */
    }

    char line[512] , key[128], val[256];
    while (fgets(line, sizeof(line), f)) {
        /* strip comments*/
        char *hash = strchr(line, '#');
        if(hash) *hash = '\0';
        if (sscanf(line, "%127s = %255s", key, val) != 2)continue;

        if  (!strcmp(key, "dry_run")) cfg->dry_run = atoi(val);
        else if (!strcmp(key, "tune_zswap")) cfg->tune_zswap = atoi(val);
        else if (!strcmp(key, "madvise_budget_mb")) cfg->madvise_budget_mb = atoll(val);
        else if (!strcmp(key, "top_candidates_n")) cfg->top_candidates_n = atoi(val);
        else if (!strcmp(key, "poll_idle_ms")) cfg->poll_interval_idle_ms = atoi(val);
        else if (!strcmp(key, "poll_active_ms")) cfg->poll_interval_active_ms = atoi(val);
        else if (!strcmp(key, "log_level")) cfg->log_level = atoi(val);
        else if (!strcmp(key, "log_file")) strncpy(cfg->log_file, val, sizeof(cfg->log_file)-1);
        else if (!strcmp(key, "protect")) {
            if (cfg->protected_count < MAX_PROTECTED_NAMES) {
                strncpy(cfg->protected_names[cfg->protected_count++], val, PROTECTED_NAME_LEN-1);
            }
        }
    }
    fclose(f);
    return 0;
}


void config_dump(const Config *cfg) {
    ZP_INFO("Config: dry_run=%d tune_zswap=%d budget=%luMB candidates=%d",
        cfg->dry_run, cfg->tune_zswap, cfg->madvise_budget_mb, cfg->top_candidates_n);
    ZP_INFO("Config: thresholds psi_some=%.1f/%.1f psi_full=%.1f/%.1f",
        cfg->threshold_low_psi_some, cfg->threshold_mod_psi_some, 
        cfg->threshold_high_psi_full, cfg->threshold_crit_psi_full);
}
