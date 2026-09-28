/*
 * CSE344 HW4 - Analyzer Process Implementation
 *
 * Each Analyzer Process (one per severity level):
 * - Spawns W worker threads consuming from Region B[level_idx]
 * - Workers use pthread_key_t TLS for per-keyword score accumulation
 * - TLS destructor flushes scores to Region C under process-shared mutex
 * - pthread_barrier_wait after all entries consumed
 * - Reporting thread (lowest gettid()) writes level summary to Region C
 *   and posts the semaphore
 * - Overlapping keyword search with sliding window
 * - Per-source top-3 tracking
 */

#define _GNU_SOURCE
#include "analyzer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "mac_compat.h"

/* ─── Shared state within this Analyzer process ─────────────── */
typedef struct {
    const analyzer_args_t *args;
    pthread_barrier_t      barrier;
    pthread_key_t          tls_key;
    pthread_mutex_t        source_mutex;  /* protects source_map */
    pthread_mutex_t        flush_mutex;   /* protects TLS destructor progress */
    pthread_cond_t         flush_cond;
    int                    flush_count;

    /* Per-source hit tracking */
    char   source_names[1024][MAX_SOURCE];
    long   source_hits[1024];
    int    num_sources;

    /* Worker TID tracking for reporting thread selection */
    pid_t  worker_tids[MAX_WORKERS];
    int    worker_entry_counts[MAX_WORKERS];
    double worker_scores[MAX_WORKERS];
    double logical_thread_scores[MAX_WORKERS];
} analyzer_shared_t;

static analyzer_shared_t *g_shared;

/* ─── TLS data: per-keyword scores for this thread ──────────── */
typedef struct {
    double scores[MAX_KEYWORDS];
    int    worker_idx;
    int    is_reporter;
    pid_t  tid;
    double total_score;
} tls_data_t;

/* ─── Count overlapping occurrences of needle in haystack ───── */
static int count_overlapping(const char *haystack, const char *needle) {
    int count = 0;
    int nlen = (int)strlen(needle);
    if (nlen == 0) return 0;

    for (int i = 0; haystack[i] != '\0'; i++) {
        if (strncmp(&haystack[i], needle, nlen) == 0) {
            count++;
        }
    }
    return count;
}

/* ─── Stable hash for deterministic per-thread output buckets ─ */
static unsigned long hash_string(unsigned long h, const char *s) {
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211UL;
    }
    return h;
}

static int logical_thread_bucket(const log_entry_t *entry, int num_workers) {
    unsigned long h = 1469598103934665603UL;
    h = hash_string(h, entry->timestamp);
    h = hash_string(h, LEVEL_NAMES[entry->level_idx]);
    h = hash_string(h, entry->source);
    h = hash_string(h, entry->message);
    return (int)(h % (unsigned long)num_workers);
}

/* ─── Add source hit ────────────────────────────────────────── */
static void add_source_hit(const char *source, long hits) {
    pthread_mutex_lock(&g_shared->source_mutex);
    for (int i = 0; i < g_shared->num_sources; i++) {
        if (strcmp(g_shared->source_names[i], source) == 0) {
            g_shared->source_hits[i] += hits;
            pthread_mutex_unlock(&g_shared->source_mutex);
            return;
        }
    }
    /* New source */
    if (g_shared->num_sources < 1024) {
        strncpy(g_shared->source_names[g_shared->num_sources], source, MAX_SOURCE - 1);
        g_shared->source_hits[g_shared->num_sources] = hits;
        g_shared->num_sources++;
    }
    pthread_mutex_unlock(&g_shared->source_mutex);
}

