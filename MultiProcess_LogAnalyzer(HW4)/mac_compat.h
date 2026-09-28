#ifndef MAC_COMPAT_H
#define MAC_COMPAT_H

#ifdef __APPLE__

#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <semaphore.h>
#include <stdio.h>

#ifndef SYS_gettid
#define SYS_gettid 0
#endif

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int trip_count;
} pthread_barrier_t;

static inline int pthread_barrier_init(pthread_barrier_t *barrier, const void *attr, unsigned count) {
    (void)attr;
    barrier->trip_count = count;
    barrier->count = 0;
    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
    return 0;
}

static inline int pthread_barrier_wait(pthread_barrier_t *barrier) {
    pthread_mutex_lock(&barrier->mutex);
    barrier->count++;
    if (barrier->count >= barrier->trip_count) {
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return 1;
    } else {
        pthread_cond_wait(&barrier->cond, &barrier->mutex);
        pthread_mutex_unlock(&barrier->mutex);
        return 0;
    }
}

static inline int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    pthread_mutex_destroy(&barrier->mutex);
    pthread_cond_destroy(&barrier->cond);
    return 0;
}

static inline void mac_sem_post(int level_idx) {
    char sem_name[64];
    snprintf(sem_name, sizeof(sem_name), "/cse344_hw4_sem_%d", level_idx);
    sem_t *s = sem_open(sem_name, 0);
    if (s != SEM_FAILED) {
        sem_post(s);
        sem_close(s);
    }
}

static inline void mac_sem_wait(int level_idx) {
    char sem_name[64];
    snprintf(sem_name, sizeof(sem_name), "/cse344_hw4_sem_%d", level_idx);
    sem_t *s = sem_open(sem_name, 0);
    if (s != SEM_FAILED) {
        sem_wait(s);
        sem_close(s);
    }
}

static inline pid_t gettid_mac(void) {
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return (pid_t)tid;
}

#define GET_TID gettid_mac()

#else

#include <sys/syscall.h>
#define GET_TID (pid_t)syscall(SYS_gettid)

#endif /* __APPLE__ */

#endif /* MAC_COMPAT_H */
