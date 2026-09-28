/*
 * CSE344 HW4 - Dispatcher Process Implementation
 *
 * The Dispatcher:
 * - Reads entries from Region A using pthread_cond_timedwait
 * - Routes each entry to Region B[level_idx]
 * - If SOURCE is in priority list, also copies to Region D
 * - Tracks EOF markers; forwards EOF to Region B when all readers reported
 * - Sets dispatcher_done in Region D when finished
 */

#define _GNU_SOURCE
#include "dispatcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ─── Check if source is high-priority ──────────────────────── */
static int is_priority_source(const char *source, char **priority_sources, int num) {
    for (int i = 0; i < num; i++) {
        if (strcmp(source, priority_sources[i]) == 0)
            return 1;
    }
    return 0;
}

/* ─── Dispatcher Entry Point ────────────────────────────────── */
void dispatcher_run(const dispatcher_args_t *args) {
    printf("[PID:%d] Dispatcher started.\n", getpid());

    int eof_seen[NUM_LEVELS] = {0};
    int eof_forwarded[NUM_LEVELS] = {0};
    int all_eof_done = 0;

    while (!all_eof_done) {
        log_entry_t entry;
        int ret = region_a_pop(args->region_a, &entry, args->timeout_sec);

        if (ret == -1) {
            /* Timeout — check if all EOFs received */
            int all_done = 1;
            for (int i = 0; i < NUM_LEVELS; i++) {
                if (eof_seen[i] < args->total_readers) {
                    all_done = 0;
                    break;
                }
            }

            if (all_done) {
                /* Forward any remaining EOFs */
                for (int i = 0; i < NUM_LEVELS; i++) {
                    if (!eof_forwarded[i]) {
                        log_entry_t eof;
                        memset(&eof, 0, sizeof(eof));
                        eof.is_eof = 1;
                        eof.eof_level = i;
                        level_buffer_push(args->region_b[i], &eof);
                        eof_forwarded[i] = 1;
                    }
                }
                all_eof_done = 1;
            }
            continue;
        }

        if (entry.is_eof) {
            eof_seen[entry.eof_level]++;
            pthread_mutex_lock(&args->region_a->mutex);
            args->region_a->eof_count_per_level[entry.eof_level] =
                eof_seen[entry.eof_level];
            pthread_mutex_unlock(&args->region_a->mutex);

            /* Check if all readers sent EOF for this level */
            int count = eof_seen[entry.eof_level];

            if (count >= args->total_readers && !eof_forwarded[entry.eof_level]) {
                /* Forward EOF to Region B for this level */
                level_buffer_push(args->region_b[entry.eof_level], &entry);
                eof_forwarded[entry.eof_level] = 1;
            }

            /* Check if all levels have been forwarded */
            int all_fwd = 1;
            for (int i = 0; i < NUM_LEVELS; i++) {
                if (!eof_forwarded[i]) {
                    all_fwd = 0;
                    break;
                }
            }
            if (all_fwd) all_eof_done = 1;
            continue;
        }

        /* Normal entry: route to Region B based on level */
        int hp = is_priority_source(entry.source, args->priority_sources, args->num_priority);

        printf("[PID:%d] Routed entry to %s buffer. High-priority: %s (source: %s)\n",
               getpid(), LEVEL_NAMES[entry.level_idx],
               hp ? "YES" : "NO", entry.source);

        level_buffer_push(args->region_b[entry.level_idx], &entry);

        /* If high-priority, also copy to Region D */
        if (hp) {
            region_d_push(args->region_d, &entry);
        }
    }

    /* Signal Region D that dispatcher is done */
    pthread_mutex_lock(&args->region_d->mutex);
    args->region_d->dispatcher_done = 1;
    pthread_cond_broadcast(&args->region_d->not_empty);
    pthread_mutex_unlock(&args->region_d->mutex);

    printf("[PID:%d] All EOF markers forwarded to Region B. Exiting.\n", getpid());
}
