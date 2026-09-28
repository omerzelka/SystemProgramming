#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_LINE        512
#define MAX_INGREDIENTS 256
#define MAX_SPELLBOOK   256
#define USERNAME_LEN    32
#define INGREDIENT_LEN  32

typedef struct {
    char name[INGREDIENT_LEN];
    int  qty;
} ingredient_t;

typedef struct {
    char name[INGREDIENT_LEN];
    int  qty;
} spell_entry_t;

typedef struct {
    int          fd;
    char         username[USERNAME_LEN];
    char         type[16];           /* WIZARD or PROFESSOR */
    char         line_buf[MAX_LINE + 1];
    int          buf_len;
    int          toolong;            /* set when line exceeded MAX_LINE */
    time_t       last_active;
    int          enrolled;
    spell_entry_t spellbook[MAX_SPELLBOOK];
    int          spell_count;
} client_t;

static volatile sig_atomic_t g_shutdown    = 0;
static ingredient_t           g_ing[MAX_INGREDIENTS];
static int                    g_ing_count  = 0;
static client_t             **g_clients    = NULL;
static int                    g_max_clients= 0;
static int                    g_cli_count  = 0;
static int                    g_timeout    = 0;
static FILE                  *g_logfp      = NULL;

static void sigint_handler(int sig) { (void)sig; g_shutdown = 1; }

