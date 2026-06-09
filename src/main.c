#include "zpressd.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>


volatile int g_running = 1;
volatile int g_reload_config = 0;

static void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM)  g_running = 0;
    if (sig == SIGHUP) g_reload_config = 1;
}


static void daemonize(void) {
    pid_t pid = fork();
    if(pid < 0) { perror("fork"); exit(1); }
    if(pid > 0) exit(0);  /* parent exits */
    setsid();
    /* Write PID file */
    FILE *pf = fopen(ZPRESSD_PID_FILE, "w");
    if(pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
    /* Redirect stdio */
    int fd = open("/dev/null", O_RDWR);
    if (fd>=0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); close(fd); }
}


static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}


int main(int argc, char *argv[]) {
    int foreground =0;
    int dry_run = 0;
    const char *config_path = ZPRESSD_CONFIG_FILE;

    for(int i=1; i<argc; i++) {
        if (!strcmp(argv[i], "-f")) foreground = 1;
        if (!strcmp(argv[i], "-n")) dry_run = 1;
        if (!strcmp(argv[i], "-c") && i+1 < argc) config_path = argv[++i];
        if (!strcmp(argv[i], "--version")) {
            printf("zpressd version %s\n", ZPRESSD_VERSION_STR);
            return 0;
        }
        if (!strcmp(argv[i], "--help")) {
            printf("Usage: zpressd [-f] [-n] [-c config_path]\n");
            printf("  -f     foreground (don't daemonize)\n");
            printf("  -n     dry-run (log but don't call madvise)\n");
            printf("  -c     config file path (default: %s)\n", ZPRESSD_CONFIG_FILE);
            return 0;
        }
    }
    /* Load config */
    Config cfg;
    config_load(&cfg, config_path);
    if (dry_run) cfg.dry_run = 1;

    /* Init logger */
    logger_init((LogLevel)cfg.log_level, cfg.log_file, !foreground);
    ZP_INFO("zpressd %s starting (dry-run=%d foreground=%d)", ZPRESSD_VERSION_STR, cfg.dry_run, foreground);
    config_dump(&cfg);

    if(!foreground) daemonize();

    /* Signal handlers */
    signal(SIGTERM , handle_signal);
    signal(SIGINT  , handle_signal);
    signal(SIGHUP  , handle_signal);
    signal(SIGPIPE , SIG_IGN);


    /* Allocate structures */
    ProcessList *p1 = proclist_alloc(MAX_PROCESSES);
    if (!p1) { ZP_FATAL("OOM allocating process list"); return 1; }

    PressureState ps;
    ZswapState zs;
    zswap_read_state(&zs);

    if (!zswap_available()) { ZP_WARN("zswap not available - tuning disabled"); }

    DaemonState state = STATE_IDLE;
    int consec_low   = 0;     /* consecutive LOW reading for hysteresis */
    int consec_active = 0;     /* consecutive elevated reading */

    ZP_INFO("Entering main loop");

    while(g_running) {
        /* Hot-reload config on SIGHUP */
        if (g_reload_config) {
            config_load(&cfg, config_path);
            if (dry_run) cfg.dry_run = 1;
            config_dump(&cfg);
            g_reload_config = 0;
            ZP_INFO("Config reloaded");
        }

        /* Sample pressure */
        pressure_sample(&ps);

        /* State machine */
        switch (state) {
            case STATE_IDLE:
                if (ps.level >= PRESSURE_LOW) {
                    consec_active++;
                    if( consec_active >= 3) {
                        state = STATE_MONITORING;
                        consec_active = 0;
                        ZP_INFO("IDLE -> MONITORING (PSI=%.2f)", ps.psi_some_avg10);  
                    }  
                } else consec_active = 0;
                sleep_ms(cfg.poll_interval_idle_ms);
                break;

            case STATE_MONITORING:
                proclist_refresh(p1);
                classify_all(p1, &cfg);
                cold_score_all(p1);
                if (ps.level >= PRESSURE_MODERATE) {
                    state = STATE_COMPRSESSING;
                    ZP_INFO("MONITORING -> COMPRESSING (PSI=%.2f/%.2f)", ps.psi_some_avg10, ps.psi_full_avg10);
                } else if (ps.level == PRESSURE_NONE) {
                    state = STATE_IDLE;
                    ZP_INFO("MONITORING -> IDLE (pressure cleared)");
                }
                sleep_ms(cfg.poll_interval_active_ms);
                break;

            case STATE_COMPRSESSING: {
                proclist_refresh(p1);
                classify_all(p1, &cfg);
                cold_score_all(p1);

                if(cfg.tune_zswap) {
                    zswap_tune(&zs, ps.level, cfg.dry_run);
                }

                HintResult hr;
                run_compression_cycle(p1, &ps, &cfg, &hr);
                ZP_INFO("[%s] hinted=%d procs %.1f MB | PSI=%.2f/%.2f MemAvail=%.1f%% ",
                    pressure_level_name(ps.level), hr.processes_hinted, (double)hr.total_hinted_bytes/(1024*1024),
                    ps.psi_some_avg10, ps.psi_full_avg10, ps.mem_avail_pct);
                
                if(ps.level < PRESSURE_MODERATE) {
                    state = STATE_RECOVERY;
                    consec_low = 0;
                    ZP_INFO("COMPRESSING -> RECOVERY");
                }
                if(ps.level == PRESSURE_CRICTICAL) {
                    state = STATE_EMERGENCY;
                    ZP_WARN("-> EMERGENCY: PSI=%.2f MemAvail=%.1f%%", ps.psi_full_avg10, ps.mem_avail_pct);
                }
                sleep_ms(cfg.poll_interval_active_ms);
                break;
            }

            case STATE_RECOVERY:
                if (ps.level == PRESSURE_NONE || ps.level == PRESSURE_LOW) {
                    consec_low++;
                    if (consec_low >=5){
                        zswap_tune(&zs, PRESSURE_NONE, cfg.dry_run);  /* revert zswap immediately on recovery */
                        state = STATE_IDLE;
                        ZP_INFO("RECOVERY -> IDLE ");
                    }
                } else {
                        consec_low = 0;
                        state = STATE_COMPRSESSING;
                        ZP_INFO("RECOVERY -> COMPRESSING (pressure returned)");
                    }
                sleep_ms(cfg.poll_interval_active_ms);
                break;


            case STATE_EMERGENCY: {
                proclist_refresh(p1);
                classify_all(p1, &cfg);
                cold_score_all(p1);
                Config emergency_cfg = cfg;
                emergency_cfg.top_candidates_n = 20;   /* hint more processes */
                emergency_cfg.madvise_budget_mb = 2048; /*2 GB budget */
                HintResult hr;
                run_compression_cycle(p1, &ps, &emergency_cfg, &hr);
                ZP_WARN("EMERGENCY cycle: hinted %d procs %.1f MB ",
                    hr.processes_hinted, (double)hr.total_hinted_bytes/(1024*1024));
                if(ps.level < PRESSURE_CRICTICAL) {
                    state = STATE_RECOVERY;
                    ZP_INFO("EMERGENCY -> RECOVERY ");
                }
                sleep_ms(cfg.poll_interval_active_ms);
                break;
            }
        }   
    }
    ZP_INFO("zpressd shutting down (received signal)");
    proclist_free(p1);
    logger_close();
    unlink(ZPRESSD_PID_FILE);
    return 0;
}