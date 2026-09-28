/*
 * CSE344 HW4 - Parent / Coordinator Process
 *
 * - Parses command-line args
 * - Reads config file (log file list) and filter file (priority sources)
 * - Initializes all shared memory regions
 * - Creates pipes for each Reader
 * - Forks: Readers -> Dispatcher -> 4 Analyzers -> Aggregator
 * - Starts Watchdog thread
 * - waitpid loop
 * - SIGINT handler (async-signal-safe)
 * - Final summary and cleanup
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>

#include "shm.h"
#include "reader.h"
#include "dispatcher.h"
#include "analyzer.h"
#include "aggregator.h"
#include "watchdog.h"

/* ─── Globals ───────────────────────────────────────────────── */
static volatile sig_atomic_t g_sigint_received = 0;
static volatile sig_atomic_t g_shutdown_watchdog = 0;

static pid_t g_child_pids[128];
static int   g_num_children = 0;
static shm_context_t g_shm_ctx;

/* ─── SIGINT handler (async-signal-safe only) ───────────────── */
static void sigint_handler(int sig) {
    (void)sig;
    g_sigint_received = 1;
    const char msg[] = "\n[SIGINT] Caught Ctrl+C. Shutting down...\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
}

/* ─── Read lines from a file into array ─────────────────────── */
static int read_lines(const char *path, char lines[][256], int max_lines) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open file: %s\n", path);
        return -1;
    }
    int count = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f) && count < max_lines) {
        /* Remove trailing newline */
        int len = (int)strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        if (len > 0) {
            strncpy(lines[count], buf, 255);
            lines[count][255] = '\0';
            count++;
        }
    }
    fclose(f);
    return count;
}