/* ─── Publish final level summary after every TLS flush ─────── */
static void publish_level_result(pid_t reporter_tid) {
    const analyzer_args_t *args = g_shared->args;
    region_c_t *rc = args->region_c;
    int level_idx = args->level_idx;

    long total_entries = 0;
    double total_weighted = 0.0;
    for (int i = 0; i < args->num_workers; i++) {
        total_entries += g_shared->worker_entry_counts[i];
        total_weighted += g_shared->worker_scores[i];
    }

    pthread_mutex_lock(&rc->mutex);
    rc->results[level_idx].total_entries = total_entries;
    strncpy(rc->results[level_idx].level, LEVEL_NAMES[level_idx],
            sizeof(rc->results[level_idx].level) - 1);
    rc->results[level_idx].level[sizeof(rc->results[level_idx].level) - 1] = '\0';

    /* Compute deterministic top-3 sources: hits desc, then source asc. */
    for (int t = 0; t < 3 && t < g_shared->num_sources; t++) {
        int max_idx = -1;
        for (int s = 0; s < g_shared->num_sources; s++) {
            int used = 0;
            for (int u = 0; u < t; u++) {
                if (strcmp(g_shared->source_names[s],
                           rc->results[level_idx].top_source[u]) == 0) {
                    used = 1;
                    break;
                }
            }
            if (used)
                continue;

            if (max_idx < 0 ||
                g_shared->source_hits[s] > g_shared->source_hits[max_idx] ||
                (g_shared->source_hits[s] == g_shared->source_hits[max_idx] &&
                 strcmp(g_shared->source_names[s],
                        g_shared->source_names[max_idx]) < 0)) {
                max_idx = s;
            }
        }

        if (max_idx >= 0) {
            strncpy(rc->results[level_idx].top_source[t],
                    g_shared->source_names[max_idx],
                    sizeof(rc->results[level_idx].top_source[t]) - 1);
            rc->results[level_idx].top_source[t]
                [sizeof(rc->results[level_idx].top_source[t]) - 1] = '\0';
            rc->results[level_idx].top_source_hits[t] =
                g_shared->source_hits[max_idx];
        }
    }

    for (int w = 0; w < args->num_workers; w++) {
        rc->results[level_idx].per_thread_score[w] =
            g_shared->logical_thread_scores[w];
    }

    rc->results[level_idx].ready = 1;
    pthread_cond_broadcast(&rc->cond);
    pthread_mutex_unlock(&rc->mutex);

    if (region_c_sem_post(rc, level_idx) != 0) {
        perror("sem_post Region C");
    }

    printf("[PID:%d][TID:%d]    Total entries: %ld | Total weighted score: %.1f\n",
           getpid(), reporter_tid, total_entries, total_weighted);
}

/* ─── TLS destructor: flush scores to Region C ──────────────── */
static void tls_destructor(void *data) {
    if (!data) return;
    tls_data_t *tls = (tls_data_t *)data;
    int is_reporter = tls->is_reporter;
    pid_t reporter_tid = tls->tid;

    /* Flush to Region C under process-shared mutex */
    region_c_t *rc = g_shared->args->region_c;
    int level_idx = g_shared->args->level_idx;

    pthread_mutex_lock(&rc->mutex);
    for (int k = 0; k < g_shared->args->num_keywords; k++) {
        rc->results[level_idx].per_keyword_score[k] += tls->scores[k];
    }
    rc->results[level_idx].total_weighted_score += tls->total_score;
    pthread_mutex_unlock(&rc->mutex);

    pthread_mutex_lock(&g_shared->flush_mutex);
    g_shared->flush_count++;
    int all_flushed = (g_shared->flush_count == g_shared->args->num_workers);
    pthread_cond_broadcast(&g_shared->flush_cond);
    pthread_mutex_unlock(&g_shared->flush_mutex);

    if (is_reporter && all_flushed) {
        publish_level_result(reporter_tid);
    }

    free(tls);
}

/* ─── Worker thread function ────────────────────────────────── */
typedef struct {
    int worker_idx;
} worker_arg_t;

