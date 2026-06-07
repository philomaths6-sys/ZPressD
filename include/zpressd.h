#ifndef ZPRESSD_H
#define ZPRESSD_H

#define ZPRESSD_VERSION_MAJOR   1
#define ZPRESSD_VERSION_MINOR   0
#define ZPRESSD_VERSION_PATCH   0
#define ZPRESSD_VERSION_STR     "1.0.0"

#define ZPRESSD_PID_FILE     "/var/run/zpressd.pid"
#define ZPRESSD_CONFIG_FILE  "/etc/zpressd.conf"
#define ZPRESSD_LOG_FILE     "/var/log/zpressd.log"


#include "logger.h"
#include "config.h"
#include "pressure.h"
#include "process.h"
#include "cold_page.h"
#include "classifier.h"
#include "compression_ctrl.h"
#include "zswap_tuner.h"
#include "proc_utils.h"

/* Global daemon state -- set by signal handelers */
extern volatile int g_running;                    /* set to 0 on SIGTERM/SIGINT */
extern volatile int g_reload_config;              /* set to 1 on SIGHUP */

#endif /* ZPRESSD_H */