/*
 * CSE344 HW4 - Reader Process Implementation
 *
 * Each Reader Process:
 * - Spawns T reader threads that divide the file into byte ranges
 * - Reader threads push parsed log_entry_t into an internal bounded buffer
 *   using pthread_mutex_t + pthread_cond_t (NOT semaphores)
 * - A parser thread consumes from internal buffer and pushes to Region A
 * - Heartbeat messages written to pipe every 50 lines
 * - After all reader threads finish, parser sends 4 EOF markers to Region A
 */

#define _GNU_SOURCE
#include "reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include "mac_compat.h"

/* ─── Internal Bounded Buffer ───────────────────────────────── */
#define INTERNAL_BUF_CAP 256

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
    int             capacity;
    int             head;
    int             tail;
    int             count;
    int             done;          /* set when all reader threads finish */
    log_entry_t     entries[INTERNAL_BUF_CAP];
} internal_buffer_t;

/* ─── Thread argument for reader threads ────────────────────── */
typedef struct {
    int               thread_idx;
    int               reader_index;
    const char       *filename;
    long              start_byte;
    long              end_byte;
    internal_buffer_t *buf;
    int               pipe_fd;
} reader_thread_arg_t;

/* ─── Thread argument for parser thread ─────────────────────── */
typedef struct {
    internal_buffer_t *buf;
    region_a_t        *region_a;
    int                reader_index;
    int                num_reader_threads;
} parser_thread_arg_t;

/* Global counters for dispatched entries per level (used by parser thread) */
static int g_dispatched[NUM_LEVELS];

/* ─── Parse a single log line ───────────────────────────────── */
static int parse_log_line(const char *line, log_entry_t *entry) {
    memset(entry, 0, sizeof(log_entry_t));
    entry->is_eof = 0;

    /* Expected format: [YYYY-MM-DD HH:MM:SS] [LEVEL] [SOURCE] MESSAGE */
    if (line[0] != '[') return -1;

    /* Parse timestamp */
    const char *ts_end = strchr(line + 1, ']');
    if (!ts_end) return -1;
    int ts_len = (int)(ts_end - line - 1);
    if (ts_len <= 0 || ts_len >= MAX_TIMESTAMP) return -1;
    strncpy(entry->timestamp, line + 1, ts_len);
    entry->timestamp[ts_len] = '\0';

    /* Skip to LEVEL */
    const char *p = ts_end + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '[') return -1;
    p++;
    const char *level_end = strchr(p, ']');
    if (!level_end) return -1;

    /* Extract level string (trim spaces) */
    char level_str[16] = {0};
    int level_len = (int)(level_end - p);
    if (level_len <= 0 || level_len >= 16) return -1;
    /* Copy and trim trailing spaces */
    strncpy(level_str, p, level_len);
    level_str[level_len] = '\0';
    /* Trim trailing spaces */
    for (int i = level_len - 1; i >= 0 && level_str[i] == ' '; i--)
        level_str[i] = '\0';

    /* Map level to index */
    if (strcmp(level_str, "ERROR") == 0)      entry->level_idx = LEVEL_ERROR;
    else if (strcmp(level_str, "WARN") == 0)  entry->level_idx = LEVEL_WARN;
    else if (strcmp(level_str, "INFO") == 0)  entry->level_idx = LEVEL_INFO;
    else if (strcmp(level_str, "DEBUG") == 0) entry->level_idx = LEVEL_DEBUG;
    else return -1; /* unknown level, skip */

    /* Skip to SOURCE */
    p = level_end + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '[') return -1;
    p++;
    const char *src_end = strchr(p, ']');
    if (!src_end) return -1;

    int src_len = (int)(src_end - p);
    if (src_len <= 0 || src_len >= MAX_SOURCE) return -1;
    strncpy(entry->source, p, src_len);
    entry->source[src_len] = '\0';
    /* Trim trailing spaces from source */
    for (int i = src_len - 1; i >= 0 && entry->source[i] == ' '; i--)
        entry->source[i] = '\0';

    /* Rest is MESSAGE — skip leading whitespace */
    p = src_end + 1;
    while (*p == ' ' || *p == '\t') p++;
    strncpy(entry->message, p, MAX_MSG - 1);
    /* Remove trailing newline */
    int msg_len = (int)strlen(entry->message);
    while (msg_len > 0 && (entry->message[msg_len - 1] == '\n' ||
                           entry->message[msg_len - 1] == '\r')) {
        entry->message[--msg_len] = '\0';
    }

    return 0;
}

