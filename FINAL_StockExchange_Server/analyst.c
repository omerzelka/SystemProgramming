#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BUF_SIZE 1024

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static int send_all(int fd, const char *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n <= 0) return -1;
        sent += (int)n;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server_ip> <tcp_port> <username>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int tcp_port = atoi(argv[2]);
    const char *username = argv[3];

    if (tcp_port < 1024) {
        fprintf(stderr, "Error: port must be >= 1024\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)tcp_port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Error: invalid server IP\n");
        close(sockfd);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        return 1;
    }

    printf("[ANALYST %s] CONNECTED server=%s:%d\n", username, server_ip, tcp_port);
    fflush(stdout);

    /* Send JOIN */
    char msg[512];
    int mlen = snprintf(msg, sizeof(msg), "JOIN ANALYST %s\n", username);
    send_all(sockfd, msg, mlen);
    printf("[ANALYST %s] SENT JOIN\n", username);
    fflush(stdout);

    /* Line buffer for server responses */
    char srv_buf[BUF_SIZE];
    int srv_len = 0;
    int quit_sent = 0;

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        int maxfd = sockfd;

        if (!quit_sent) {
            FD_SET(STDIN_FILENO, &rfds);
            if (STDIN_FILENO > maxfd) maxfd = STDIN_FILENO;
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Data from server */
        if (FD_ISSET(sockfd, &rfds)) {
            int space = BUF_SIZE - srv_len - 1;
            if (space <= 0) { srv_len = 0; space = BUF_SIZE - 1; }
            ssize_t n = read(sockfd, srv_buf + srv_len, (size_t)space);
            if (n <= 0) {
                printf("[ANALYST %s] DISCONNECTED reason=shutdown\n", username);
                fflush(stdout);
                close(sockfd);
                return 0;
            }
            srv_len += (int)n;
            srv_buf[srv_len] = '\0';

            char *start = srv_buf;
            char *nl;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = '\0';
                printf("[ANALYST %s] RECEIVED %s\n", username, start);
                fflush(stdout);

                if (strcmp(start, "SERVER SHUTDOWN") == 0) {
                    printf("[ANALYST %s] DISCONNECTED reason=shutdown\n", username);
                    fflush(stdout);
                    close(sockfd);
                    return 0;
                }
                if (strcmp(start, "OK QUIT") == 0) {
                    printf("[ANALYST %s] DISCONNECTED reason=QUIT\n", username);
                    fflush(stdout);
                    close(sockfd);
                    return 0;
                }
                start = nl + 1;
            }
            int remaining = srv_len - (int)(start - srv_buf);
            if (remaining > 0 && start != srv_buf)
                memmove(srv_buf, start, (size_t)remaining);
            srv_len = remaining;
        }

        /* Data from stdin */
        if (!quit_sent && FD_ISSET(STDIN_FILENO, &rfds)) {
            char input[512];
            ssize_t n = read(STDIN_FILENO, input, sizeof(input) - 1);
            if (n <= 0) {
                /* EOF (Ctrl+D) */
                mlen = snprintf(msg, sizeof(msg), "QUIT\n");
                send_all(sockfd, msg, mlen);
                printf("[ANALYST %s] SENT QUIT\n", username);
                fflush(stdout);
                quit_sent = 1;
                continue;
            }
            input[n] = '\0';

            char *line_start = input;
            char *line_nl;
            while ((line_nl = strchr(line_start, '\n')) != NULL) {
                *line_nl = '\0';
                if (strlen(line_start) > 0) {
                    printf("[ANALYST %s] SENT %s\n", username, line_start);
                    fflush(stdout);
                    mlen = snprintf(msg, sizeof(msg), "%s\n", line_start);
                    send_all(sockfd, msg, mlen);
                    if (strcmp(line_start, "QUIT") == 0)
                        quit_sent = 1;
                }
                line_start = line_nl + 1;
            }
            if (strlen(line_start) > 0) {
                printf("[ANALYST %s] SENT %s\n", username, line_start);
                fflush(stdout);
                mlen = snprintf(msg, sizeof(msg), "%s\n", line_start);
                send_all(sockfd, msg, mlen);
                if (strcmp(line_start, "QUIT") == 0)
                    quit_sent = 1;
            }
        }
    }

    /* SIGINT received */
    if (!quit_sent) {
        mlen = snprintf(msg, sizeof(msg), "QUIT\n");
        send_all(sockfd, msg, mlen);
        printf("[ANALYST %s] SENT QUIT\n", username);
        fflush(stdout);
    }
    printf("[ANALYST %s] DISCONNECTED reason=QUIT\n", username);
    fflush(stdout);
    close(sockfd);
    return 0;
}
