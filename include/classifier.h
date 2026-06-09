#ifndef ZPRESSD_CLASSIFIER_H
#define ZPRESSD_CLASSIFIER_H

#include "process.h"
#include "config.h"

/*
    * Classifiaction rules (in oriority order):
    *   1. PID==0 or tty_nr == -1 -> KERNEL
    *   2. comm matches proctected_names list -> INTERACTIVE
    *   3. tty_nr !=0 -> INTERACTIVE(has controlled terminal)
    *   4. voluntary_ctxt_switches rate high (>100/sec) -> INTERACTIVE heuristic
    *   5. Default -> BACKGROUND
*/

/* Classify a single process. Updates proc->class*/
void classify_process(ProcessInfo *proc,const Config *cfg);

/* Classify all processes in list. */
void classify_all(ProcessList *p1, const Config *cfg);

#endif /* ZPRESSD_CLASSIFIER_H */