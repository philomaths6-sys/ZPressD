#include "classifier.h"
#include "process.h"
#include "config.h"
#include "logger.h"
#include <string.h>

/* Built in Protected terminal/editor names */
static const char *PROTECTED_BUILTIN[] = {
    "bash", "zsh", "fish", "sh", "dash",
    "vim", "vi", "nano", "emacs", "nvim",
    "code", "code-server", "genome-terminal",
    "konsole", "xterm", "alacritty", "kitty", "wezterm",
    "tmux", "screen", "sway", "i3",
    "zpressd",                /* never compress overselves */
    NULL
};

static int is_protected_name(const char *comm, const Config *cfg) {
    /* check built-in list*/
    for (int i=0;PROTECTED_BUILTIN[i];i++){
        if (strcmp(comm , PROTECTED_BUILTIN[i]) == 0) return 1;
    }
    /* Check user config list */
    for (int i=0; i< cfg->protected_count;i++){
        if (strcmp(comm, cfg->protected_names[i]) == 0) return 1;
    }
    return 0;
}


void classify_process(ProcessInfo *proc, Config *cfg) {
    /* PID 0 or 1 or empty comm -> kernel/init */
    if (proc->pid <=1 || proc->comm[0] == '\0') {
        proc->p_class = CLASS_KERNEL;
        return;
    }
    /* Kthreads have RSS = 0 */
    if (proc->rss_kb == 0) {
        proc->p_class = CLASS_KERNEL;
        return;
    }
    /* Protected name */
    if(is_protected_name(proc->comm, cfg)) {
        proc->p_class = CLASS_INTERACTIVE;
        return;
    }
    /* Has controlling terminal -> interactive */
    if (proc->tty_nr != 0) {
        proc->p_class = CLASS_INTERACTIVE;
        return;
    }
    /* can add high voluntry ctxt switches process also as interactive // for later */
    /* Default : background */
    proc->p_class = CLASS_BACKGROUND;
}

void classify_all(ProcessList *p1, const Config *cfg) {
    for(int i=0; i<p1->count; i++) {
        classify_process(&p1->procs[i], cfg);
    }
}