/* ─── Internal buffer push ──────────────────────────────────── */
static void ibuf_push(internal_buffer_t *buf, const log_entry_t *entry) {
    pthread_mutex_lock(&buf->mutex);
    while (buf->count >= buf->capacity) {
        pthread_cond_wait(&buf->not_full, &buf->mutex);
    }
    buf->entries[buf->tail] = *entry;
    buf->tail = (buf->tail + 1) % buf->capacity;
    buf->count++;
    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
}

/* ─── Internal buffer pop (returns -1 if done and empty) ────── */
static int ibuf_pop(internal_buffer_t *buf, log_entry_t *entry) {
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == 0) {
        if (buf->done) {
            pthread_mutex_unlock(&buf->mutex);
            return -1; /* no more data */
        }
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    }
    *entry = buf->entries[buf->head];
    buf->head = (buf->head + 1) % buf->capacity;
    buf->count--;
    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* ─── Reader thread function ────────────────────────────────── */
static void *reader_thread_func(void *arg) {
    reader_thread_arg_t *rta = (reader_thread_arg_t *)arg;
    pid_t tid = GET_TID;

    printf("[PID:%d][TID:%d] Reader thread %d: range [%ld, %ld) bytes\n",
           getpid(), tid, rta->thread_idx, rta->start_byte, rta->end_byte);

    FILE *fp = fopen(rta->filename, "r");
    if (!fp) {
        fprintf(stderr, "[PID:%d] Cannot open file: %s\n", getpid(), rta->filename);
        return NULL;
    }

    /* Seek to start position. If the boundary is in the middle of a line,
     * skip that partial line; if it is exactly at a line boundary, read it. */
    fseek(fp, rta->start_byte, SEEK_SET);
    if (rta->start_byte > 0) {
        if (fseek(fp, rta->start_byte - 1, SEEK_SET) == 0) {
            int prev = fgetc(fp);
            if (prev != '\n') {
                int ch;
                while ((ch = fgetc(fp)) != EOF && ch != '\n')
                    ;
            } else {
                fseek(fp, rta->start_byte, SEEK_SET);
            }
        }
    }

    long current_pos = ftell(fp);
    int lines_read = 0;
    int malformed = 0;
    char line[MAX_LINE];

    while (current_pos < rta->end_byte && fgets(line, sizeof(line), fp) != NULL) {
        current_pos = ftell(fp);

        /* Skip blank lines */
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
            continue;

        log_entry_t entry;
        if (parse_log_line(line, &entry) == 0) {
            ibuf_push(rta->buf, &entry);
            lines_read++;
        } else {
            malformed++;
        }

        /* Heartbeat every 50 lines */
        if (lines_read > 0 && lines_read % 50 == 0) {
            char hb[128];
            int n = snprintf(hb, sizeof(hb), "[R%d] %d lines processed\n",
                             rta->reader_index, lines_read);
            if (rta->pipe_fd >= 0 && n > 0) {
                (void)write(rta->pipe_fd, hb, n);
            }
        }
    }

    /* Final heartbeat */
    if (lines_read % 50 != 0) {
        char hb[128];
        int n = snprintf(hb, sizeof(hb), "[R%d] %d lines processed\n",
                         rta->reader_index, lines_read);
        if (rta->pipe_fd >= 0 && n > 0) {
            (void)write(rta->pipe_fd, hb, n);
        }
    }

    fclose(fp);
    printf("[PID:%d][TID:%d] Reader thread %d: finished, lines_read=%d, malformed=%d\n",
           getpid(), tid, rta->thread_idx, lines_read, malformed);

    return NULL;
}

