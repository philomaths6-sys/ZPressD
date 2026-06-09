#include "proc_utils.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>

uint64_t proc_meminfo_get_kb(const char *field) {
    FILE *f = fopen("/proc/meminfo", "r");
    if(!f) return 0;
    char line[256], key[128];
    uint64_t val = 0;
    while (fgets(line,sizeof(line),f)) {
        if (sscanf(line, "%127[^:]: %lu kB", key, &val) == 2) {
            if (strcmp(key, field) == 0) break;
        }
    }
    fclose(f);
    return val;
}


uint64_t proc_vmstat_get(const char *field) {
    FILE *f = fopen("/proc/vmstat", "r");
    if(!f) return 0;
    char line[256], key[128];
    uint64_t val = 0;
    while (fgets(line,sizeof(line),f)) {
        if (sscanf(line, "%127s %lu", key, &val) == 2) {
            if (strcmp(key, field) == 0) break;
        }
    }
    fclose(f);
    return val;
}

int proc_parse_psi(const char *path, PsiMetrics *out) {
    FILE *f = fopen(path, "r");
    if(!f) { ZP_WARN("PSI unavailable: %s", path); return -1; }
    memset(out, 0, sizeof(*out));
    char line[256];
    while (fgets(line,sizeof(line),f)) {
        if (strncmp(line, "some", 4) == 0) {
            sscanf(line, "some avg10=%lf avg60=%lf avg300=%lf total=%lu", 
                &out->some_avg10, &out->some_avg60, &out->some_avg300, &out->some_total);
        } else if (strncmp(line, "full", 4) == 0) {
            sscanf(line, "full avg10=%lf avg60=%lf avg300=%lf total=%lu", 
                &out->full_avg10, &out->full_avg60, &out->full_avg300, &out->full_total);
        }
    }
    fclose(f);
    return 0;    
}

int proc_read_stat(pid_t pid, ProcStat *out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if(!f) return -1;
    memset(out, 0, sizeof(*out));
    /* stat format: pid (comm) state ppid pgroup session tty_nr ...*/
    /* Read comm separately to handle spaces in process names */
    char buf[4096];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);
    char *l = strchr(buf, '(');
    char *r = strrchr(buf, ')');
    if(!l || !r) return -1;
    *r = '\0';
    snprintf(out->comm, sizeof(out->comm), "%s", l+1);
    /* Now parse remaining fields after ')' */
    char *rest = r + 2; /* skip ") " */
    long tty_nr;
    sscanf(rest,
        "%c %*d %*d %*d %ld %*d %*u %lu %*u %lu"     /*state..majflt*/
        " %*u %lu %lu",                                        /* utime stime */
        &out->state, &tty_nr, &out->minflt, &out->majflt,
        &out->utime, &out->stime);
    out->tty_nr = (int)tty_nr;
    out->pid = pid;
    return 0;
}

int proc_read_smaps_rollup(pid_t pid, SmapsRollup *out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/smaps_rollup", pid);
    FILE *f = fopen(path, "r");
    if(!f) return -1;
    memset(out, 0, sizeof(*out));
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        uint64_t val = 0;
        if (sscanf(line, "Rss: %lu kB", &val) == 1)     out->rss_kb = val;
        else if (sscanf(line, "Pss: %lu kB", &val) == 1) out->pss_kb = val;
        else if (sscanf(line, "Swap: %lu kB", &val) == 1) out->swap_kb = val;
        else if (sscanf(line, "Private_Dirty: %lu kB", &val) == 1) out->private_dirty_kb = val;
    }
    fclose(f);
    return 0;    
}


int proc_read_maps(pid_t pid, VmaRegion **out, int *count) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if(!f) return -1;
    int cap = 256 , n = 0;
    *out = malloc(cap * sizeof(VmaRegion));
    if(!*out) { fclose(f); return -1; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if(n == cap) {
            cap *= 2;
            *out = realloc(*out, cap * sizeof(VmaRegion));
        }
        VmaRegion *r = &(*out)[n];
        memset(r, 0, sizeof(*r));
        char perm[8];
        int items = sscanf(line, "%lx-%lx %7s %*x %*x:%*x %*u %255s",
                                    &r->start, &r->end, perm, r->pathname);
        strncpy(r->perms, perm, sizeof(r->perms) - 1);
        /* Anonymous: no pathname or [heap] [stack] */
        r->is_anon = (items < 4 || r->pathname[0] == '\0' ||
                                                r->pathname[0] == '[') ? 1 : 0;
        /* Must be writable to be a compression target */
        if (r->perms[1] == 'w' ) n++;
    }
    fclose(f);
    *count = n;
    return 0;
}


int proc_enum_pids(pid_t *pids, int max_count) {
    DIR *dir = opendir("/proc");
    if(!dir) return -1;
    int n=0;
    struct dirent *de;
    while ((de = readdir(dir)) && n<max_count) {
        if (de->d_type == DT_DIR && isdigit(de->d_name[0])) {
            pids[n++] = atoi(de->d_name);
        }
    }
    closedir(dir);
    return n;
}


int proc_read_zram_mmstat(const char *dev, ZramMmStat *out) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/block/%s/mm_stat", dev);
    FILE *f = fopen(path, "r");
    if(!f) return -1;
    memset(out, 0, sizeof(*out));
    fscanf(f, "%lu %lu %lu %lu %lu %lu %lu",
        &out->orig_data_size, &out->compr_data_size, &out->mem_used_total,
        &out->mem_limit, &out->mem_used_max, &out->same_pages, &out->pages_compacted);
    fclose(f);
    return 0;
     
}