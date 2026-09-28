#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_CLIENTS    64
#define MAX_STOCKS     128
#define MAX_PORTFOLIO  128
#define BUF_SIZE       1024
#define MSG_SIZE       4096
#define BCAST_SEC      5

typedef struct {
    char symbol[9];
    int  qty;
} holding_t;

typedef struct {
    int       fd;
    char      username[32];
    char      type[16];
    char      line_buf[BUF_SIZE];
    int       line_len;
    int       joined;
    holding_t portfolio[MAX_PORTFOLIO];
    int       num_holdings;
    char      ip[INET_ADDRSTRLEN];
} client_t;

typedef struct {
    char   symbol[9];
    double price;
} stock_t;

static volatile sig_atomic_t g_shutdown = 0;
static client_t  g_clients[MAX_CLIENTS];
static stock_t   g_stocks[MAX_STOCKS];
static int       g_stock_count = 0;
static int       g_tcp_fd = -1;
static int       g_udp_fd = -1;
static struct sockaddr_in g_udp_addr;
static FILE     *g_logfp = NULL;
static struct timespec g_last_bcast;

static void sig_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

static void server_log(const char *fmt, ...)
{
    char buf[MSG_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    if (g_logfp) {
        fprintf(g_logfp, "%s\n", buf);
        fflush(g_logfp);
    }
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

static int send_to_client(int fd, const char *msg)
{
    return send_all(fd, msg, (int)strlen(msg));
}

static int load_stocks(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), f) && count < MAX_STOCKS) {
        char sym[16];
        double price;
        if (sscanf(line, "%15s %lf", sym, &price) != 2) continue;
        if (price <= 0.0) continue;
        int slen = (int)strlen(sym);
        if (slen == 0 || slen > 8) continue;
        int valid = 1;
        for (int i = 0; i < slen; i++) {
            if (!isupper((unsigned char)sym[i]) && !isdigit((unsigned char)sym[i])) {
                valid = 0;
                break;
            }
        }
        if (!valid) continue;
        strncpy(g_stocks[count].symbol, sym, 8);
        g_stocks[count].symbol[8] = '\0';
        g_stocks[count].price = price;
        count++;
    }
    fclose(f);
    g_stock_count = count;
    return count;
}

static int find_stock(const char *symbol)
{
    for (int i = 0; i < g_stock_count; i++) {
        if (strcmp(g_stocks[i].symbol, symbol) == 0) return i;
    }
    return -1;
}

static void broadcast_prices(const char *trigger)
{
    char msg[MSG_SIZE];
    int off = snprintf(msg, sizeof(msg), "PRICE_UPDATE");
    for (int i = 0; i < g_stock_count; i++) {
        off += snprintf(msg + off, (size_t)(MSG_SIZE - off), " %s:%.2f",
                        g_stocks[i].symbol, g_stocks[i].price);
    }
    msg[off++] = '\n';
    sendto(g_udp_fd, msg, (size_t)off, 0,
           (struct sockaddr *)&g_udp_addr, sizeof(g_udp_addr));
    clock_gettime(CLOCK_MONOTONIC, &g_last_bcast);
    server_log("[SERVER] PRICE_BROADCAST trigger=%s", trigger);
}

static int find_username(const char *name)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd >= 0 && g_clients[i].joined &&
            strcmp(g_clients[i].username, name) == 0)
            return i;
    }
    return -1;
}

static void remove_client(int idx, const char *reason)
{
    if (g_clients[idx].fd < 0) return;
    if (g_clients[idx].joined) {
        server_log("[SERVER] CLIENT_DISCONNECTED username=%s reason=%s",
                   g_clients[idx].username, reason);
    }
    close(g_clients[idx].fd);
    g_clients[idx].fd = -1;
    g_clients[idx].joined = 0;
    g_clients[idx].line_len = 0;
    g_clients[idx].num_holdings = 0;
    memset(g_clients[idx].username, 0, sizeof(g_clients[idx].username));
    memset(g_clients[idx].type, 0, sizeof(g_clients[idx].type));
}

