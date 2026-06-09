#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#include <sys/syslog.h>

static LogLevel g_min_level = LOG_LEVEL_DEBUG;
static FILE *g_logfile = NULL;
static int g_use_syslog = 0;

static const char *level_str[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

static const int syslog_prio[] = {
    LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR, LOG_CRIT
};

void logger_init(LogLevel min_level, const char *logfile_path, int use_syslog) {
    g_min_level = min_level;
    g_use_syslog = use_syslog;
    if(logfile_path &&  *logfile_path) {
        g_logfile = fopen(logfile_path, "a"); 
        if(!g_logfile) perror("logger: cannot open logfile");
    }
    if(use_syslog){
        openlog("zpressd", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }
}

void logger_close(void){
    if(g_logfile) { fclose(g_logfile); g_logfile = NULL; };
    if(g_use_syslog) closelog();
}


void _log_write(LogLevel level, const char *file, int line, const char *fmt, ...) {
    if(level < g_min_level) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_info);

    FILE *dest = g_logfile ? g_logfile : stderr;
    fprintf(dest, "[%s.%03ld] [%s] (%s:%d) %s\n", tbuf, ts.tv_nsec/1000000, level_str[level], file, line, msg);
    fflush(dest);

    if(g_use_syslog) {
        syslog(syslog_prio[level], "(%s:%d) %s", file, line, msg);
    }   
}
