/*
 * CSE344 HW4 - Shared Memory Implementation
 * mmap-based shared memory regions with process-shared synchronization.
 */

#define _GNU_SOURCE
#include "shm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* ─── Helper: init process-shared mutex ─────────────────────── */
static int init_shared_mutex(pthread_mutex_t *mtx) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    int ret = pthread_mutex_init(mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    return ret;
}

/* ─── Helper: init process-shared condvar ───────────────────── */
static int init_shared_cond(pthread_cond_t *cond) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    int ret = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return ret;
}

/* ─── shm_init ──────────────────────────────────────────────── */
int shm_init(shm_context_t *ctx, int cap_a, int cap_b, int cap_d,
             int total_readers, int num_keywords, const char keywords[][MAX_SOURCE],
             int num_workers) {
    memset(ctx, 0, sizeof(shm_context_t));

    /* ── Region A ── */
    ctx->size_a = sizeof(region_a_t) + (size_t)cap_a * sizeof(log_entry_t);
    ctx->region_a = mmap(NULL, ctx->size_a, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctx->region_a == MAP_FAILED) {
        perror("mmap Region A");
        return -1;
    }
    memset(ctx->region_a, 0, ctx->size_a);
    ctx->region_a->capacity = cap_a;
    ctx->region_a->total_readers = total_readers;
    init_shared_mutex(&ctx->region_a->mutex);
    init_shared_cond(&ctx->region_a->not_full);
    init_shared_cond(&ctx->region_a->not_empty);

    /* ── Region B (×4) ── */
    ctx->size_b = sizeof(level_buffer_t) + (size_t)cap_b * sizeof(log_entry_t);
    for (int i = 0; i < NUM_LEVELS; i++) {
        ctx->region_b[i] = mmap(NULL, ctx->size_b, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (ctx->region_b[i] == MAP_FAILED) {
            perror("mmap Region B");
            return -1;
        }
        memset(ctx->region_b[i], 0, ctx->size_b);
        ctx->region_b[i]->capacity = cap_b;
        init_shared_mutex(&ctx->region_b[i]->mutex);
        init_shared_cond(&ctx->region_b[i]->not_full);
        init_shared_cond(&ctx->region_b[i]->not_empty);
    }

    /* ── Region C ── */
    ctx->size_c = sizeof(region_c_t);
    ctx->region_c = mmap(NULL, ctx->size_c, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctx->region_c == MAP_FAILED) {
        perror("mmap Region C");
        return -1;
    }
    memset(ctx->region_c, 0, ctx->size_c);
    init_shared_mutex(&ctx->region_c->mutex);
    init_shared_cond(&ctx->region_c->cond);
    ctx->region_c->num_keywords = num_keywords;
    ctx->region_c->num_workers = num_workers;
    for (int i = 0; i < num_keywords; i++) {
        strncpy(ctx->region_c->keywords[i], keywords[i], MAX_SOURCE - 1);
    }
    for (int i = 0; i < NUM_LEVELS; i++) {
        strncpy(ctx->region_c->results[i].level, LEVEL_NAMES[i], 7);
#ifdef __APPLE__
        snprintf(ctx->region_c->sem_name[i],
                 sizeof(ctx->region_c->sem_name[i]),
                 "/cse344_hw4_%ld_%d", (long)getpid(), i);
        sem_unlink(ctx->region_c->sem_name[i]);
        sem_t *named_sem = sem_open(ctx->region_c->sem_name[i],
                                    O_CREAT | O_EXCL, 0600, 0);
        if (named_sem == SEM_FAILED) {
            perror("sem_open Region C");
            return -1;
        }
        sem_close(named_sem);
#else
        /* Init unnamed process-shared semaphore */
        if (sem_init(&ctx->region_c->sem[i], 1, 0) != 0) {
            perror("sem_init Region C");
            return -1;
        }
#endif
    }

    /* ── Region D ── */
    ctx->size_d = sizeof(region_d_t) + (size_t)cap_d * sizeof(log_entry_t);
    ctx->region_d = mmap(NULL, ctx->size_d, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctx->region_d == MAP_FAILED) {
        perror("mmap Region D");
        return -1;
    }
    memset(ctx->region_d, 0, ctx->size_d);
    ctx->region_d->capacity = cap_d;
    init_shared_mutex(&ctx->region_d->mutex);
    init_shared_cond(&ctx->region_d->not_full);
    init_shared_cond(&ctx->region_d->not_empty);

    return 0;
}

/* ─── shm_destroy ───────────────────────────────────────────── */
void shm_destroy(shm_context_t *ctx) {
    if (ctx->region_a) {
        pthread_mutex_destroy(&ctx->region_a->mutex);
        pthread_cond_destroy(&ctx->region_a->not_full);
        pthread_cond_destroy(&ctx->region_a->not_empty);
        munmap(ctx->region_a, ctx->size_a);
        ctx->region_a = NULL;
    }

    for (int i = 0; i < NUM_LEVELS; i++) {
        if (ctx->region_b[i]) {
            pthread_mutex_destroy(&ctx->region_b[i]->mutex);
            pthread_cond_destroy(&ctx->region_b[i]->not_full);
            pthread_cond_destroy(&ctx->region_b[i]->not_empty);
            munmap(ctx->region_b[i], ctx->size_b);
            ctx->region_b[i] = NULL;
        }
    }

    if (ctx->region_c) {
        pthread_mutex_destroy(&ctx->region_c->mutex);
        pthread_cond_destroy(&ctx->region_c->cond);
        for (int i = 0; i < NUM_LEVELS; i++) {
#ifdef __APPLE__
            if (ctx->region_c->sem_name[i][0] != '\0')
                sem_unlink(ctx->region_c->sem_name[i]);
#else
            sem_destroy(&ctx->region_c->sem[i]);
#endif
        }
        munmap(ctx->region_c, ctx->size_c);
        ctx->region_c = NULL;
    }

    if (ctx->region_d) {
        pthread_mutex_destroy(&ctx->region_d->mutex);
        pthread_cond_destroy(&ctx->region_d->not_full);
        pthread_cond_destroy(&ctx->region_d->not_empty);
        munmap(ctx->region_d, ctx->size_d);
        ctx->region_d = NULL;
    }
}

/* ─── Region C semaphore post ───────────────────────────────── */
int region_c_sem_post(region_c_t *c, int level_idx) {
    if (!c || level_idx < 0 || level_idx >= NUM_LEVELS) {
        errno = EINVAL;
        return -1;
    }

#ifdef __APPLE__
    sem_t *s = sem_open(c->sem_name[level_idx], 0);
    if (s == SEM_FAILED)
        return -1;

    int ret = sem_post(s);
    int saved_errno = errno;
    sem_close(s);
    errno = saved_errno;
    return ret;
#else
    return sem_post(&c->sem[level_idx]);
#endif
}

/* ─── Region C semaphore wait ───────────────────────────────── */
int region_c_sem_wait(region_c_t *c, int level_idx) {
    if (!c || level_idx < 0 || level_idx >= NUM_LEVELS) {
        errno = EINVAL;
        return -1;
    }

#ifdef __APPLE__
    sem_t *s = sem_open(c->sem_name[level_idx], 0);
    if (s == SEM_FAILED)
        return -1;

    int ret;
    do {
        ret = sem_wait(s);
    } while (ret == -1 && errno == EINTR);

    int saved_errno = errno;
    sem_close(s);
    errno = saved_errno;
    return ret;
#else
    int ret;
    do {
        ret = sem_wait(&c->sem[level_idx]);
    } while (ret == -1 && errno == EINTR);
    return ret;
#endif
}

/* ─── Region A push (blocking) ──────────────────────────────── */
void region_a_push(region_a_t *a, const log_entry_t *entry) {
    pthread_mutex_lock(&a->mutex);
    while (a->count >= a->capacity) {
        pthread_cond_wait(&a->not_full, &a->mutex);
    }
    a->entries[a->tail] = *entry;
    a->tail = (a->tail + 1) % a->capacity;
    a->count++;

    pthread_cond_signal(&a->not_empty);
    pthread_mutex_unlock(&a->mutex);
}

/* ─── Region A pop (timed wait) ─────────────────────────────── */
int region_a_pop(region_a_t *a, log_entry_t *entry, int timeout_sec) {
    struct timespec ts;
    pthread_mutex_lock(&a->mutex);

    while (a->count == 0) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_sec;
        int ret = pthread_cond_timedwait(&a->not_empty, &a->mutex, &ts);
        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&a->mutex);
            return -1; /* timeout */
        }
    }

    *entry = a->entries[a->head];
    a->head = (a->head + 1) % a->capacity;
    a->count--;

    pthread_cond_signal(&a->not_full);
    pthread_mutex_unlock(&a->mutex);
    return 0;
}

