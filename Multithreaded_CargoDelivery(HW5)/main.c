#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_NAME_LEN 33
#define UNIT_MS 500
#define MAX_ORDERS 1024

typedef enum {
  PRIORITY_EXPRESS = 1,
  PRIORITY_STANDARD = 2,
  PRIORITY_ECONOMY = 3
} Priority;

typedef struct {
  int id;
  char name[MAX_NAME_LEN];
  Priority priority;
  int duration;
} Order;

typedef struct {
  Order items[MAX_ORDERS];
  int size;
} PriorityQueue;

typedef struct {
  int completed;
  long total_time_ms;
} CourierStats;

static PriorityQueue pq;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static _Atomic int completed_orders = 0;
static _Atomic int cancelled_orders = 0;
static _Atomic long total_delivery_time = 0;

static volatile sig_atomic_t shutdown_flag = 0;

static int all_enqueued = 0;

static int total_orders = 0;

static CourierStats *courier_stats = NULL;
static int num_couriers = 0;

static int order_higher(const Order *a, const Order *b) {
  if (a->priority != b->priority)
    return a->priority < b->priority;
  return a->id < b->id;
}

static void swap_orders(Order *a, Order *b) {
  Order tmp = *a;
  *a = *b;
  *b = tmp;
}

static void heap_push(PriorityQueue *q, const Order *o) {
  q->items[q->size] = *o;
  int i = q->size++;
  while (i > 0) {
    int parent = (i - 1) / 2;
    if (order_higher(&q->items[i], &q->items[parent])) {
      swap_orders(&q->items[i], &q->items[parent]);
      i = parent;
    } else
      break;
  }
}

static Order heap_pop(PriorityQueue *q) {
  Order top = q->items[0];
  q->items[0] = q->items[--q->size];
  int i = 0;
  for (;;) {
    int best = i, l = 2 * i + 1, r = 2 * i + 2;
    if (l < q->size && order_higher(&q->items[l], &q->items[best]))
      best = l;
    if (r < q->size && order_higher(&q->items[r], &q->items[best]))
      best = r;
    if (best == i)
      break;
    swap_orders(&q->items[i], &q->items[best]);
    i = best;
  }
  return top;
}

static const char *priority_str(Priority p) {
  switch (p) {
  case PRIORITY_EXPRESS:
    return "EXPRESS";
  case PRIORITY_STANDARD:
    return "STANDARD";
  case PRIORITY_ECONOMY:
    return "ECONOMY";
  }
  return "UNKNOWN";
}

static Priority parse_priority(const char *s) {
  if (strcmp(s, "EXPRESS") == 0)
    return PRIORITY_EXPRESS;
  if (strcmp(s, "STANDARD") == 0)
    return PRIORITY_STANDARD;
  if (strcmp(s, "ECONOMY") == 0)
    return PRIORITY_ECONOMY;
  return (Priority)-1;
}

static void sigint_handler(int sig) {
  (void)sig;
  shutdown_flag = 1;
}

typedef struct {
  int id;
} CourierArg;

