#include "cold_page.h"
#include "process.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>


void cold_score_compute(ProcessInfo *proc) {
    /* fault_delta: page faults since last sample (both minor + major) */
    uint64_t fault_delta = proc->minflt_delta + proc->majflt_delta;
     
    /* rss_weight: larger processes are better compression target (normalised to100MB )*/
    double rss_weight = (double)proc->rss_kb / (100.0 * 1024);
    if(rss_weight > 10.0) rss_weight = 10.0;   /* cap at 1000MB */
    if(rss_weight < 0.01) rss_weight = 0.01;   /* floor for tiny processes  */

    /* idle_weight: how long since last non-zero fault delta */
    double idle_capped = proc->idle_seconds > 60 ? 60 : proc->idle_seconds;
    double idle_weight = idle_capped / 60.0;

    /* Core formula */
    proc->cold_score = (1.0 / ((double)fault_delta + 1.0)) * rss_weight * idle_weight;

    /* Bonus: already partially swapped -> even better target */
    if(proc->swap_kb > 0) {
        proc->cold_score *= 1.2;
    }

    ZP_DEBUG("PID %d (%s): fault_delta=%lu, rss=%lukB, idle=%lus, score=%.4f",
                proc->pid, proc->comm, fault_delta, proc->rss_kb, proc->idle_seconds, proc->cold_score);
}


/* qsort comparator - descending score */
static int score_cmp_desc(const void *a, const void *b) {
    const ProcessInfo *pa = (const ProcessInfo *)a;
    const ProcessInfo *pb = (const ProcessInfo *)b;
    if(pb->cold_score > pa->cold_score) return 1;
    if(pb->cold_score < pa->cold_score) return -1;
    return 0;
}

void cold_score_all(ProcessList *p1) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    for(int i=0; i<p1->count; i++) {
        ProcessInfo *p = &p1->procs[i];
        if (p->p_class != CLASS_BACKGROUND) {
            p->cold_score = 0.0;
            continue;
        }
        /* Update idle_seconds */
        if(p->minflt_delta ==0 && p->majflt_delta == 0)
            p->idle_seconds++;
        else
            p->idle_seconds = 0;
        cold_score_compute(p); 
    }
    /* Sort entire list so top candidates are at front */
    qsort(p1->procs, p1->count, sizeof(ProcessInfo), score_cmp_desc);
}

int cold_top_candidates(ProcessList *p1, int n, ProcessInfo **out) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int found = 0;
    for(int i=0; i<p1->count && found < n; i++) {
        ProcessInfo *p = &p1->procs[i];
        if(p->p_class != CLASS_BACKGROUND) continue;
        if(p->cold_score <= 0.0) break;   /* sorted desc , rest are 0*/
        /* Skip cooling-off period */
        if (p->last_hinted_at.tv_sec > 0) {
            double since = difftime(now.tv_sec, p->last_hinted_at.tv_sec);
            if(since < 30.0) { p->cooling_off = 1; continue;}                   /*hardcoded 30 here later review this part*/ 
        }
        p->cooling_off = 0;
        out[found++] = p;
    }
    return found;
}
