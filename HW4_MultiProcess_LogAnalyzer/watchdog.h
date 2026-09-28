/*
 * CSE344 HW4 - Watchdog Thread Header
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <signal.h>

#define MAX_READERS 32

typedef struct {
    int             pipe_fds[MAX_READERS];    /* read-ends of heartbeat pipes */
    char            filenames[MAX_READERS][256];
    int             num_readers;
    volatile sig_atomic_t *shutdown_flag;       /* set by parent to stop watchdog */
    int             num_children;              /* total forked children */
} watchdog_args_t;

/*
 * Watchdog thread function.
 * Runs inside the parent process, uses select() on pipes.
 */
void *watchdog_thread_func(void *arg);

#endif /* WATCHDOG_H */