static void log_event(const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    fprintf(g_logfp, "[%s] [SERVER] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logfp, fmt, ap);
    va_end(ap);
    fprintf(g_logfp, "\n");
    fflush(g_logfp);
}

static int send_all(int fd, const char *msg) {
    size_t len = strlen(msg), sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, msg + sent, len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int find_ing(const char *name) {
    for (int i = 0; i < g_ing_count; i++)
        if (strcmp(g_ing[i].name, name) == 0) return i;
    return -1;
}

static spell_entry_t *get_spell(client_t *c, const char *name, int create) {
    for (int i = 0; i < c->spell_count; i++)
        if (strcmp(c->spellbook[i].name, name) == 0) return &c->spellbook[i];
    if (!create || c->spell_count >= MAX_SPELLBOOK) return NULL;
    spell_entry_t *e = &c->spellbook[c->spell_count++];
    strncpy(e->name, name, INGREDIENT_LEN - 1);
    e->name[INGREDIENT_LEN - 1] = '\0';
    e->qty = 0;
    return e;
}

static int is_name_taken(const char *uname) {
    for (int i = 0; i < g_max_clients; i++)
        if (g_clients[i] && g_clients[i]->enrolled &&
            strcmp(g_clients[i]->username, uname) == 0) return 1;
    return 0;
}

/* Returns 1 if client sent APPARATE and should be disconnected. */
static int handle_line(client_t *c, char *line) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
        line[--len] = '\0';

    c->last_active = time(NULL);

    char resp[2048];

    /* ── not yet enrolled ───────────────────────────────────────────────── */
    if (!c->enrolled) {
        char type[16], uname[USERNAME_LEN];
        if (sscanf(line, "ENROLL %15s %31s", type, uname) == 2 &&
            (strcmp(type, "WIZARD") == 0 || strcmp(type, "PROFESSOR") == 0)) {
            if (is_name_taken(uname)) {
                send_all(c->fd, "ERR ENROLL name_taken\n");
                return 0;
            }
            strncpy(c->username, uname, USERNAME_LEN - 1);
            c->username[USERNAME_LEN - 1] = '\0';
            strncpy(c->type, type, 15);
            c->type[15] = '\0';
            c->enrolled = 1;
            log_event("ENROLL username=%s type=%s fd=%d", uname, type, c->fd);
            snprintf(resp, sizeof(resp), "OK ENROLL %s\n", uname);
            send_all(c->fd, resp);
        } else {
            send_all(c->fd, "ERR NOT_ENROLLED\n");
        }
        return 0;
    }

    /* ── APPARATE (both types) ──────────────────────────────────────────── */
    if (strcmp(line, "APPARATE") == 0) {
        send_all(c->fd, "OK APPARATE\n");
        return 1;
    }

    /* ── WIZARD commands ────────────────────────────────────────────────── */
    if (strcmp(c->type, "WIZARD") == 0) {
        char ing[INGREDIENT_LEN];
        int  qty;

        if (sscanf(line, "BREW %31s %d", ing, &qty) == 2) {
            int idx = find_ing(ing);
            if (idx < 0) { send_all(c->fd, "ERR UNKNOWN_INGREDIENT\n"); return 0; }
            if (qty <= 0) { send_all(c->fd, "ERR UNKNOWN_INGREDIENT\n"); return 0; }
            int old = g_ing[idx].qty;
            g_ing[idx].qty += qty;
            spell_entry_t *e = get_spell(c, ing, 1);
            if (e) e->qty += qty;
            log_event("BREW wizard=%s ingredient=%s qty=%d old_qty=%d new_qty=%d",
                      c->username, ing, qty, old, g_ing[idx].qty);
            snprintf(resp, sizeof(resp), "OK BREW %s %d %d\n",
                     ing, qty, g_ing[idx].qty);
            send_all(c->fd, resp);

        } else if (sscanf(line, "CONSUME %31s %d", ing, &qty) == 2) {
            int idx = find_ing(ing);
            if (idx < 0) { send_all(c->fd, "ERR UNKNOWN_INGREDIENT\n"); return 0; }
            if (qty <= 0) { send_all(c->fd, "ERR INSUFFICIENT_INGREDIENTS\n"); return 0; }
            spell_entry_t *e = get_spell(c, ing, 0);
            int has = e ? e->qty : 0;
            if (has < qty) { send_all(c->fd, "ERR INSUFFICIENT_INGREDIENTS\n"); return 0; }
            int old = g_ing[idx].qty;
            g_ing[idx].qty -= qty;
            if (g_ing[idx].qty < 0) g_ing[idx].qty = 0;
            e->qty -= qty;
            log_event("CONSUME wizard=%s ingredient=%s qty=%d old_qty=%d new_qty=%d",
                      c->username, ing, qty, old, g_ing[idx].qty);
            snprintf(resp, sizeof(resp), "OK CONSUME %s %d %d\n",
                     ing, qty, g_ing[idx].qty);
            send_all(c->fd, resp);

        } else if (strcmp(line, "SPELLBOOK") == 0) {
            char buf[8192];
            int pos = 0, any = 0;
            for (int i = 0; i < c->spell_count; i++) {
                if (c->spellbook[i].qty > 0) {
                    if (!any) {
                        pos += snprintf(buf + pos, (int)sizeof(buf) - pos,
                                        "OK SPELLBOOK ");
                        any = 1;
                    } else {
                        buf[pos++] = ',';
                    }
                    pos += snprintf(buf + pos, (int)sizeof(buf) - pos,
                                    "%s:%d", c->spellbook[i].name, c->spellbook[i].qty);
                }
            }
            if (!any) {
                send_all(c->fd, "OK SPELLBOOK EMPTY\n");
            } else {
                if (pos < (int)sizeof(buf) - 1) { buf[pos++] = '\n'; buf[pos] = '\0'; }
                send_all(c->fd, buf);
            }

        } else if (strncmp(line, "INSPECT", 7) == 0 ||
                   strcmp(line, "SCROLL")  == 0 ||
                   strcmp(line, "ROSTER")  == 0) {
            send_all(c->fd, "ERR UNAUTHORIZED\n");

        } else {
            snprintf(resp, sizeof(resp), "ERR UNKNOWN %s\n", line);
            send_all(c->fd, resp);
        }

    /* ── PROFESSOR commands ─────────────────────────────────────────────── */
    } else {
        char ing[INGREDIENT_LEN];

        if (sscanf(line, "INSPECT %31s", ing) == 1) {
            int idx = find_ing(ing);
            if (idx < 0) { send_all(c->fd, "ERR UNKNOWN_INGREDIENT\n"); return 0; }
            log_event("INSPECT professor=%s ingredient=%s qty=%d",
                      c->username, ing, g_ing[idx].qty);
            snprintf(resp, sizeof(resp), "OK INSPECT %s %d\n", ing, g_ing[idx].qty);
            send_all(c->fd, resp);

        } else if (strcmp(line, "SCROLL") == 0) {
            char buf[8192];
            int pos = snprintf(buf, sizeof(buf), "OK SCROLL ");
            for (int i = 0; i < g_ing_count; i++) {
                if (i > 0) buf[pos++] = ',';
                pos += snprintf(buf + pos, (int)sizeof(buf) - pos,
                                "%s:%d", g_ing[i].name, g_ing[i].qty);
            }
            if (pos < (int)sizeof(buf) - 1) { buf[pos++] = '\n'; buf[pos] = '\0'; }
            log_event("SCROLL professor=%s ingredients=%d", c->username, g_ing_count);
            send_all(c->fd, buf);

        } else if (strcmp(line, "ROSTER") == 0) {
            char buf[4096];
            int pos = snprintf(buf, sizeof(buf), "OK ROSTER ");
            int first = 1, count = 0;
            for (int i = 0; i < g_max_clients; i++) {
                if (g_clients[i] && g_clients[i]->enrolled) {
                    if (!first) buf[pos++] = ',';
                    pos += snprintf(buf + pos, (int)sizeof(buf) - pos,
                                    "%s", g_clients[i]->username);
                    first = 0; count++;
                }
            }
            if (pos < (int)sizeof(buf) - 1) { buf[pos++] = '\n'; buf[pos] = '\0'; }
            log_event("ROSTER professor=%s clients=%d", c->username, count);
            send_all(c->fd, buf);

        } else if (strncmp(line, "BREW", 4)    == 0 ||
                   strncmp(line, "CONSUME", 7) == 0 ||
                   strcmp(line, "SPELLBOOK")   == 0) {
            send_all(c->fd, "ERR UNAUTHORIZED\n");

        } else {
            snprintf(resp, sizeof(resp), "ERR UNKNOWN %s\n", line);
            send_all(c->fd, resp);
        }
    }
    return 0;
}

static void remove_client(int idx, const char *reason) {
    client_t *c = g_clients[idx];
    if (!c) return;
    log_event("CLIENT_DISCONNECTED username=%s reason=%s",
              c->enrolled ? c->username : "(unknown)", reason);
    close(c->fd);
    free(c);
    g_clients[idx] = NULL;
    g_cli_count--;
}

static void load_ingredients(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror("fopen ingredients"); exit(1); }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char name[INGREDIENT_LEN];
        int  qty;
        if (sscanf(line, "%31s %d", name, &qty) == 2 && qty > 0 &&
            g_ing_count < MAX_INGREDIENTS) {
            strncpy(g_ing[g_ing_count].name, name, INGREDIENT_LEN - 1);
            g_ing[g_ing_count].name[INGREDIENT_LEN - 1] = '\0';
            g_ing[g_ing_count].qty = qty;
            g_ing_count++;
        }
    }
    fclose(f);
}

