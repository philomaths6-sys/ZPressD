#include "process.h"
#include "proc_utils.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>


ProcessList *proclist_alloc(int capacity) {
    ProcessList *p1 = malloc(sizeof(ProcessList));
    if(!p1) return NULL;
    p1->procs = calloc(capacity, sizeof(ProcessInfo));
    if(!p1->procs) { free(p1); return NULL; }
    p1->count = 0;
    p1->capacity = capacity;
    return p1;
}

void proclist_free(ProcessList *p1) {
    if(p1) {
        free(p1->procs);
        free(p1);
    }
}

int proclist_refresh(ProcessList *p1) {
    pid_t pids[MAX_PROCESSES];
    int npids = proc_enum_pids(pids, MAX_PROCESSES);
    if(npids < 0) return -1;

    /* Build new snapshot */
    ProcessInfo new_procs[MAX_PROCESSES];
    int new_count = 0;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    for(int i=0; i < npids ; i++){
        ProcStat ps;
        SmapsRollup sr;
        if(proc_read_stat(pids[i], &ps) != 0) continue;
        if(proc_read_smaps_rollup(pids[i], &sr) != 0) memset(&sr, 0, sizeof(sr));

        ProcessInfo *pnew = &new_procs[new_count++];
        memset(pnew, 0, sizeof(*pnew));
        pnew->pid = pids[i];
        pnew->tty_nr = ps.tty_nr;
        pnew->state = ps.state;
        pnew->utime = ps.utime;
        pnew->stime = ps.stime;
        pnew->minflt = ps.minflt;
        pnew->majflt = ps.majflt;
        pnew->rss_kb = sr.rss_kb;
        pnew->pss_kb = sr.pss_kb;
        pnew->swap_kb = sr.swap_kb;
        pnew->anon_kb = sr.private_dirty_kb;
        strncpy(pnew->comm, ps.comm, sizeof(pnew->comm)-1);
        pnew->sampled_at = now;
        /* Compute deltas from previous snapshot */
        ProcessInfo *prev = proclist_find_pid(p1, pids[i]);
        if(prev) {
            pnew->minflt_delta = pnew->minflt - prev->minflt;
            pnew->majflt_delta = pnew->majflt - prev->majflt;
            pnew->idle_seconds = prev->idle_seconds;  /* will be updated in cold_score_all */
            pnew->last_hinted_at = prev->last_hinted_at;
        }
    }
    /* replace old list */
    if(new_count > p1->capacity) {
    ProcessInfo *tmp = realloc(p1->procs, new_count * sizeof(ProcessInfo));
    if(!tmp) { free(p1->procs); p1->procs = NULL; return -1; }
    p1->procs = tmp;
    p1->capacity = new_count;
    }
    memcpy(p1->procs, new_procs, new_count * sizeof(ProcessInfo));
    p1->count = new_count;
    return 0;
    
}

ProcessInfo *proclist_find_pid(ProcessList *p1, pid_t pid) {
    for(int i=0; i<p1->count; i++) {
        if(p1->procs[i].pid == pid) return &p1->procs[i];
    }
    return NULL;
}