/* ─── Level Buffer push (blocking) ──────────────────────────── */
void level_buffer_push(level_buffer_t *b, const log_entry_t *entry) {
    pthread_mutex_lock(&b->mutex);
    while (b->count >= b->capacity) {
        pthread_cond_wait(&b->not_full, &b->mutex);
    }
    b->entries[b->tail] = *entry;
    b->tail = (b->tail + 1) % b->capacity;
    b->count++;
    pthread_cond_broadcast(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
}

/* ─── Level Buffer pop (blocking, returns -1 on EOF) ─────────── */
int level_buffer_pop(level_buffer_t *b, log_entry_t *entry) {
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0) {
        if (b->eof_posted) {
            pthread_mutex_unlock(&b->mutex);
            return -1; /* no more data */
        }
        pthread_cond_wait(&b->not_empty, &b->mutex);
    }

    *entry = b->entries[b->head];
    b->head = (b->head + 1) % b->capacity;
    b->count--;

    /* If we just consumed an EOF marker, set eof_posted */
    if (entry->is_eof) {
        b->eof_posted = 1;
        pthread_cond_broadcast(&b->not_empty); /* wake up other workers */
        pthread_cond_signal(&b->not_full);
        pthread_mutex_unlock(&b->mutex);
        return -1; /* EOF signal */
    }

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ─── Level Buffer pop for deterministic worker turn ────────── */
int level_buffer_pop_for_worker(level_buffer_t *b, log_entry_t *entry,
                                int worker_idx, int num_workers) {
    pthread_mutex_lock(&b->mutex);

    while (1) {
        while (b->count == 0) {
            if (b->eof_posted) {
                pthread_mutex_unlock(&b->mutex);
                return -1;
            }
            pthread_cond_wait(&b->not_empty, &b->mutex);
        }

        *entry = b->entries[b->head];
        if (entry->is_eof) {
            b->eof_posted = 1;
            pthread_cond_broadcast(&b->not_empty);
            pthread_cond_signal(&b->not_full);
            pthread_mutex_unlock(&b->mutex);
            return -1;
        }

        if (num_workers <= 1 || b->next_worker == worker_idx)
            break;

        pthread_cond_wait(&b->not_empty, &b->mutex);
    }

    b->head = (b->head + 1) % b->capacity;
    b->count--;
    if (num_workers > 0)
        b->next_worker = (b->next_worker + 1) % num_workers;

    pthread_cond_signal(&b->not_full);
    pthread_cond_broadcast(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ─── Region D push (blocking) ──────────────────────────────── */
void region_d_push(region_d_t *d, const log_entry_t *entry) {
    pthread_mutex_lock(&d->mutex);
    while (d->count >= d->capacity) {
        pthread_cond_wait(&d->not_full, &d->mutex);
    }
    d->entries[d->tail] = *entry;
    d->tail = (d->tail + 1) % d->capacity;
    d->count++;
    pthread_cond_signal(&d->not_empty);
    pthread_mutex_unlock(&d->mutex);
}

/* ─── Region D pop (blocking until data or dispatcher_done) ─── */
int region_d_pop(region_d_t *d, log_entry_t *entry) {
    pthread_mutex_lock(&d->mutex);
    while (d->count == 0 && !d->dispatcher_done) {
        pthread_cond_wait(&d->not_empty, &d->mutex);
    }

    if (d->count == 0 && d->dispatcher_done) {
        pthread_mutex_unlock(&d->mutex);
        return -1;
    }

    *entry = d->entries[d->head];
    d->head = (d->head + 1) % d->capacity;
    d->count--;
    pthread_cond_signal(&d->not_full);
    pthread_mutex_unlock(&d->mutex);
    return 0;
}

/* ─── Region D pop (non-blocking-ish, returns -1 if empty+done) ─ */
int region_d_pop_nonblock(region_d_t *d, log_entry_t *entry) {
    pthread_mutex_lock(&d->mutex);
    if (d->count == 0) {
        pthread_mutex_unlock(&d->mutex);
        return -1;
    }
    *entry = d->entries[d->head];
    d->head = (d->head + 1) % d->capacity;
    d->count--;
    pthread_cond_signal(&d->not_full);
    pthread_mutex_unlock(&d->mutex);
    return 0;
}
