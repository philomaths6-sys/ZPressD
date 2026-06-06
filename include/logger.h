#ifndef ZPRESSD_LOGGER_H
#define ZPRESSD_LOGGER_H

#include <stdio.h>
#include <time.h>
#include <syslog.h>

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_FATAL = 4,
} LogLevel;

/* Intialise logger . Call once at daemon startup. */
void logger_init(LogLevel min_level,  const char * logfile_path, int use_syslog);
void logger_close(void);
/* Internal _ use macros below.*/
void _log_write(LogLevel level, const char *file, int line, const char *fmt, ...);

#define LOG_DEBUG(fmt, ...) _log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) _log_write(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) _log_write(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) _log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) _log_write(LOG_LEVEL_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif /* ZPRESSD_LOGGER_H */