static void process_command(int idx, char *line)
{
    client_t *c = &g_clients[idx];
    char cmd[64] = {0};
    sscanf(line, "%63s", cmd);
    if (cmd[0] == '\0') return;

    if (!c->joined) {
        if (strcmp(cmd, "JOIN") != 0) {
            send_to_client(c->fd, "ERR NOT_JOINED\n");
            return;
        }
        char type[16] = {0}, user[32] = {0};
        if (sscanf(line, "JOIN %15s %31s", type, user) != 2) {
            send_to_client(c->fd, "ERR UNKNOWN JOIN\n");
            return;
        }
        if (strcmp(type, "TRADER") != 0 && strcmp(type, "ANALYST") != 0) {
            send_to_client(c->fd, "ERR UNKNOWN JOIN\n");
            return;
        }
        if (find_username(user) >= 0) {
            send_to_client(c->fd, "ERR JOIN name_taken\n");
            return;
        }
        strncpy(c->username, user, 31);
        c->username[31] = '\0';
        strncpy(c->type, type, 15);
        c->type[15] = '\0';
        c->joined = 1;
        c->num_holdings = 0;
        char resp[128];
        snprintf(resp, sizeof(resp), "OK JOIN %s\n", user);
        send_to_client(c->fd, resp);
        server_log("[SERVER] JOIN username=%s type=%s fd=%d", user, type, c->fd);
        return;
    }

    if (strcmp(cmd, "QUIT") == 0) {
        send_to_client(c->fd, "OK QUIT\n");
        remove_client(idx, "QUIT");
        return;
    }

    int is_trader = (strcmp(c->type, "TRADER") == 0);

    if (is_trader) {
        if (strcmp(cmd, "BUY") == 0) {
            char sym[16] = {0};
            int qty = 0;
            if (sscanf(line, "BUY %15s %d", sym, &qty) != 2 || qty <= 0) {
                send_to_client(c->fd, "ERR UNKNOWN_SYMBOL\n");
                return;
            }
            int si = find_stock(sym);
            if (si < 0) {
                send_to_client(c->fd, "ERR UNKNOWN_SYMBOL\n");
                return;
            }
            double old_price = g_stocks[si].price;
            g_stocks[si].price += qty * 0.01;

            int found = -1;
            for (int h = 0; h < c->num_holdings; h++) {
                if (strcmp(c->portfolio[h].symbol, sym) == 0) {
                    found = h;
                    break;
                }
            }
            if (found >= 0) {
                c->portfolio[found].qty += qty;
            } else if (c->num_holdings < MAX_PORTFOLIO) {
                strncpy(c->portfolio[c->num_holdings].symbol, sym, 8);
                c->portfolio[c->num_holdings].symbol[8] = '\0';
                c->portfolio[c->num_holdings].qty = qty;
                c->num_holdings++;
            }

            char resp[256];
            snprintf(resp, sizeof(resp), "OK BUY %s %d %.2f\n",
                     sym, qty, g_stocks[si].price);
            send_to_client(c->fd, resp);
            server_log("[SERVER] BUY trader=%s symbol=%s qty=%d old_price=%.2f new_price=%.2f",
                       c->username, sym, qty, old_price, g_stocks[si].price);
            broadcast_prices("TRADE");

        } else if (strcmp(cmd, "SELL") == 0) {
            char sym[16] = {0};
            int qty = 0;
            if (sscanf(line, "SELL %15s %d", sym, &qty) != 2 || qty <= 0) {
                send_to_client(c->fd, "ERR UNKNOWN_SYMBOL\n");
                return;
            }
            int si = find_stock(sym);
            if (si < 0) {
                send_to_client(c->fd, "ERR UNKNOWN_SYMBOL\n");
                return;
            }
            int found = -1;
            for (int h = 0; h < c->num_holdings; h++) {
                if (strcmp(c->portfolio[h].symbol, sym) == 0) {
                    found = h;
                    break;
                }
            }
            if (found < 0 || c->portfolio[found].qty < qty) {
                send_to_client(c->fd, "ERR INSUFFICIENT_SHARES\n");
                return;
            }
            double old_price = g_stocks[si].price;
            g_stocks[si].price -= qty * 0.01;
            if (g_stocks[si].price < 0.01) g_stocks[si].price = 0.01;

            c->portfolio[found].qty -= qty;
            if (c->portfolio[found].qty == 0) {
                c->portfolio[found] = c->portfolio[c->num_holdings - 1];
                c->num_holdings--;
            }

            char resp[256];
            snprintf(resp, sizeof(resp), "OK SELL %s %d %.2f\n",
                     sym, qty, g_stocks[si].price);
            send_to_client(c->fd, resp);
            server_log("[SERVER] SELL trader=%s symbol=%s qty=%d old_price=%.2f new_price=%.2f",
                       c->username, sym, qty, old_price, g_stocks[si].price);
            broadcast_prices("TRADE");

        } else if (strcmp(cmd, "PORTFOLIO") == 0) {
            if (c->num_holdings == 0) {
                send_to_client(c->fd, "OK PORTFOLIO EMPTY\n");
            } else {
                char resp[MSG_SIZE];
                int off = snprintf(resp, sizeof(resp), "OK PORTFOLIO ");
                for (int h = 0; h < c->num_holdings; h++) {
                    if (h > 0) off += snprintf(resp + off, (size_t)(MSG_SIZE - off), ",");
                    off += snprintf(resp + off, (size_t)(MSG_SIZE - off), "%s:%d",
                                    c->portfolio[h].symbol, c->portfolio[h].qty);
                }
                off += snprintf(resp + off, (size_t)(MSG_SIZE - off), "\n");
                send_to_client(c->fd, resp);
            }

        } else if (strcmp(cmd, "PRICE") == 0 || strcmp(cmd, "REPORT") == 0 ||
                   strcmp(cmd, "LIST") == 0) {
            send_to_client(c->fd, "ERR UNAUTHORIZED\n");
        } else {
            char resp[256];
            snprintf(resp, sizeof(resp), "ERR UNKNOWN %s\n", cmd);
            send_to_client(c->fd, resp);
        }

    } else {
        /* ANALYST */
        if (strcmp(cmd, "PRICE") == 0) {
            char sym[16] = {0};
            if (sscanf(line, "PRICE %15s", sym) != 1) {
                send_to_client(c->fd, "ERR UNKNOWN_SYMBOL\n");
                return;
            }
            int si = find_stock(sym);
            if (si < 0) {
                send_to_client(c->fd, "ERR UNKNOWN_SYMBOL\n");
                return;
            }
            char resp[128];
            snprintf(resp, sizeof(resp), "OK PRICE %s %.2f\n",
                     sym, g_stocks[si].price);
            send_to_client(c->fd, resp);

        } else if (strcmp(cmd, "REPORT") == 0) {
            char resp[MSG_SIZE];
            int off = snprintf(resp, sizeof(resp), "OK REPORT ");
            for (int i = 0; i < g_stock_count; i++) {
                if (i > 0) off += snprintf(resp + off, (size_t)(MSG_SIZE - off), ",");
                off += snprintf(resp + off, (size_t)(MSG_SIZE - off), "%s:%.2f",
                                g_stocks[i].symbol, g_stocks[i].price);
            }
            off += snprintf(resp + off, (size_t)(MSG_SIZE - off), "\n");
            send_to_client(c->fd, resp);

        } else if (strcmp(cmd, "LIST") == 0) {
            char resp[MSG_SIZE];
            int off = snprintf(resp, sizeof(resp), "OK LIST ");
            int first = 1;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (g_clients[i].fd >= 0 && g_clients[i].joined) {
                    if (!first) off += snprintf(resp + off, (size_t)(MSG_SIZE - off), ",");
                    off += snprintf(resp + off, (size_t)(MSG_SIZE - off), "%s",
                                    g_clients[i].username);
                    first = 0;
                }
            }
            off += snprintf(resp + off, (size_t)(MSG_SIZE - off), "\n");
            send_to_client(c->fd, resp);

        } else if (strcmp(cmd, "BUY") == 0 || strcmp(cmd, "SELL") == 0 ||
                   strcmp(cmd, "PORTFOLIO") == 0) {
            send_to_client(c->fd, "ERR UNAUTHORIZED\n");
        } else {
            char resp[256];
            snprintf(resp, sizeof(resp), "ERR UNKNOWN %s\n", cmd);
            send_to_client(c->fd, resp);
        }
    }
}