static void *courier_func(void *arg) {
  CourierArg *ca = (CourierArg *)arg;
  int cid = ca->id;

  for (;;) {
    Order order;
    int got_order = 0;

    pthread_mutex_lock(&queue_mutex);

    while (pq.size == 0 && !all_enqueued && !shutdown_flag) {
      pthread_mutex_lock(&log_mutex);
      printf("[COURIER-%d] WAITING\n", cid);
      pthread_mutex_unlock(&log_mutex);
      pthread_cond_wait(&queue_cond, &queue_mutex);
    }

    if (pq.size > 0 && !shutdown_flag) {
      order = heap_pop(&pq);
      got_order = 1;
    }

    pthread_mutex_unlock(&queue_mutex);

    if (!got_order) {
      /* No work and either shutdown or all_enqueued with empty queue */
      break;
    }

    /* Deliver */
    pthread_mutex_lock(&log_mutex);
    printf("[COURIER-%d] DELIVERY_START id=%d recipient=%s priority=%s\n", cid,
           order.id, order.name, priority_str(order.priority));
    pthread_mutex_unlock(&log_mutex);

    long delivery_ms = (long)order.duration * UNIT_MS;

    {
      long remaining_us = delivery_ms * 1000L;
      while (remaining_us > 0) {
        long chunk = remaining_us;
        if (chunk > 100000L)
          chunk = 100000L; /* 100 ms chunks */
        usleep((useconds_t)chunk);
        remaining_us -= chunk;
      }
    }

    atomic_fetch_add(&completed_orders, 1);
    atomic_fetch_add(&total_delivery_time, delivery_ms);

    courier_stats[cid - 1].completed++;
    courier_stats[cid - 1].total_time_ms += delivery_ms;

    pthread_mutex_lock(&log_mutex);
    printf("[COURIER-%d] DELIVERY_COMPLETE id=%d recipient=%s duration=%ldms\n",
           cid, order.id, order.name, delivery_ms);
    pthread_mutex_unlock(&log_mutex);
  }

  pthread_mutex_lock(&log_mutex);
  printf("[COURIER-%d] SHIFT_OVER\n", cid);
  pthread_mutex_unlock(&log_mutex);

  return NULL;
}

