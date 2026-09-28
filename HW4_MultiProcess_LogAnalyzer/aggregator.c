/*
 * CSE344 HW4 - Aggregator Process Implementation
 *
 * The Aggregator:
 * - Waits for all 4 level_result_t in Region C using pthread_cond_timedwait
 * - Drains Region D for high-priority score
 * - Sorts levels by total weighted score descending
 * - Writes human-readable output file (-o)
 * - Writes binary checkpoint file (-O) with atomic rename
 */

#define _GNU_SOURCE
#include "aggregator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

static int all_results_ready(region_c_t *rc) {
    for (int i = 0; i < NUM_LEVELS; i++) {
        if (!rc->results[i].ready)
            return 0;
    }
    return 1;
}

/* ─── Count overlapping keyword occurrences ─────────────────── */
static int count_overlapping_agg(const char *haystack, const char *needle) {
    int count = 0;
    int nlen = (int)strlen(needle);
    if (nlen == 0) return 0;
    for (int i = 0; haystack[i] != '\0'; i++) {
        if (strncmp(&haystack[i], needle, nlen) == 0)
            count++;
    }
    return count;
}

/* ─── Aggregator Entry Point ────────────────────────────────── */
void aggregator_run(const aggregator_args_t *args) {
    printf("[PID:%d] Aggregator started. Waiting for 4 levels...\n", getpid());

    region_c_t *rc = args->region_c;

    /* Drain Region D early. If the priority buffer fills, the Dispatcher cannot
     * finish forwarding EOF markers, so waiting for Analyzer results first can
     * deadlock when -d is small (including the required baseline test). */
    double high_priority_score = 0.0;
    {
        log_entry_t entry;
        while (region_d_pop(args->region_d, &entry) == 0) {
            for (int k = 0; k < args->num_keywords; k++) {
                int cnt = count_overlapping_agg(entry.message, args->keywords[k]);
                if (cnt > 0) {
                    high_priority_score +=
                        (double)cnt * LEVEL_WEIGHT[entry.level_idx];
                }
            }
        }
    }

    /* Wait for all analyzer results using Region C's timed condvar.
     * Semaphores are still consumed for the required per-level completion signal. */
    int received[NUM_LEVELS] = {0};
    int received_count = 0;

    while (received_count < NUM_LEVELS) {
        int newly_ready[NUM_LEVELS];
        int newly_count = 0;

        pthread_mutex_lock(&rc->mutex);
        while (!all_results_ready(rc)) {
            int has_new = 0;
            for (int i = 0; i < NUM_LEVELS; i++) {
                if (rc->results[i].ready && !received[i]) {
                    has_new = 1;
                    break;
                }
            }
            if (has_new)
                break;

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += args->timeout_sec;

            int ret = pthread_cond_timedwait(&rc->cond, &rc->mutex, &ts);
            if (ret == ETIMEDOUT && !all_results_ready(rc)) {
                fprintf(stderr, "[PID:%d] Timeout waiting for analyzer results\n",
                        getpid());
                pthread_mutex_unlock(&rc->mutex);
                _exit(1);
            } else if (ret != 0 && ret != ETIMEDOUT) {
                errno = ret;
                perror("pthread_cond_timedwait Region C");
                pthread_mutex_unlock(&rc->mutex);
                _exit(1);
            }
        }

        for (int i = 0; i < NUM_LEVELS; i++) {
            if (rc->results[i].ready && !received[i]) {
                received[i] = 1;
                newly_ready[newly_count++] = i;
                received_count++;
            }
        }
        pthread_mutex_unlock(&rc->mutex);

        for (int i = 0; i < newly_count; i++) {
            int level_idx = newly_ready[i];
            if (region_c_sem_wait(rc, level_idx) != 0) {
                perror("sem_wait Region C");
                _exit(1);
            }
            printf("[PID:%d] %s result received.\n",
                   getpid(), LEVEL_NAMES[level_idx]);
        }
    }

    /* Defensive check in case a platform wakes spuriously in an unexpected way. */
    for (int i = 0; i < NUM_LEVELS; i++) {
        if (!received[i]) {
            fprintf(stderr, "[PID:%d] Missing %s result\n",
                    getpid(), LEVEL_NAMES[i]);
            _exit(1);
        }
    }

    printf("[PID:%d] All results received. Writing output files...\n", getpid());

    /* Sort levels by total_weighted_score descending */
    int order[NUM_LEVELS] = {0, 1, 2, 3};
    for (int i = 0; i < NUM_LEVELS - 1; i++) {
        for (int j = i + 1; j < NUM_LEVELS; j++) {
            if (rc->results[order[j]].total_weighted_score >
                rc->results[order[i]].total_weighted_score) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    /* Calculate totals */
    double total_weighted = 0.0;
    for (int i = 0; i < NUM_LEVELS; i++) {
        total_weighted += rc->results[i].total_weighted_score;
    }

    /* ── Write human-readable output file ── */
    FILE *fout = fopen(args->output_file, "w");
    if (!fout) {
        fprintf(stderr, "[PID:%d] Cannot open output file: %s\n",
                getpid(), args->output_file);
        _exit(1);
    }

    /* Header */
    fprintf(fout, "KEYWORD_LIST: ");
    for (int k = 0; k < args->num_keywords; k++) {
        if (k > 0) fprintf(fout, ",");
        fprintf(fout, "%s", args->keywords[k]);
    }
    fprintf(fout, "\n");
    fprintf(fout, "FILES: %d\n", args->num_files);
    fprintf(fout, "TOTAL_WEIGHTED_SCORE: %.1f\n", total_weighted);
    fprintf(fout, "HIGH_PRIORITY_SCORE: %.1f\n", high_priority_score);
    fprintf(fout, "\n");

    /* Level table header */
    fprintf(fout, "# Levels sorted by total_weighted_score DESC\n");
    fprintf(fout, "%-8s  %8s  %15s", "LEVEL", "ENTRIES", "WEIGHTED_SCORE");
    for (int k = 0; k < args->num_keywords; k++) {
        fprintf(fout, "  %8s", args->keywords[k]);
    }
    fprintf(fout, "\n");

    /* Level rows */
    for (int i = 0; i < NUM_LEVELS; i++) {
        int idx = order[i];
        level_result_t *r = &rc->results[idx];
        fprintf(fout, "%-8s  %8ld  %15.1f", r->level, r->total_entries,
                r->total_weighted_score);
        for (int k = 0; k < args->num_keywords; k++) {
            fprintf(fout, "  %8.1f", r->per_keyword_score[k]);
        }
        fprintf(fout, "\n");
    }
    fprintf(fout, "\n");

    /* Top-3 sources per level */
    fprintf(fout, "# Top-3 sources per level\n");
    for (int i = 0; i < NUM_LEVELS; i++) {
        int idx = order[i];
        level_result_t *r = &rc->results[idx];
        fprintf(fout, "%-8s", r->level);
        for (int t = 0; t < 3; t++) {
            if (r->top_source[t][0] != '\0') {
                fprintf(fout, "  %s:%ld", r->top_source[t], r->top_source_hits[t]);
            }
        }
        fprintf(fout, "\n");
    }
    fprintf(fout, "\n");

    /* Per-thread contributions */
    fprintf(fout, "# Per-thread contributions (weighted score)\n");
    for (int i = 0; i < NUM_LEVELS; i++) {
        int idx = order[i];
        level_result_t *r = &rc->results[idx];
        fprintf(fout, "%-8s", r->level);
        for (int w = 0; w < args->num_workers; w++) {
            fprintf(fout, "  thread_%d:%.1f", w, r->per_thread_score[w]);
        }
        fprintf(fout, "\n");
    }

    fclose(fout);

    /* ── Write binary checkpoint file with atomic rename ── */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", args->binary_file);

    FILE *fbin = fopen(tmp_path, "wb");
    if (!fbin) {
        fprintf(stderr, "[PID:%d] Cannot open binary file: %s\n", getpid(), tmp_path);
        _exit(1);
    }

    /* Binary header */
    binary_header_t header;
    header.magic = BINARY_MAGIC;
    header.version = BINARY_VERSION;
    header.num_levels = NUM_LEVELS;
    header.num_keywords = (uint32_t)args->num_keywords;
    header.total_weighted = total_weighted;
    header.high_priority_weighted = high_priority_score;

    if (fwrite(&header, sizeof(header), 1, fbin) != 1) {
        fprintf(stderr, "[PID:%d] Partial write on binary header\n", getpid());
        fclose(fbin);
        _exit(1);
    }

    /* Level results (in Region C order) */
    for (int i = 0; i < NUM_LEVELS; i++) {
        if (fwrite(&rc->results[i], sizeof(level_result_t), 1, fbin) != 1) {
            fprintf(stderr, "[PID:%d] Partial write on level %d\n", getpid(), i);
            fclose(fbin);
            _exit(1);
        }
    }

    fclose(fbin);

    /* Atomic rename */
    if (rename(tmp_path, args->binary_file) != 0) {
        perror("rename binary file");
        _exit(1);
    }

    printf("[PID:%d] Output files written: %s, %s\n",
           getpid(), args->output_file, args->binary_file);
    printf("[PID:%d] Aggregator exiting.\n", getpid());
}