static void process_client_data(int idx)
{
    client_t *c = &g_clients[idx];
    int space = BUF_SIZE - c->line_len - 1;
    if (space <= 0) {
        send_to_client(c->fd, "ERR TOOLONG\n");
        c->line_len = 0;
        return;
    }

    ssize_t n = read(c->fd, c->line_buf + c->line_len, (size_t)space);
    if (n <= 0) {
        remove_client(idx, "hangup");
        return;
    }
    c->line_len += (int)n;
    c->line_buf[c->line_len] = '\0';

    char *start = c->line_buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        int linelen = (int)(nl - start);
        if (linelen > 512) {
            send_to_client(c->fd, "ERR TOOLONG\n");
        } else {
            process_command(idx, start);
        }
        if (c->fd < 0) return;
        start = nl + 1;
    }

    int remaining = c->line_len - (int)(start - c->line_buf);
    if (remaining > 0 && start != c->line_buf) {
        memmove(c->line_buf, start, (size_t)remaining);
    }
    c->line_len = remaining;

    if (c->line_len > 512) {
        send_to_client(c->fd, "ERR TOOLONG\n");
        c->line_len = 0;
    }
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s -p <tcp_port> -u <udp_port> -s <stocks.txt> -l <logfile>\n", prog);
}

int main(int argc, char *argv[])
{
    int tcp_port = -1, udp_port = -1;
    char *stocks_file = NULL, *log_file = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "p:u:s:l:")) != -1) {
        switch (opt) {
        case 'p': tcp_port = atoi(optarg); break;
        case 'u': udp_port = atoi(optarg); break;
        case 's': stocks_file = optarg; break;
        case 'l': log_file = optarg; break;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (tcp_port < 1024 || udp_port < 1024 || !stocks_file || !log_file) {
        print_usage(argv[0]);
        return 1;
    }

    if (load_stocks(stocks_file) <= 0) {
        fprintf(stderr, "Error: cannot load stocks from %s\n", stocks_file);
        return 1;
    }

    g_logfp = fopen(log_file, "w");
    if (!g_logfp) {
        perror("fopen logfile");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    /* TCP listening socket */
    g_tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tcp_fd < 0) { perror("socket TCP"); return 1; }
    int reuse = 1;
    setsockopt(g_tcp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)tcp_port);
    if (bind(g_tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind TCP");
        return 1;
    }
    if (listen(g_tcp_fd, 10) < 0) {
        perror("listen");
        return 1;
    }

    /* UDP broadcast socket */
    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) { perror("socket UDP"); return 1; }
    int bcast = 1;
    setsockopt(g_udp_fd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    memset(&g_udp_addr, 0, sizeof(g_udp_addr));
    g_udp_addr.sin_family = AF_INET;
    g_udp_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    g_udp_addr.sin_port = htons((uint16_t)udp_port);

    /* Init client slots */
    for (int i = 0; i < MAX_CLIENTS; i++)
        g_clients[i].fd = -1;

    server_log("[SERVER] SERVER_START tcp_port=%d udp_port=%d stocks=%d",
               tcp_port, udp_port, g_stock_count);

    clock_gettime(CLOCK_MONOTONIC, &g_last_bcast);

    /* ===== Main select() loop ===== */
    while (!g_shutdown) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_tcp_fd, &rfds);
        int maxfd = g_tcp_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].fd >= 0) {
                FD_SET(g_clients[i].fd, &rfds);
                if (g_clients[i].fd > maxfd) maxfd = g_clients[i].fd;
            }
        }

        /* Calculate timeout for periodic broadcast */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - g_last_bcast.tv_sec) +
                         (double)(now.tv_nsec - g_last_bcast.tv_nsec) / 1e9;
        double remain = (double)BCAST_SEC - elapsed;
        if (remain < 0.0) remain = 0.0;

        struct timeval tv;
        tv.tv_sec = (long)remain;
        tv.tv_usec = (long)((remain - (double)tv.tv_sec) * 1e6);

        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (g_shutdown) break;

        /* Periodic broadcast check */
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - g_last_bcast.tv_sec) +
                  (double)(now.tv_nsec - g_last_bcast.tv_nsec) / 1e9;
        if (elapsed >= (double)BCAST_SEC) {
            broadcast_prices("PERIODIC");
        }

        if (ret == 0) continue;

        /* Accept new TCP connections */
        if (FD_ISSET(g_tcp_fd, &rfds)) {
            struct sockaddr_in cli;
            socklen_t clen = sizeof(cli);
            int cfd = accept(g_tcp_fd, (struct sockaddr *)&cli, &clen);
            if (cfd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (g_clients[i].fd < 0) { slot = i; break; }
                }
                if (slot >= 0) {
                    memset(&g_clients[slot], 0, sizeof(client_t));
                    g_clients[slot].fd = cfd;
                    inet_ntop(AF_INET, &cli.sin_addr,
                              g_clients[slot].ip, INET_ADDRSTRLEN);
                    server_log("[SERVER] CLIENT_CONNECTED fd=%d ip=%s",
                               cfd, g_clients[slot].ip);
                } else {
                    close(cfd);
                }
            }
        }

        /* Handle data from connected clients */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].fd >= 0 && FD_ISSET(g_clients[i].fd, &rfds)) {
                process_client_data(i);
            }
        }
    }

    /* ===== Shutdown sequence ===== */
    server_log("[SERVER] SHUTDOWN signal=SIGINT");

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd >= 0) {
            send_to_client(g_clients[i].fd, "SERVER SHUTDOWN\n");
            close(g_clients[i].fd);
            g_clients[i].fd = -1;
        }
    }

    close(g_tcp_fd);
    close(g_udp_fd);

    server_log("[SERVER] CLEANUP_DONE clients=0");

    if (g_logfp) fclose(g_logfp);
    return 0;
}