int main(int argc, char *argv[]) {
  char *input_file = NULL;
  char *stats_file = NULL;
  int opt;

  while ((opt = getopt(argc, argv, "n:i:s:")) != -1) {
    switch (opt) {
    case 'n':
      num_couriers = atoi(optarg);
      break;
    case 'i':
      input_file = optarg;
      break;
    case 's':
      stats_file = optarg;
      break;
    default:
      fprintf(stderr,
              "Usage: %s -n <num_couriers> -i <orders.txt> -s <stats.txt>\n",
              argv[0]);
      return 1;
    }
  }

  if (num_couriers < 1 || !input_file || !stats_file) {
    fprintf(stderr,
            "Usage: %s -n <num_couriers> -i <orders.txt> -s <stats.txt>\n",
            argv[0]);
    return 1;
  }

  /* ── Read input file ── */
  FILE *fp = fopen(input_file, "r");
  if (!fp) {
    fprintf(stderr, "Error: cannot open input file '%s': %s\n", input_file,
            strerror(errno));
    return 1;
  }

  pq.size = 0;
  Order orders_buf[MAX_ORDERS];
  int order_count = 0;

  {
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
      int id, dur;
      char name[MAX_NAME_LEN];
      char pri_str[16];

      /* Skip blank lines */
      if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
        continue;

      if (sscanf(line, "%d %32s %15s %d", &id, name, pri_str, &dur) != 4)
        continue;

      Priority p = parse_priority(pri_str);
      if ((int)p == -1 || dur <= 0)
        continue;

      Order o;
      o.id = id;
      strncpy(o.name, name, MAX_NAME_LEN - 1);
      o.name[MAX_NAME_LEN - 1] = '\0';
      o.priority = p;
      o.duration = dur;
      orders_buf[order_count++] = o;

      if (order_count >= MAX_ORDERS)
        break;
    }
  }
  fclose(fp);

  total_orders = order_count;

  {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
  }

  pthread_mutex_lock(&log_mutex);
  printf("[CARGOGTU] SHIFT_START couriers=%d orders=%d\n", num_couriers,
         total_orders);
  pthread_mutex_unlock(&log_mutex);

  pthread_mutex_lock(&queue_mutex);
  for (int i = 0; i < order_count; i++) {
    heap_push(&pq, &orders_buf[i]);
    pthread_mutex_lock(&log_mutex);
    printf(
        "[CARGOGTU] ORDER_QUEUED id=%d recipient=%s priority=%s duration=%d\n",
        orders_buf[i].id, orders_buf[i].name,
        priority_str(orders_buf[i].priority), orders_buf[i].duration);
    pthread_mutex_unlock(&log_mutex);
  }
  all_enqueued = 1;
  pthread_mutex_unlock(&queue_mutex);

  courier_stats = calloc((size_t)num_couriers, sizeof(CourierStats));
  if (!courier_stats) {
    perror("calloc");
    return 1;
  }

  pthread_t *threads = malloc(sizeof(pthread_t) * (size_t)num_couriers);
  CourierArg *cargs = malloc(sizeof(CourierArg) * (size_t)num_couriers);
  if (!threads || !cargs) {
    perror("malloc");
    free(courier_stats);
    return 1;
  }

  for (int i = 0; i < num_couriers; i++) {
    cargs[i].id = i + 1;
    pthread_create(&threads[i], NULL, courier_func, &cargs[i]);
  }

  pthread_mutex_lock(&queue_mutex);
  pthread_cond_broadcast(&queue_cond);
  pthread_mutex_unlock(&queue_mutex);

  while (!shutdown_flag) {
    /* Check if all work is done */
    if (atomic_load(&completed_orders) >= total_orders)
      break;
    usleep(50000); /* 50 ms */
  }

  if (shutdown_flag) {
    pthread_mutex_lock(&queue_mutex);

    pthread_mutex_lock(&log_mutex);
    printf("[CARGOGTU] SIGINT_RECEIVED pending_orders=%d\n", pq.size);
    pthread_mutex_unlock(&log_mutex);

    while (pq.size > 0) {
      Order o = heap_pop(&pq);
      atomic_fetch_add(&cancelled_orders, 1);
      pthread_mutex_lock(&log_mutex);
      printf("[CARGOGTU] ORDER_CANCELLED id=%d recipient=%s priority=%s\n",
             o.id, o.name, priority_str(o.priority));
      pthread_mutex_unlock(&log_mutex);
    }

    pthread_cond_broadcast(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
  }

  for (int i = 0; i < num_couriers; i++) {
    pthread_join(threads[i], NULL);
  }

  int comp = atomic_load(&completed_orders);
  int canc = atomic_load(&cancelled_orders);
  long ttime = atomic_load(&total_delivery_time);

  pthread_mutex_lock(&log_mutex);
  printf("[CARGOGTU] SHIFT_END completed=%d cancelled=%d total_time=%ldms\n",
         comp, canc, ttime);
  printf("[CARGOGTU] SHUTDOWN_COMPLETE\n");
  pthread_mutex_unlock(&log_mutex);

  FILE *sf = fopen(stats_file, "w");
  if (!sf) {
    fprintf(stderr, "Error: cannot open stats file '%s': %s\n", stats_file,
            strerror(errno));
    free(threads);
    free(cargs);
    free(courier_stats);
    return 1;
  }

  long avg = (comp > 0) ? (ttime / comp) : 0;

  fprintf(sf, "SHIFT_SUMMARY\n");
  fprintf(sf, "Total orders : %d\n", total_orders);
  fprintf(sf, "Completed    : %d\n", comp);
  fprintf(sf, "Cancelled    : %d\n", canc);
  fprintf(sf, "Total time   : %ldms\n", ttime);
  fprintf(sf, "Avg per order: %ldms\n", avg);
  fprintf(sf, "\nCOURIER_STATS\n");
  for (int i = 0; i < num_couriers; i++) {
    fprintf(sf, "Courier-%d completed=%d total_time=%ldms\n", i + 1,
            courier_stats[i].completed, courier_stats[i].total_time_ms);
  }

  fclose(sf);

  free(threads);
  free(cargs);
  free(courier_stats);

  pthread_mutex_destroy(&queue_mutex);
  pthread_mutex_destroy(&log_mutex);
  pthread_cond_destroy(&queue_cond);

  return 0;
}
