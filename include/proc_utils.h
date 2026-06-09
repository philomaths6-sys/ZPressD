#ifndef ZPRESSD_PROC_UTILS_H
#define ZPRESSD_PROC_UTILS_H

#include <sys/types.h>
#include <stdint.h>

/* /proc/meminfo : key-value pairs -> look up specific field */
uint64_t proc_meminfo_get_kb(const char *field);

/* /proc/vmstat : look up a single counter */
uint64_t proc_vmstat_get(const char *field);

/* Parse PSI file (/proc/pressure/memory or /proc/pressure/cpu)*/
typedef struct {
    double some_avg10, some_avg60, some_avg300;
    double full_avg10, full_avg60, full_avg300;
    uint64_t some_total, full_total;
} PsiMetrics;

int proc_parse_psi(const char *path, PsiMetrics *out);

/* /proc/[pid]/stat: fields we care about (subset) */
typedef struct {
    pid_t pid;
    char comm[64];
    char state;
    uint64_t minflt;
    uint64_t majflt;
    uint64_t utime;
    uint64_t stime;
    int tty_nr;
    long nice;
    uint64_t starttime;
    uint64_t voluntary_ctxt_switches;
} ProcStat;

int proc_read_stat(pid_t pid, ProcStat *out);

/* /proc/[pid]/smaps_rollup */
typedef struct {
    uint64_t rss_kb;
    uint64_t pss_kb;
    uint64_t swap_kb;
    uint64_t private_clean_kb;
    uint64_t private_dirty_kb;
} SmapsRollup;

int proc_read_smaps_rollup(pid_t pid, SmapsRollup *out);

/* /proc/[pid]/maps: one VMA region */
typedef struct {
    unsigned long start;
    unsigned long end;
    char          perms[8];
    char          pathname[256];   /* empty for anonymous */
    int           is_anon;         /* 1 if anonymous (heap/stack) */
} VmaRegion;

/* Read all VMA regions for pid. out must be free()d by caller. */
int proc_read_maps(pid_t pid, VmaRegion **out, int *count);

/* Enumerate all numeric entries in /proc. pids must hold >= MAX_PROCESSES ints. */
int proc_enum_pids(pid_t *pids, int max_count);


/* /sys/block/zram0/mm_stat */
typedef struct {
    uint64_t orig_data_size;
    uint64_t compr_data_size;
    uint64_t mem_used_total;
    uint64_t mem_limit;
    uint64_t mem_used_max;
    uint64_t same_pages;
    uint64_t pages_compacted;
} ZramMmStat;

int proc_read_zram_mmstat(const char *dev, ZramMmStat *out);

#endif /* ZPRESSD_PROC_UTILS_H */


