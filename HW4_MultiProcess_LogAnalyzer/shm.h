/*
 * CSE344 HW4 - Shared Memory Definitions
 * All shared memory region structs, log entry types, and helper function prototypes.
 */

#ifndef SHM_H
#define SHM_H

#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>

/* ─── Constants ─────────────────────────────────────────────── */
#define MAX_KEYWORDS    8
#define MAX_WORKERS     64
#define MAX_MSG         512
#define MAX_SOURCE      64
#define MAX_TIMESTAMP   24
#define MAX_FILES       32
#define MAX_LINE        1024
#define NUM_LEVELS      4

/* Level indices */
#define LEVEL_ERROR     0
#define LEVEL_WARN      1
#define LEVEL_INFO      2
#define LEVEL_DEBUG      3

/* Level weights for scoring */
static const int LEVEL_WEIGHT[NUM_LEVELS] = {4, 3, 2, 1};
static const char *LEVEL_NAMES[NUM_LEVELS] = {"ERROR", "WARN", "INFO", "DEBUG"};

/* ─── Log Entry ─────────────────────────────────────────────── */
typedef struct {
    char   timestamp[MAX_TIMESTAMP];
    int    level_idx;                   /* 0=ERROR,1=WARN,2=INFO,3=DEBUG */
    char   source[MAX_SOURCE];
    char   message[MAX_MSG];
    int    is_eof;                      /* 1 = EOF marker, not a real entry */
    int    eof_level;                   /* which level this EOF is for */
} log_entry_t;

/* ─── Region A — Dispatcher Input Queue ─────────────────────── */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;

    int capacity;
    int head;
    int tail;
    int count;

    int eof_count_per_level[NUM_LEVELS]; /* how many readers sent EOF per level */
    int total_readers;                    /* set by parent before fork */

    log_entry_t entries[];               /* flexible array member */
} region_a_t;

/* ─── Region B — Per-Level Analysis Buffer ──────────────────── */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;

    int capacity;
    int head;
    int tail;
    int count;
    int eof_posted;                     /* set to 1 by Dispatcher */
    int next_worker;                    /* deterministic round-robin consumer */

    log_entry_t entries[];              /* flexible array member */
} level_buffer_t;

/* ─── Region C — Results Area ───────────────────────────────── */
typedef struct {
    char   level[8];
    long   total_entries;
    double total_weighted_score;
    double per_keyword_score[MAX_KEYWORDS];
    double per_thread_score[MAX_WORKERS];
    char   top_source[3][64];
    long   top_source_hits[3];
    int    ready;
} level_result_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;              /* for Aggregator timed wait */

#ifdef __APPLE__
    char            sem_name[NUM_LEVELS][64]; /* macOS named-semaphore fallback */
#else
    sem_t           sem[NUM_LEVELS];   /* one per level */
#endif

    int             num_keywords;
    char            keywords[MAX_KEYWORDS][MAX_SOURCE];
    int             num_workers;

    level_result_t  results[NUM_LEVELS];
} region_c_t;

/* ─── Region D — High-Priority Buffer ──────────────────────── */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;

    int capacity;
    int head;
    int tail;
    int count;
    int dispatcher_done;               /* set by Dispatcher when finished */

    log_entry_t entries[];             /* flexible array member */
} region_d_t;

/* ─── SHM Context ──────────────────────────────────────────── */
typedef struct {
    region_a_t      *region_a;
    level_buffer_t  *region_b[NUM_LEVELS];
    region_c_t      *region_c;
    region_d_t      *region_d;

    size_t size_a;
    size_t size_b;   /* per level buffer */
    size_t size_c;
    size_t size_d;
} shm_context_t;

/* ─── Binary Checkpoint Header ─────────────────────────────── */
#define BINARY_MAGIC    0xC5E3440B
#define BINARY_VERSION  1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t num_levels;
    uint32_t num_keywords;
    double   total_weighted;
    double   high_priority_weighted;
} binary_header_t;

/* ─── Function Prototypes ──────────────────────────────────── */

/*
 * Initialize all shared memory regions.
 * Must be called in parent before any fork().
 */
int shm_init(shm_context_t *ctx, int cap_a, int cap_b, int cap_d,
             int total_readers, int num_keywords, const char keywords[][MAX_SOURCE],
             int num_workers);

/*
 * Destroy all primitives and munmap all regions.
 */
void shm_destroy(shm_context_t *ctx);

/* Region A operations */
void region_a_push(region_a_t *a, const log_entry_t *entry);
int  region_a_pop(region_a_t *a, log_entry_t *entry, int timeout_sec);

/* Region B operations */
void level_buffer_push(level_buffer_t *b, const log_entry_t *entry);
int  level_buffer_pop(level_buffer_t *b, log_entry_t *entry);
int  level_buffer_pop_for_worker(level_buffer_t *b, log_entry_t *entry,
                                 int worker_idx, int num_workers);

/* Region C semaphore operations */
int  region_c_sem_post(region_c_t *c, int level_idx);
int  region_c_sem_wait(region_c_t *c, int level_idx);

/* Region D operations */
void region_d_push(region_d_t *d, const log_entry_t *entry);
int  region_d_pop(region_d_t *d, log_entry_t *entry);
int  region_d_pop_nonblock(region_d_t *d, log_entry_t *entry);

#endif /* SHM_H */
