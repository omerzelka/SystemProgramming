/*
 * CSE344 HW4 - Dispatcher Process Header
 */

#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "shm.h"

typedef struct {
    region_a_t      *region_a;
    level_buffer_t  *region_b[NUM_LEVELS];
    region_d_t      *region_d;
    int              total_readers;
    int              timeout_sec;       /* -T flag */
    char           **priority_sources;  /* array of source strings */
    int              num_priority;      /* number of priority sources */
} dispatcher_args_t;

/*
 * Entry point for Dispatcher child process.
 */
void dispatcher_run(const dispatcher_args_t *args);

#endif /* DISPATCHER_H */