int main(int argc, char *argv[]) {
    int   port = -1, max_clients = -1, timeout_val = -1;
    char *ing_path = NULL, *log_path = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:s:l:n:t:")) != -1) {
        switch (opt) {
        case 'p': port        = atoi(optarg); break;
        case 's': ing_path    = optarg;       break;
        case 'l': log_path    = optarg;       break;
        case 'n': max_clients = atoi(optarg); break;
        case 't': timeout_val = atoi(optarg); break;
        default:
            fprintf(stderr,
                "Usage: %s -p port -s ingredients.txt -l logfile "
                "-n max_clients -t timeout\n", argv[0]);
            exit(1);
        }
    }

    if (port < 1024 || !ing_path || !log_path ||
        max_clients < 1 || timeout_val < 1) {
        fprintf(stderr,
            "Usage: %s -p port -s ingredients.txt -l logfile "
            "-n max_clients -t timeout\n", argv[0]);
        exit(1);
    }

    g_logfp = fopen(log_path, "a");
    if (!g_logfp) { perror("fopen logfile"); exit(1); }

    load_ingredients(ing_path);

    g_max_clients = max_clients;
    g_timeout     = timeout_val;
    g_clients     = calloc((size_t)max_clients, sizeof(client_t *));
    if (!g_clients) { perror("calloc"); exit(1); }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    int optval = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family      = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port        = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_fd, 32) < 0) {
        perror("listen"); exit(1);
    }

    printf("Hogwarts is ready. Port: %d | Max Clients: %d | Timeout: %ds\n",
           port, max_clients, timeout_val);
    fflush(stdout);

    log_event("SERVER_STARTED port=%d max_clients=%d timeout=%d ingredients=%d",
              port, max_clients, timeout_val, g_ing_count);

    /* ── main select() loop ─────────────────────────────────────────────── */
    while (!g_shutdown) {

        /* 1. Check and disconnect idle clients */
        time_t now = time(NULL);
        for (int i = 0; i < g_max_clients; i++) {
            if (!g_clients[i]) continue;
            time_t elapsed = now - g_clients[i]->last_active;
            if (elapsed >= (time_t)g_timeout) {
                client_t *c = g_clients[i];
                send_all(c->fd, "TIMEOUT DISCONNECT\n");
                log_event("TIMEOUT username=%s fd=%d elapsed=%lds",
                          c->enrolled ? c->username : "(unknown)",
                          c->fd, (long)elapsed);
                remove_client(i, "timeout");
            }
        }

        /* 2. Build fd_set and compute minimum remaining timeout */
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        int max_fd = server_fd;

        time_t min_rem = (time_t)g_timeout;
        now = time(NULL);
        for (int i = 0; i < g_max_clients; i++) {
            if (!g_clients[i]) continue;
            FD_SET(g_clients[i]->fd, &readfds);
            if (g_clients[i]->fd > max_fd) max_fd = g_clients[i]->fd;
            time_t elapsed = now - g_clients[i]->last_active;
            time_t rem     = (time_t)g_timeout - elapsed;
            if (rem < 1) rem = 1;
            if (rem < min_rem) min_rem = rem;
        }

        struct timeval tv;
        tv.tv_sec  = min_rem;
        tv.tv_usec = 0;

        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* 3. Accept new connections */
        if (FD_ISSET(server_fd, &readfds)) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int new_fd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
            if (new_fd >= 0) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
                log_event("CLIENT_CONNECTED fd=%d ip=%s", new_fd, ip);

                if (g_cli_count >= g_max_clients) {
                    send_all(new_fd, "ERR HOGWARTS_FULL\n");
                    log_event("REJECTED fd=%d ip=%s reason=HOGWARTS_FULL", new_fd, ip);
                    close(new_fd);
                } else {
                    client_t *c = calloc(1, sizeof(client_t));
                    if (!c) { close(new_fd); }
                    else {
                        c->fd          = new_fd;
                        c->last_active = time(NULL);
                        for (int i = 0; i < g_max_clients; i++) {
                            if (!g_clients[i]) { g_clients[i] = c; break; }
                        }
                        g_cli_count++;
                    }
                }
            }
        }

        /* 4. Handle client data */
        for (int i = 0; i < g_max_clients; i++) {
            if (!g_clients[i]) continue;
            if (!FD_ISSET(g_clients[i]->fd, &readfds)) continue;

            client_t *c = g_clients[i];
            char tmp[MAX_LINE];
            ssize_t n = read(c->fd, tmp, sizeof(tmp));

            if (n <= 0) {
                remove_client(i, "hangup");
                continue;
            }

            int disc = 0;
            for (int j = 0; j < (int)n && !disc; j++) {
                char ch = tmp[j];

                if (c->toolong) {
                    if (ch == '\n') { c->toolong = 0; c->buf_len = 0; }
                    continue;
                }

                if (c->buf_len >= MAX_LINE) {
                    send_all(c->fd, "ERR TOOLONG\n");
                    c->toolong = 1;
                    c->buf_len = 0;
                    continue;
                }

                c->line_buf[c->buf_len++] = ch;
                if (ch == '\n') {
                    c->line_buf[c->buf_len] = '\0';
                    disc = handle_line(c, c->line_buf);
                    c->buf_len = 0;
                }
            }

            if (disc) {
                log_event("CLIENT_DISCONNECTED username=%s reason=APPARATE",
                          c->enrolled ? c->username : "(unknown)");
                close(c->fd);
                free(c);
                g_clients[i] = NULL;
                g_cli_count--;
            }
        }
    }

    /* ── shutdown ───────────────────────────────────────────────────────── */
    for (int i = 0; i < g_max_clients; i++) {
        if (!g_clients[i]) continue;
        send_all(g_clients[i]->fd, "SERVER SHUTDOWN\n");
        log_event("CLIENT_DISCONNECTED username=%s reason=shutdown",
                  g_clients[i]->enrolled ? g_clients[i]->username : "(unknown)");
        close(g_clients[i]->fd);
        free(g_clients[i]);
        g_clients[i] = NULL;
    }
    close(server_fd);
    log_event("SHUTDOWN signal=SIGINT");
    log_event("CLEANUP_DONE clients=0");
    fclose(g_logfp);
    free(g_clients);
    return 0;
}
