#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_LINE 512

static volatile sig_atomic_t g_exit = 0;
static int g_sock = -1;

static void sigint_handler(int sig) { (void)sig; g_exit = 1; }

static int send_all(int fd, const char *msg) {
    size_t len = strlen(msg), sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, msg + sent, len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server_ip> <port> <username>\n", argv[0]);
        exit(1);
    }

    const char *server_ip = argv[1];
    int         port      = atoi(argv[2]);
    const char *username  = argv[3];

    if (port < 1) { fprintf(stderr, "Invalid port\n"); exit(1); }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", server_ip);
        exit(1);
    }

    if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); exit(1);
    }

    printf("[PROFESSOR %s] CONNECTED server=%s:%d\n", username, server_ip, port);
    fflush(stdout);

    /* send ENROLL automatically */
    char enroll_msg[MAX_LINE + 2];
    snprintf(enroll_msg, sizeof(enroll_msg), "ENROLL PROFESSOR %s\n", username);
    send_all(g_sock, enroll_msg);

    /* per-client line buffer for server responses */
    char srv_buf[MAX_LINE + 1];
    int  srv_len = 0;

    while (!g_exit) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(g_sock, &rfds);

        int ret = select(g_sock + 1, &rfds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* ── data from server ───────────────────────────────────────────── */
        if (FD_ISSET(g_sock, &rfds)) {
            char tmp[MAX_LINE];
            ssize_t n = read(g_sock, tmp, sizeof(tmp));
            if (n <= 0) {
                printf("\n[PROFESSOR %s] DISCONNECTED reason=shutdown\n", username);
                fflush(stdout);
                close(g_sock);
                exit(0);
            }
            for (int i = 0; i < (int)n; i++) {
                char ch = tmp[i];
                if (srv_len < MAX_LINE) srv_buf[srv_len++] = ch;
                if (ch == '\n') {
                    srv_buf[srv_len] = '\0';
                    int l = srv_len;
                    while (l > 0 && (srv_buf[l-1]=='\n' || srv_buf[l-1]=='\r'))
                        srv_buf[--l] = '\0';
                    srv_len = 0;

                    printf("[PROFESSOR %s] RECEIVED %s\n", username, srv_buf);
                    fflush(stdout);

                    if (strcmp(srv_buf, "OK APPARATE") == 0) {
                        printf("[PROFESSOR %s] DISCONNECTED reason=APPARATE\n", username);
                        fflush(stdout);
                        close(g_sock);
                        exit(0);
                    }
                    if (strcmp(srv_buf, "TIMEOUT DISCONNECT") == 0) {
                        printf("[PROFESSOR %s] DISCONNECTED reason=timeout\n", username);
                        fflush(stdout);
                        close(g_sock);
                        exit(0);
                    }
                    if (strcmp(srv_buf, "SERVER SHUTDOWN") == 0) {
                        printf("[PROFESSOR %s] DISCONNECTED reason=shutdown\n", username);
                        fflush(stdout);
                        close(g_sock);
                        exit(0);
                    }

                    printf("> ");
                    fflush(stdout);
                }
            }
        }

        /* ── data from stdin ────────────────────────────────────────────── */
        if (!g_exit && FD_ISSET(STDIN_FILENO, &rfds)) {
            char line[MAX_LINE + 1];
            if (fgets(line, sizeof(line), stdin) == NULL) {
                g_exit = 1;
                break;
            }
            int l = (int)strlen(line);
            while (l > 0 && (line[l-1]=='\n' || line[l-1]=='\r'))
                line[--l] = '\0';

            printf("[PROFESSOR %s] SENT %s\n", username, line);
            fflush(stdout);

            char msg[MAX_LINE + 2];
            snprintf(msg, sizeof(msg), "%s\n", line);
            if (send_all(g_sock, msg) < 0) {
                printf("[PROFESSOR %s] DISCONNECTED reason=shutdown\n", username);
                fflush(stdout);
                close(g_sock);
                exit(0);
            }
        }
    }

    /* ── SIGINT: send APPARATE then exit cleanly ────────────────────────── */
    printf("\n[PROFESSOR %s] SENT APPARATE\n", username);
    fflush(stdout);
    send_all(g_sock, "APPARATE\n");

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(g_sock, &rfds);
    struct timeval tv = {2, 0};
    if (select(g_sock + 1, &rfds, NULL, NULL, &tv) > 0) {
        char tmp[MAX_LINE + 1];
        ssize_t n = read(g_sock, tmp, MAX_LINE);
        if (n > 0) {
            tmp[n] = '\0';
            int l = (int)n;
            while (l > 0 && (tmp[l-1]=='\n' || tmp[l-1]=='\r')) tmp[--l] = '\0';
            printf("[PROFESSOR %s] RECEIVED %s\n", username, tmp);
            fflush(stdout);
        }
    }
    printf("[PROFESSOR %s] DISCONNECTED reason=APPARATE\n", username);
    fflush(stdout);
    close(g_sock);
    return 0;
}