/* ─── Parse comma-separated keywords ────────────────────────── */
static int parse_keywords(const char *str, char keywords[][MAX_SOURCE], int max_kw) {
    int count = 0;
    char tmp[1024];
    strncpy(tmp, str, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *tok = strtok(tmp, ",");
    while (tok) {
        size_t len = strlen(tok);
        if (len == 0 || len >= MAX_SOURCE)
            return -1;
        if (tok[0] == ' ' || tok[0] == '\t' ||
            tok[len - 1] == ' ' || tok[len - 1] == '\t')
            return -1;
        if (count >= max_kw)
            return -1;
        strncpy(keywords[count], tok, MAX_SOURCE - 1);
        keywords[count][MAX_SOURCE - 1] = '\0';
        count++;
        tok = strtok(NULL, ",");
    }
    return count;
}

/* ─── Main ──────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    /* Default values */
    char config_file[256] = {0};
    char filter_file[256] = {0};
    char keyword_str[1024] = {0};
    int  num_reader_threads = 0;
    int  num_worker_threads = 0;
    int  capacity_a = 0;
    int  capacity_b = 0;
    int  capacity_d = 0;
    int  timeout_sec = 10;
    char output_file[256] = {0};
    char binary_file[256] = {0};

    /* Parse command-line arguments */
    int opt;
    while ((opt = getopt(argc, argv, "c:f:k:t:w:a:b:d:T:o:O:")) != -1) {
        switch (opt) {
            case 'c': strncpy(config_file, optarg, 255); break;
            case 'f': strncpy(filter_file, optarg, 255); break;
            case 'k': strncpy(keyword_str, optarg, 1023); break;
            case 't': num_reader_threads = atoi(optarg); break;
            case 'w': num_worker_threads = atoi(optarg); break;
            case 'a': capacity_a = atoi(optarg); break;
            case 'b': capacity_b = atoi(optarg); break;
            case 'd': capacity_d = atoi(optarg); break;
            case 'T': timeout_sec = atoi(optarg); break;
            case 'o': strncpy(output_file, optarg, 255); break;
            case 'O': strncpy(binary_file, optarg, 255); break;
            default:
                fprintf(stderr, "Usage: %s -c <config> -f <filter> -k <keywords> "
                        "-t <threads> -w <workers> -a <capA> -b <capB> -d <capD> "
                        "[-T <timeout>] -o <output> -O <binary>\n", argv[0]);
                exit(1);
        }
    }

    /* Validate parameters */
    if (config_file[0] == '\0') { fprintf(stderr, "Error: -c config_file required\n"); exit(1); }
    if (filter_file[0] == '\0') { fprintf(stderr, "Error: -f filter_file required\n"); exit(1); }
    if (keyword_str[0] == '\0') { fprintf(stderr, "Error: -k keywords required\n"); exit(1); }
    if (num_reader_threads < 1) { fprintf(stderr, "Error: -t must be >= 1\n"); exit(1); }
    if (num_worker_threads < 1 || num_worker_threads > MAX_WORKERS) {
        fprintf(stderr, "Error: -w must be 1..%d\n", MAX_WORKERS); exit(1);
    }
    if (capacity_a < 4) { fprintf(stderr, "Error: -a must be >= 4\n"); exit(1); }
    if (capacity_b < 4) { fprintf(stderr, "Error: -b must be >= 4\n"); exit(1); }
    if (capacity_d < 2) { fprintf(stderr, "Error: -d must be >= 2\n"); exit(1); }
    if (timeout_sec < 1) { fprintf(stderr, "Error: -T must be >= 1\n"); exit(1); }
    if (output_file[0] == '\0') { fprintf(stderr, "Error: -o output_file required\n"); exit(1); }
    if (binary_file[0] == '\0') { fprintf(stderr, "Error: -O binary_file required\n"); exit(1); }

    /* Parse keywords */
    char keywords[MAX_KEYWORDS][MAX_SOURCE];
    int num_keywords = parse_keywords(keyword_str, keywords, MAX_KEYWORDS);
    if (num_keywords < 1) {
        fprintf(stderr, "Error: keywords must be 1..%d non-empty values with no spaces around commas\n",
                MAX_KEYWORDS);
        exit(1);
    }

    /* Read config file (log file paths) */
    char log_files[MAX_FILES][256];
    int num_files = read_lines(config_file, log_files, MAX_FILES);
    if (num_files < 0) {
        fprintf(stderr, "Error: cannot read config file: %s\n", config_file);
        exit(1);
    }
    if (num_files < 1) { fprintf(stderr, "Error: no log files in config\n"); exit(1); }

    for (int i = 0; i < num_files; i++) {
        if (access(log_files[i], R_OK) != 0) {
            fprintf(stderr, "Error: log file is not readable: %s\n", log_files[i]);
            exit(1);
        }
    }

    /* Read filter file (priority sources) */
    char priority_sources[MAX_FILES][256];
    int num_priority = read_lines(filter_file, priority_sources, MAX_FILES);
    if (num_priority < 0) {
        fprintf(stderr, "Error: cannot read filter file: %s\n", filter_file);
        exit(1);
    }

    /* Print startup */
    printf("[PID:%d] Parent started. Files: %d, Keywords: %s\n",
           getpid(), num_files, keyword_str);

    /* Initialize shared memory */
    if (shm_init(&g_shm_ctx, capacity_a, capacity_b, capacity_d,
                 num_files, num_keywords, (const char (*)[MAX_SOURCE])keywords,
                 num_worker_threads) != 0) {
        fprintf(stderr, "Failed to initialize shared memory\n");
        exit(1);
    }

    printf("[PID:%d] Shared memory initialized (A:%d B:%dx%d D:%d).\n",
           getpid(), capacity_a, capacity_b, NUM_LEVELS, capacity_d);

    /* Install SIGINT handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        perror("sigaction");
        exit(1);
    }

    /* Create pipes for each Reader Process */
    int pipe_read_fds[MAX_FILES];
    int pipe_write_fds[MAX_FILES];
    for (int i = 0; i < num_files; i++) {
        int pfd[2];
        if (pipe(pfd) != 0) {
            perror("pipe");
            exit(1);
        }
        pipe_read_fds[i] = pfd[0];
        pipe_write_fds[i] = pfd[1];
    }

    /* ── Fork Reader Processes ── */
    for (int i = 0; i < num_files; i++) {
        printf("[PID:%d] Forking Reader %d -> %s\n", getpid(), i, log_files[i]);
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork reader");
            exit(1);
        }
        if (pid == 0) {
            /* Child: close all pipe read-ends and other write-ends */
            for (int j = 0; j < num_files; j++) {
                close(pipe_read_fds[j]);
                if (j != i) close(pipe_write_fds[j]);
            }

            reader_args_t ra;
            ra.reader_index = i;
            ra.filename = log_files[i];
            ra.num_threads = num_reader_threads;
            ra.region_a = g_shm_ctx.region_a;
            ra.pipe_fd = pipe_write_fds[i];

            reader_run(&ra);
            _exit(0);
        }
        g_child_pids[g_num_children++] = pid;
        close(pipe_write_fds[i]); /* Parent closes write-end */
    }

    /* ── Fork Dispatcher Process ── */
    printf("[PID:%d] Forking Dispatcher\n", getpid());
    {
        pid_t pid = fork();
        if (pid < 0) { perror("fork dispatcher"); exit(1); }
        if (pid == 0) {
            /* Close all pipe fds in child */
            for (int j = 0; j < num_files; j++)
                close(pipe_read_fds[j]);

            dispatcher_args_t da;
            da.region_a = g_shm_ctx.region_a;
            for (int i = 0; i < NUM_LEVELS; i++)
                da.region_b[i] = g_shm_ctx.region_b[i];
            da.region_d = g_shm_ctx.region_d;
            da.total_readers = num_files;
            da.timeout_sec = timeout_sec;

            /* Build priority source pointer array */
            char **psrc = malloc(num_priority * sizeof(char *));
            for (int i = 0; i < num_priority; i++)
                psrc[i] = priority_sources[i];
            da.priority_sources = psrc;
            da.num_priority = num_priority;

            dispatcher_run(&da);
            free(psrc);
            _exit(0);
        }
        g_child_pids[g_num_children++] = pid;
    }

    /* ── Fork 4 Analyzer Processes ── */
    for (int lvl = 0; lvl < NUM_LEVELS; lvl++) {
        printf("[PID:%d] Forking Analyzer %s (index %d)\n",
               getpid(), LEVEL_NAMES[lvl], lvl);
        pid_t pid = fork();
        if (pid < 0) { perror("fork analyzer"); exit(1); }
        if (pid == 0) {
            for (int j = 0; j < num_files; j++)
                close(pipe_read_fds[j]);

            analyzer_args_t aa;
            aa.level_idx = lvl;
            aa.num_workers = num_worker_threads;
            aa.num_keywords = num_keywords;
            for (int k = 0; k < num_keywords; k++)
                strncpy(aa.keywords[k], keywords[k], MAX_SOURCE - 1);
            aa.level_buffer = g_shm_ctx.region_b[lvl];
            aa.region_c = g_shm_ctx.region_c;

            analyzer_run(&aa);
            _exit(0);
        }
        g_child_pids[g_num_children++] = pid;
    }

    /* ── Fork Aggregator Process ── */
    printf("[PID:%d] Forking Aggregator\n", getpid());
    {
        pid_t pid = fork();
        if (pid < 0) { perror("fork aggregator"); exit(1); }
        if (pid == 0) {
            for (int j = 0; j < num_files; j++)
                close(pipe_read_fds[j]);

            aggregator_args_t aga;
            aga.region_c = g_shm_ctx.region_c;
            aga.region_d = g_shm_ctx.region_d;
            aga.timeout_sec = timeout_sec;
            aga.num_keywords = num_keywords;
            for (int k = 0; k < num_keywords; k++)
                strncpy(aga.keywords[k], keywords[k], MAX_SOURCE - 1);
            aga.output_file = output_file;
            aga.binary_file = binary_file;
            aga.num_files = num_files;
            aga.num_workers = num_worker_threads;

            aggregator_run(&aga);
            _exit(0);
        }
        g_child_pids[g_num_children++] = pid;
    }

    /* ── Start Watchdog Thread ── */
    pthread_t watchdog_tid;
    watchdog_args_t wa;
    wa.num_readers = num_files;
    wa.shutdown_flag = &g_shutdown_watchdog;
    wa.num_children = g_num_children;
    for (int i = 0; i < num_files; i++) {
        wa.pipe_fds[i] = pipe_read_fds[i];
        strncpy(wa.filenames[i], log_files[i], 255);
    }

    if (pthread_create(&watchdog_tid, NULL, watchdog_thread_func, &wa) != 0) {
        perror("pthread_create watchdog");
        exit(1);
    }
    printf("[PID:%d] Watchdog thread started.\n", getpid());

    /* ── Wait for all children ── */
    int children_remaining = g_num_children;
    while (children_remaining > 0) {
        if (g_sigint_received) {
            /* SIGINT handling: send SIGTERM to all children */
            for (int i = 0; i < g_num_children; i++) {
                if (g_child_pids[i] > 0)
                    kill(g_child_pids[i], SIGTERM);
            }

            /* Wait with 5-second deadline */
            time_t deadline = time(NULL) + 5;
            while (children_remaining > 0 && time(NULL) < deadline) {
                pid_t w = waitpid(-1, NULL, WNOHANG);
                if (w > 0) {
                    children_remaining--;
                    for (int i = 0; i < g_num_children; i++) {
                        if (g_child_pids[i] == w) g_child_pids[i] = 0;
                    }
                } else {
                    usleep(50000); /* 50ms */
                }
            }

            /* Force kill remaining */
            for (int i = 0; i < g_num_children; i++) {
                if (g_child_pids[i] > 0) {
                    kill(g_child_pids[i], SIGKILL);
                    waitpid(g_child_pids[i], NULL, 0);
                }
            }

            /* Cleanup and exit */
            g_shutdown_watchdog = 1;
            pthread_join(watchdog_tid, NULL);
            shm_destroy(&g_shm_ctx);
            _exit(1);
        }

        pid_t w = waitpid(-1, NULL, WNOHANG);
        if (w > 0) {
            children_remaining--;
            wa.num_children = children_remaining;
            for (int i = 0; i < g_num_children; i++) {
                if (g_child_pids[i] == w) g_child_pids[i] = 0;
            }
        } else {
            usleep(50000);
        }
    }

    /* ── Stop Watchdog Thread ── */
    g_shutdown_watchdog = 1;
    pthread_join(watchdog_tid, NULL);

    /* ── Print Final Summary ── */
    region_c_t *rc = g_shm_ctx.region_c;

    /* Compute totals from Region C */
    long total_entries = 0;
    double total_weighted = 0.0;
    for (int i = 0; i < NUM_LEVELS; i++) {
        total_entries += rc->results[i].total_entries;
        total_weighted += rc->results[i].total_weighted_score;
    }

    /* Read high-priority score from the output file (or binary) — just report 0.0 as baseline */
    /* Actually we can compute from Region D but it was already drained by Aggregator.
     * We'll re-read from binary file header */
    double high_priority = 0.0;
    {
        FILE *bf = fopen(binary_file, "rb");
        if (bf) {
            binary_header_t hdr;
            if (fread(&hdr, sizeof(hdr), 1, bf) == 1 && hdr.magic == BINARY_MAGIC) {
                high_priority = hdr.high_priority_weighted;
            }
            fclose(bf);
        }
    }

    printf("==================================================\n");
    printf("SYSTEM SUMMARY\n");
    printf("Keywords         : %s\n", keyword_str);
    printf("Log files        : %d\n", num_files);
    printf("Total entries    : %ld\n", total_entries);
    printf("Total weighted   : %.1f\n", total_weighted);
    printf("High-priority    : %.1f (source filter: %s)\n", high_priority, filter_file);

    /* Sort levels by weighted score desc for display */
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
    for (int i = 0; i < NUM_LEVELS; i++) {
        int idx = order[i];
        printf("  %-6s: %ld entries, score: %.1f\n",
               rc->results[idx].level,
               rc->results[idx].total_entries,
               rc->results[idx].total_weighted_score);
    }
    printf("==================================================\n");
    printf("Program terminated successfully.\n");

    /* Cleanup */
    shm_destroy(&g_shm_ctx);

    return 0;
}