static void *worker_thread_func(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    int worker_idx = wa->worker_idx;
    pid_t tid = GET_TID;
    g_shared->worker_tids[worker_idx] = tid;

    printf("[PID:%d][TID:%d] Worker %d started.\n", getpid(), tid, worker_idx);

    /* Initialize TLS */
    tls_data_t *tls = calloc(1, sizeof(tls_data_t));
    if (!tls) {
        fprintf(stderr, "[PID:%d] calloc TLS failed\n", getpid());
        pthread_exit(NULL);
    }
    tls->worker_idx = worker_idx;
    tls->tid = tid;
    pthread_setspecific(g_shared->tls_key, tls);

    const analyzer_args_t *args = g_shared->args;
    int entry_count = 0;

    /* Consume entries from Region B */
    while (1) {
        log_entry_t entry;
        int ret = level_buffer_pop_for_worker(args->level_buffer, &entry,
                                              worker_idx, args->num_workers);
        if (ret == -1) break; /* EOF or done */

        entry_count++;

        /* Search for keywords with overlapping matching */
        int total_hits = 0;
        double entry_weighted = 0.0;
        for (int k = 0; k < args->num_keywords; k++) {
            int count = count_overlapping(entry.message, args->keywords[k]);
            if (count > 0) {
                double weighted = (double)count * LEVEL_WEIGHT[args->level_idx];
                tls->scores[k] += weighted;
                tls->total_score += weighted;
                entry_weighted += weighted;
                total_hits += count;
            }
        }

        /* Track source hits */
        if (total_hits > 0) {
            add_source_hit(entry.source, total_hits);
        }

        if (entry_weighted > 0.0) {
            int bucket = logical_thread_bucket(&entry, args->num_workers);
            pthread_mutex_lock(&g_shared->source_mutex);
            g_shared->logical_thread_scores[bucket] += entry_weighted;
            pthread_mutex_unlock(&g_shared->source_mutex);
        }
    }

    g_shared->worker_entry_counts[worker_idx] = entry_count;
    g_shared->worker_scores[worker_idx] = tls->total_score;

    printf("[PID:%d][TID:%d] Worker %d done. Entries: %d, Weighted score: %.1f\n",
           getpid(), tid, worker_idx, entry_count, tls->total_score);

    /* Barrier: wait for all workers */
    pthread_barrier_wait(&g_shared->barrier);

    /* After barrier, find the thread with the lowest TID */
    pid_t min_tid = g_shared->worker_tids[0];
    for (int i = 1; i < args->num_workers; i++) {
        if (g_shared->worker_tids[i] < min_tid)
            min_tid = g_shared->worker_tids[i];
    }

    if (tid == min_tid) {
        tls_data_t *report_tls = pthread_getspecific(g_shared->tls_key);
        if (report_tls)
            report_tls->is_reporter = 1;

        printf("[PID:%d][TID:%d] ** Reporting thread (lowest TID). Level: %s **\n",
               getpid(), tid, LEVEL_NAMES[args->level_idx]);

        /* Non-reporting workers exit first so their TLS destructors flush.
         * This reporting worker exits last; its destructor publishes Region C. */
        pthread_mutex_lock(&g_shared->flush_mutex);
        while (g_shared->flush_count < args->num_workers - 1) {
            pthread_cond_wait(&g_shared->flush_cond, &g_shared->flush_mutex);
        }
        pthread_mutex_unlock(&g_shared->flush_mutex);
    }

    /* Thread exits -> TLS destructor fires -> flushes scores to Region C */
    return NULL;
}

/* ─── Analyzer Process Entry Point ──────────────────────────── */
void analyzer_run(const analyzer_args_t *args) {
    printf("[PID:%d] Analyzer %s started. Workers: %d\n",
           getpid(), LEVEL_NAMES[args->level_idx], args->num_workers);

    /* Allocate shared state */
    g_shared = calloc(1, sizeof(analyzer_shared_t));
    if (!g_shared) {
        fprintf(stderr, "[PID:%d] calloc analyzer shared failed\n", getpid());
        _exit(1);
    }
    g_shared->args = args;
    pthread_mutex_init(&g_shared->source_mutex, NULL);
    pthread_mutex_init(&g_shared->flush_mutex, NULL);
    pthread_cond_init(&g_shared->flush_cond, NULL);

    /* Create TLS key with destructor */
    if (pthread_key_create(&g_shared->tls_key, tls_destructor) != 0) {
        fprintf(stderr, "[PID:%d] pthread_key_create failed\n", getpid());
        _exit(1);
    }

    /* Create barrier for num_workers */
    if (pthread_barrier_init(&g_shared->barrier, NULL, args->num_workers) != 0) {
        fprintf(stderr, "[PID:%d] pthread_barrier_init failed\n", getpid());
        _exit(1);
    }

    /* Create worker threads */
    int W = args->num_workers;
    pthread_t *threads = malloc(W * sizeof(pthread_t));
    worker_arg_t *wargs = malloc(W * sizeof(worker_arg_t));
    if (!threads || !wargs) {
        fprintf(stderr, "[PID:%d] malloc failed\n", getpid());
        _exit(1);
    }

    for (int i = 0; i < W; i++) {
        wargs[i].worker_idx = i;
        if (pthread_create(&threads[i], NULL, worker_thread_func, &wargs[i]) != 0) {
            fprintf(stderr, "[PID:%d] pthread_create worker %d failed\n", getpid(), i);
            _exit(1);
        }
    }

    /* Wait for all worker threads */
    for (int i = 0; i < W; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Cleanup */
    pthread_barrier_destroy(&g_shared->barrier);
    pthread_key_delete(g_shared->tls_key);
    pthread_mutex_destroy(&g_shared->source_mutex);
    pthread_mutex_destroy(&g_shared->flush_mutex);
    pthread_cond_destroy(&g_shared->flush_cond);
    free(threads);
    free(wargs);
    free(g_shared);

    printf("[PID:%d] Analyzer %s exiting.\n", getpid(), LEVEL_NAMES[args->level_idx]);
}