/* ─── Parser thread function ────────────────────────────────── */
static void *parser_thread_func(void *arg) {
    parser_thread_arg_t *pta = (parser_thread_arg_t *)arg;
    int counts[NUM_LEVELS] = {0};
    log_entry_t entry;

    while (ibuf_pop(pta->buf, &entry) == 0) {
        counts[entry.level_idx]++;
        region_a_push(pta->region_a, &entry);
    }

    /* Send one EOF marker per severity level to Region A */
    for (int i = 0; i < NUM_LEVELS; i++) {
        log_entry_t eof;
        memset(&eof, 0, sizeof(eof));
        eof.is_eof = 1;
        eof.eof_level = i;
        region_a_push(pta->region_a, &eof);
    }

    /* Store for summary */
    for (int i = 0; i < NUM_LEVELS; i++)
        g_dispatched[i] = counts[i];

    printf("[PID:%d] Parser thread: dispatched E:%d W:%d I:%d D:%d -> Region A\n",
           getpid(), counts[LEVEL_ERROR], counts[LEVEL_WARN],
           counts[LEVEL_INFO], counts[LEVEL_DEBUG]);

    return NULL;
}

/* ─── Reader Process Entry Point ────────────────────────────── */
void reader_run(const reader_args_t *args) {
    printf("[PID:%d] Reader %d started. File: %s, Threads: %d\n",
           getpid(), args->reader_index, args->filename, args->num_threads);

    /* Get file size */
    struct stat st;
    if (stat(args->filename, &st) != 0) {
        fprintf(stderr, "[PID:%d] Cannot stat file: %s\n", getpid(), args->filename);
        _exit(1);
    }
    long file_size = st.st_size;

    /* Initialize internal buffer */
    internal_buffer_t ibuf;
    memset(&ibuf, 0, sizeof(ibuf));
    ibuf.capacity = INTERNAL_BUF_CAP;
    pthread_mutex_init(&ibuf.mutex, NULL);
    pthread_cond_init(&ibuf.not_full, NULL);
    pthread_cond_init(&ibuf.not_empty, NULL);

    /* Reset globals */
    memset(g_dispatched, 0, sizeof(g_dispatched));

    int T = args->num_threads;
    long chunk_size = file_size / T;

    /* Create reader threads */
    pthread_t *reader_tids = malloc(T * sizeof(pthread_t));
    reader_thread_arg_t *rta = malloc(T * sizeof(reader_thread_arg_t));
    if (!reader_tids || !rta) {
        fprintf(stderr, "[PID:%d] malloc failed\n", getpid());
        _exit(1);
    }

    for (int i = 0; i < T; i++) {
        rta[i].thread_idx = i;
        rta[i].reader_index = args->reader_index;
        rta[i].filename = args->filename;
        rta[i].start_byte = i * chunk_size;
        rta[i].end_byte = (i == T - 1) ? file_size : (i + 1) * chunk_size;
        rta[i].buf = &ibuf;
        rta[i].pipe_fd = args->pipe_fd;

        if (pthread_create(&reader_tids[i], NULL, reader_thread_func, &rta[i]) != 0) {
            fprintf(stderr, "[PID:%d] pthread_create reader thread %d failed\n", getpid(), i);
            _exit(1);
        }
    }

    /* Create parser thread */
    pthread_t parser_tid;
    parser_thread_arg_t pta;
    pta.buf = &ibuf;
    pta.region_a = args->region_a;
    pta.reader_index = args->reader_index;
    pta.num_reader_threads = T;

    if (pthread_create(&parser_tid, NULL, parser_thread_func, &pta) != 0) {
        fprintf(stderr, "[PID:%d] pthread_create parser thread failed\n", getpid());
        _exit(1);
    }

    /* Wait for all reader threads */
    for (int i = 0; i < T; i++) {
        pthread_join(reader_tids[i], NULL);
    }

    /* Signal parser that all reader threads are done */
    pthread_mutex_lock(&ibuf.mutex);
    ibuf.done = 1;
    pthread_cond_broadcast(&ibuf.not_empty);
    pthread_mutex_unlock(&ibuf.mutex);

    /* Wait for parser thread */
    pthread_join(parser_tid, NULL);

    /* Cleanup */
    pthread_mutex_destroy(&ibuf.mutex);
    pthread_cond_destroy(&ibuf.not_full);
    pthread_cond_destroy(&ibuf.not_empty);
    free(reader_tids);
    free(rta);

    /* Close pipe write end */
    if (args->pipe_fd >= 0)
        close(args->pipe_fd);

    printf("[PID:%d] Reader %d exiting.\n", getpid(), args->reader_index);
}
