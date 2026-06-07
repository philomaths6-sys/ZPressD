#ifndef ZPREED_COLD_PAGE_H
#define ZPREED_COLD_PAGE_H


#include "process.h"

/* 
   * cold_score formula :
   * score = (1.0/ (fault_rate +1)) * rss_weight * idle_weight
   * rss_weight = rss_kb / (100*1024) -- larger RSS scores higher   //later view this you should normalise this.
   * idle_weight = min(idle_seconds, 60) / 60.0
   * 
   * 
   *  Score range : [0, 1]. Higher = better compression target.
*/

/* Compute cold_score for a single process. Updates proc->cold_score */
void cold_score_compute(ProcessInfo *proc);

/* Score all BACKGROUND process in list. Sort by score descending.*/
void cold_score_all(ProcessList *p1);

/* Return top N candidates (already scored+sorted). Retuens actual count. */
int cold_top_candidates(ProcessInfo *p1, int n, ProcessInfo **out);

#endif /* ZPREED_COLD_PAGE_H */