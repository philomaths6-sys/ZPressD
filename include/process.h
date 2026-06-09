#ifndef ZPRESSD_PROCESS_H
#define ZPRESSD_PROCESS_H

#include <sys/types.h>
#include <stdint.h>
#include <time.h>

#define PROC_NAME_MAX 64
#define MAX_PROCESSES 4096

typedef enum {
    CLASS_UNKNOWN     = 0, 
    CLASS_INTERACTIVE = 1,  /* has tty , or in protected list */
    CLASS_BACKGROUND  = 2,  /* no tty , compresssion candidate */
    CLASS_KERNEL      = 3,  /* kthread -- never touch */
    CLASS_DAEMON      = 4,  /* system daemon - configurable */
} ProcessClass;


typedef struct {
    pid_t pid;
    char comm[PROC_NAME_MAX];   /* /proc/pid/comm */
    char state;                 /* R S D Z T */

    /* From /proc/pid/stat */
    int      tty_nr;
    uint64_t utime;
    uint64_t stime;
    uint64_t minflt;
    uint64_t majflt;
    int      voluntary_ctxt_switches;

    /* Deltas since last sample */
    uint64_t minflt_delta;
    uint64_t majflt_delta;


    /* From /proc/pid/smap_rollup (kB)*/
    uint64_t rss_kb;
    uint64_t pss_kb;
    uint64_t swap_kb;
    uint64_t anon_kb;

    /* Classification */
    ProcessClass p_class;

    /* Scoring */
    double cold_score;
    uint64_t idle_seconds;                   /* seconds since last non-zero fault delta */
    struct timespec last_hinted_at;          /* timestamp of last madvise call */
    int      cooling_off;                    /* 1 if in 30s cooldown period */

    struct timespec sampled_at;              /* when snapshot was taken */
} ProcessInfo;

typedef struct {
    ProcessInfo *procs;
    int          count;
    int          capacity;
} ProcessList;


ProcessList *proclist_alloc(int capacity);
void         proclist_free(ProcessList *p1);
int          proclist_refresh(ProcessList *p1);              /* re-enumerate /proc */
ProcessInfo *proclist_find_pid(ProcessList *p1, pid_t pid);

#endif /* ZPRESSD_PROCESS_H */