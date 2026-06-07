#ifndef ZPRESSD_CONFIG_H
#define ZPRESSD_CONFIG_H

#include <stdint.h>

#define CONFIG_PATH_DEFAULT "/etc/zpressd.conf"
#define MAX_PROTECTED_NAMES 64
#define PROCTECTED_NAME_LEN 64

typedef struct {
    /* Polling interval in (millisecond) */
    int poll_interval_idle_ms;         /* default : 500 */
    int poll_interval_active_ms;       /* default : 100 */

    /* Pressure thresholds */
    double threshold_low_psi_some;         /* default : 5.0  */
    double threshold_mod_psi_some;         /* default : 20.0 */
    double threshold_high_psi_full;        /* default : 5.0  */
    double thrshold_crit_psi_full;         /* default : 20.0 */
    double threshold_low_mem_pct;          /* default : 40.0 */
    double threshold_high_mem_pct;         /* default : 10.0 */
    double threshold_crit_mem_pct;         /* default : 3.0  */

    /* Compression behavior */
    uint64_t madvise_budget_mb;            /* default : 500  */
    int      top_candidates_n;             /* default : 5    */
    int      cooling_period_secs;          /* default : 30   */
    int      hysteresis_secs;              /* default : 5    */


    /* Flags */
    int    dry_run;                        /* default : 0    */
    int    tune_zswap;                     /* default : 1    */
    int    log_level;                      /* default : INFO */
    char   log_file[256];                  /* default : /var/log/zpressd.log */

    /* Procted process names (user-defined)*/
    char   protected_names[MAX_PROTECTED_NAMES][PROCTECTED_NAME_LEN];
    int    protected_count;
} Config;

/* Load config file. Missing keys use defaults.  Returns 0 on success */
int config_load(Config *cfg, const char *path);

/* Fill cfg with defaults (call before config_load) */
void config_defaults(Config *cfg);

/* Print current config to log */
void config_dump(const Config *cfg);

#endif /* ZPRESSD_CONFIG_H */