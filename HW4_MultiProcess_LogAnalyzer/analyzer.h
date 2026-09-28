/*
 * CSE344 HW4 - Analyzer Process Header
 */

#ifndef ANALYZER_H
#define ANALYZER_H

#include "shm.h"

typedef struct {
    int             level_idx;         /* 0=ERROR,1=WARN,2=INFO,3=DEBUG */
    int             num_workers;       /* -w flag */
    int             num_keywords;
    char            keywords[MAX_KEYWORDS][MAX_SOURCE];
    level_buffer_t *level_buffer;      /* Region B[level_idx] */
    region_c_t     *region_c;
} analyzer_args_t;

/*
 * Entry point for Analyzer child process.
 */
void analyzer_run(const analyzer_args_t *args);

#endif /* ANALYZER_H */
