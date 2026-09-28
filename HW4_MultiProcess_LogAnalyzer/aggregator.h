/*
 * CSE344 HW4 - Aggregator Process Header
 */

#ifndef AGGREGATOR_H
#define AGGREGATOR_H

#include "shm.h"

typedef struct {
    region_c_t     *region_c;
    region_d_t     *region_d;
    int             timeout_sec;       /* -T flag */
    int             num_keywords;
    char            keywords[MAX_KEYWORDS][MAX_SOURCE];
    const char     *output_file;       /* -o flag */
    const char     *binary_file;       /* -O flag */
    int             num_files;
    int             num_workers;
} aggregator_args_t;

/*
 * Entry point for Aggregator child process.
 */
void aggregator_run(const aggregator_args_t *args);

#endif /* AGGREGATOR_H */
