/*
 * CSE344 HW4 - Reader Process Header
 */

#ifndef READER_H
#define READER_H

#include "shm.h"

typedef struct {
    int            reader_index;
    const char    *filename;
    int            num_threads;       /* -t flag */
    region_a_t    *region_a;
    int            pipe_fd;           /* write-end of heartbeat pipe */
} reader_args_t;

/*
 * Entry point for Reader child process.
 * Called after fork() in the child.
 */
void reader_run(const reader_args_t *args);

#endif /* READER_H */
