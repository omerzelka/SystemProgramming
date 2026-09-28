/*
 * CSE344 HW4 - Watchdog Thread Implementation
 *
 * Runs inside the parent process after all children are forked.
 * Uses select() with 3-second timeout on pipe read-ends from Reader Processes.
 * Prints progress snapshots to stderr.
 */

#define _GNU_SOURCE
#include "watchdog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>

void *watchdog_thread_func(void *arg) {
    watchdog_args_t *wa = (watchdog_args_t *)arg;

    int lines_processed[MAX_READERS];
    memset(lines_processed, 0, sizeof(lines_processed));

    time_t start_time = time(NULL);
    int elapsed = 0;

    while (!(*wa->shutdown_flag)) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = -1;

        for (int i = 0; i < wa->num_readers; i++) {
            if (wa->pipe_fds[i] >= 0) {
                FD_SET(wa->pipe_fds[i], &readfds);
                if (wa->pipe_fds[i] > maxfd)
                    maxfd = wa->pipe_fds[i];
            }
        }

        struct timeval tv;
        tv.tv_sec = 3;
        tv.tv_usec = 0;

        int ret;
        if (maxfd >= 0) {
            ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        } else {
            /* No valid pipes left, just sleep */
            sleep(3);
            ret = 0;
        }

        if (*wa->shutdown_flag) break;

        /* Read any available data from pipes */
        if (ret > 0) {
            for (int i = 0; i < wa->num_readers; i++) {
                if (wa->pipe_fds[i] >= 0 && FD_ISSET(wa->pipe_fds[i], &readfds)) {
                    char buf[4096];
                    ssize_t n = read(wa->pipe_fds[i], buf, sizeof(buf) - 1);
                    if (n > 0) {
                        buf[n] = '\0';
                        /* Parse heartbeat lines: "[R<i>] <n> lines processed" */
                        char *line = strtok(buf, "\n");
                        while (line) {
                            int ri, lcount;
                            if (sscanf(line, "[R%d] %d lines processed", &ri, &lcount) == 2) {
                                if (ri >= 0 && ri < wa->num_readers) {
                                    lines_processed[ri] = lcount;
                                }
                            }
                            line = strtok(NULL, "\n");
                        }
                    } else if (n == 0) {
                        /* Pipe closed — reader exited */
                        close(wa->pipe_fds[i]);
                        wa->pipe_fds[i] = -1;
                    }
                }
            }
        }

        /* Print progress snapshot to stderr */
        elapsed = (int)(time(NULL) - start_time);
        /* Only print on timeout (every ~3 seconds) or after reading data */
        if (elapsed > 0) {
            fprintf(stderr, "[WATCHDOG] Progress at T+%ds:", elapsed);
            for (int i = 0; i < wa->num_readers; i++) {
                fprintf(stderr, " Reader%d=%d", i, lines_processed[i]);
            }
            fprintf(stderr, " children_alive=%d\n", wa->num_children);
        }
    }

    /* Close remaining pipe fds */
    for (int i = 0; i < wa->num_readers; i++) {
        if (wa->pipe_fds[i] >= 0) {
            close(wa->pipe_fds[i]);
            wa->pipe_fds[i] = -1;
        }
    }

    return NULL;
}
