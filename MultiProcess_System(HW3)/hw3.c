#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_WORD_LEN 128

#ifndef MAP_ANONYMOUS
#ifndef MAP_ANON
#define MAP_ANON 0x1000
#endif
#define MAP_ANONYMOUS MAP_ANON
#endif

enum {
  REQUEST_FREE = 0,
  REQUEST_PENDING = 1,
  REQUEST_ONBOARD = 2,
  REQUEST_DONE = 3
};

typedef struct {
  int num_floors;
  int word_carriers_per_floor;
  int letter_carriers_per_floor;
  int sorting_processes_per_floor;
  int max_words_per_floor;
  int delivery_elevator_capacity;
  int reposition_elevator_capacity;
  char input_file[PATH_MAX];
  char output_file[PATH_MAX];
} Config;

typedef struct {
  char ch;
  int original_index;
  int claimed;
  int delivered;
} CharTask;

typedef struct {
  pthread_mutex_t lock;
  int word_id;
  int sorting_floor;
  int arrival_floor;
  int length;
  char text[MAX_WORD_LEN];
  int claimed;
  int admitted;
  int completed;
  pid_t completed_by_pid;
  int arrival_slot_held;
  int sorting_slot_held;
  int all_chars_claimed;
  int fixed_count;
  int delivered_count;
  char sorting_area[MAX_WORD_LEN];
  int slot_origin[MAX_WORD_LEN];
  unsigned char occupied[MAX_WORD_LEN];
  unsigned char fixed[MAX_WORD_LEN];
  CharTask tasks[MAX_WORD_LEN];
} SharedWord;

typedef struct {
  int active_words;
  int carrier_count;
  unsigned long sort_epoch;
  pthread_cond_t sorter_cv;
} FloorState;

typedef struct {
  int admissions;
} WordCarrierStats;

typedef struct {
  int delivered;
  int direct_deliveries;
  int elevator_deliveries;
  int repositions;
} LetterCarrierStats;

typedef struct {
  int sort_steps;
  int words_completed;
} SorterStats;

typedef struct {
  int state;
  int source_floor;
  int destination_floor;
  int word_index;
  int char_index;
  int carrier_slot;
  int direction;
  pthread_cond_t cv;
} DeliveryRequest;

typedef struct {
  int state;
  int source_floor;
  int destination_floor;
  int carrier_slot;
  int direction;
  pthread_cond_t cv;
} RepositionRequest;

typedef struct {
  Config cfg;
  int total_words;
  int next_rr_index;
  int completed_words;
  int total_unclaimed_chars;
  int retry_count;
  int transported_chars;
  int delivery_operations;
  int reposition_operations;
  int output_generated;
  int shutdown_requested;
  unsigned long word_epoch;
  unsigned long char_epoch;
  pthread_mutex_t lock;
  pthread_cond_t word_cv;
  pthread_cond_t char_cv;
  pthread_cond_t completion_cv;
  pthread_cond_t delivery_cv;
  pthread_cond_t reposition_cv;
} SharedCore;

typedef struct {
  pid_t pid;
  int slot;
  int floor;
} ChildInfo;

typedef struct {
  int word_id;
  char text[MAX_WORD_LEN];
  int sorting_floor;
} InputWord;

static SharedCore *g_core;
static FloorState *g_floors;
static SharedWord *g_words;
static WordCarrierStats *g_word_stats;
static LetterCarrierStats *g_letter_stats;
static SorterStats *g_sorter_stats;
static DeliveryRequest *g_delivery_requests;
static RepositionRequest *g_reposition_requests;

static volatile sig_atomic_t g_parent_interrupted = 0;
static SharedWord *g_sort_words = NULL;

static void fail(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(EXIT_FAILURE);
}

static void die_perror(const char *msg) {
  fprintf(stderr, "%s: %s\n", msg, strerror(errno));
  exit(EXIT_FAILURE);
}

static void check_pthread(int rc, const char *msg) {
  if (rc != 0) {
    errno = rc;
    die_perror(msg);
  }
}

static void *map_shared_region(size_t size) {
  void *ptr;

  ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1,
             0);
  if (ptr == MAP_FAILED) {
    die_perror("mmap");
  }

  memset(ptr, 0, size);
  return ptr;
}

static void parent_signal_handler(int signo) {
  (void)signo;
  g_parent_interrupted = 1;
}

static unsigned make_seed(int salt) {
  struct timespec ts;
  unsigned seed;

  if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
    ts.tv_nsec = 0;
    ts.tv_sec = time(NULL);
  }

  seed = (unsigned)ts.tv_nsec;
  seed ^= (unsigned)ts.tv_sec;
  seed ^= (unsigned)getpid();
  seed ^= (unsigned)(salt * 2654435761u);
  return seed;
}

static int random_floor_except(int current_floor, unsigned *seed) {
  int candidate;

  if (g_core->cfg.num_floors <= 1) {
    return current_floor;
  }

  candidate = rand_r(seed) % g_core->cfg.num_floors;
  if (candidate == current_floor) {
    candidate =
        (candidate + 1 + (rand_r(seed) % (g_core->cfg.num_floors - 1))) %
        g_core->cfg.num_floors;
  }

  return candidate;
}

static void initialize_mutex(pthread_mutex_t *mutex,
                             pthread_mutexattr_t *attr) {
  check_pthread(pthread_mutex_init(mutex, attr), "pthread_mutex_init");
}

static void initialize_cond(pthread_cond_t *cond, pthread_condattr_t *attr) {
  check_pthread(pthread_cond_init(cond, attr), "pthread_cond_init");
}

static void init_sync_objects(int num_floors, int total_words,
                              int total_word_carriers,
                              int total_letter_carriers, int total_sorters) {
  pthread_mutexattr_t mutex_attr;
  pthread_condattr_t cond_attr;
  int i;
  (void)total_word_carriers;
  (void)total_sorters;

  check_pthread(pthread_mutexattr_init(&mutex_attr), "pthread_mutexattr_init");
  check_pthread(
      pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED),
      "pthread_mutexattr_setpshared");

  check_pthread(pthread_condattr_init(&cond_attr), "pthread_condattr_init");
  check_pthread(pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED),
                "pthread_condattr_setpshared");

  initialize_mutex(&g_core->lock, &mutex_attr);
  initialize_cond(&g_core->word_cv, &cond_attr);
  initialize_cond(&g_core->char_cv, &cond_attr);
  initialize_cond(&g_core->completion_cv, &cond_attr);
  initialize_cond(&g_core->delivery_cv, &cond_attr);
  initialize_cond(&g_core->reposition_cv, &cond_attr);

  for (i = 0; i < num_floors; i++) {
    initialize_cond(&g_floors[i].sorter_cv, &cond_attr);
  }

  for (i = 0; i < total_words; i++) {
    initialize_mutex(&g_words[i].lock, &mutex_attr);
  }

  for (i = 0; i < total_letter_carriers; i++) {
    initialize_cond(&g_delivery_requests[i].cv, &cond_attr);
    initialize_cond(&g_reposition_requests[i].cv, &cond_attr);
  }

  check_pthread(pthread_condattr_destroy(&cond_attr),
                "pthread_condattr_destroy");
  check_pthread(pthread_mutexattr_destroy(&mutex_attr),
                "pthread_mutexattr_destroy");
}

static void usage(const char *progname) {
  fprintf(stderr,
          "Usage: %s -f <floors> -w <word_carriers> -l <letter_carriers> "
          "-s <sorters> -c <floor_capacity> -d <delivery_capacity> "
          "-r <reposition_capacity> -i <input_file> -o <output_file>\n",
          progname);
}

static void parse_positive_int(const char *arg, const char *name, int *value) {
  char *endptr = NULL;
  long parsed;

  errno = 0;
  parsed = strtol(arg, &endptr, 10);
  if (errno != 0 || endptr == arg || *endptr != '\0' || parsed < 1 ||
      parsed > INT_MAX) {
    fail("Invalid value for %s: %s", name, arg);
  }

  *value = (int)parsed;
}

static Config parse_args(int argc, char **argv) {
  Config cfg;
  int opt;
  bool have_f = false;
  bool have_w = false;
  bool have_l = false;
  bool have_s = false;
  bool have_c = false;
  bool have_d = false;
  bool have_r = false;
  bool have_i = false;
  bool have_o = false;

  memset(&cfg, 0, sizeof(cfg));

  while ((opt = getopt(argc, argv, "f:w:l:s:c:d:r:i:o:")) != -1) {
    switch (opt) {
    case 'f':
      parse_positive_int(optarg, "-f", &cfg.num_floors);
      have_f = true;
      break;
    case 'w':
      parse_positive_int(optarg, "-w", &cfg.word_carriers_per_floor);
      have_w = true;
      break;
    case 'l':
      parse_positive_int(optarg, "-l", &cfg.letter_carriers_per_floor);
      have_l = true;
      break;
    case 's':
      parse_positive_int(optarg, "-s", &cfg.sorting_processes_per_floor);
      have_s = true;
      break;
    case 'c':
      parse_positive_int(optarg, "-c", &cfg.max_words_per_floor);
      have_c = true;
      break;
    case 'd':
      parse_positive_int(optarg, "-d", &cfg.delivery_elevator_capacity);
      have_d = true;
      break;
    case 'r':
      parse_positive_int(optarg, "-r", &cfg.reposition_elevator_capacity);
      have_r = true;
      break;
    case 'i':
      if (snprintf(cfg.input_file, sizeof(cfg.input_file), "%s", optarg) >=
          (int)sizeof(cfg.input_file)) {
        fail("Input file path is too long");
      }
      have_i = true;
      break;
    case 'o':
      if (snprintf(cfg.output_file, sizeof(cfg.output_file), "%s", optarg) >=
          (int)sizeof(cfg.output_file)) {
        fail("Output file path is too long");
      }
      have_o = true;
      break;
    default:
      usage(argv[0]);
      exit(EXIT_FAILURE);
    }
  }

  if (!(have_f && have_w && have_l && have_s && have_c && have_d && have_r &&
        have_i && have_o)) {
    usage(argv[0]);
    exit(EXIT_FAILURE);
  }

  if (access(cfg.input_file, R_OK) != 0) {
    fail("Input file is not readable: %s", cfg.input_file);
  }

  return cfg;
}

static int is_lowercase_word(const char *word) {
  size_t i;

  if (word[0] == '\0') {
    return 0;
  }

  for (i = 0; word[i] != '\0'; i++) {
    if (!islower((unsigned char)word[i]) || !isalpha((unsigned char)word[i])) {
      return 0;
    }
  }

  return 1;
}

static InputWord *read_input_file(const Config *cfg, int *count_out) {
  FILE *fp;
  InputWord *words = NULL;
  size_t capacity = 0;
  int count = 0;
  char *line = NULL;
  size_t line_cap = 0;
  ssize_t len;

  fp = fopen(cfg->input_file, "r");
  if (fp == NULL) {
    die_perror("fopen input file");
  }

  while ((len = getline(&line, &line_cap, fp)) != -1) {
    InputWord entry;
    char extra[4];
    char word_buf[MAX_WORD_LEN];
    int matched;

    if (len == 0 || line[0] == '\n') {
      fail("Input file contains a blank line");
    }

    memset(&entry, 0, sizeof(entry));
    memset(word_buf, 0, sizeof(word_buf));
    memset(extra, 0, sizeof(extra));

    matched = sscanf(line, "%d %127s %d %3s", &entry.word_id, word_buf,
                     &entry.sorting_floor, extra);
    if (matched != 3) {
      fail("Invalid input line: %s", line);
    }

    if (!is_lowercase_word(word_buf)) {
      fail("Invalid word in input file: %s", word_buf);
    }

    if (entry.sorting_floor < 0 || entry.sorting_floor >= cfg->num_floors) {
      fail("Sorting floor %d is outside 0..%d", entry.sorting_floor,
           cfg->num_floors - 1);
    }

    if (snprintf(entry.text, sizeof(entry.text), "%s", word_buf) >=
        (int)sizeof(entry.text)) {
      fail("Word is too long: %s", word_buf);
    }

    if ((size_t)count == capacity) {
      size_t new_capacity = (capacity == 0) ? 16 : capacity * 2;
      InputWord *tmp = realloc(words, new_capacity * sizeof(*tmp));
      if (tmp == NULL) {
        die_perror("realloc");
      }
      words = tmp;
      capacity = new_capacity;
    }

    words[count++] = entry;
  }

  free(line);

  if (fclose(fp) != 0) {
    die_perror("fclose input file");
  }

  if (count == 0) {
    fail("Input file is empty");
  }

  *count_out = count;
  return words;
}

static void initialize_word_from_input(SharedWord *dst, const InputWord *src) {
  int i;

  memset(dst, 0, sizeof(*dst));
  dst->word_id = src->word_id;
  dst->sorting_floor = src->sorting_floor;
  dst->arrival_floor = -1;
  dst->length = (int)strlen(src->text);
  memcpy(dst->text, src->text, (size_t)dst->length + 1);

  for (i = 0; i < dst->length; i++) {
    dst->tasks[i].ch = dst->text[i];
    dst->tasks[i].original_index = i;
    dst->slot_origin[i] = -1;
  }
}

static void initialize_shared_state(const Config *cfg, InputWord *input_words,
                                    int total_words) {
  int total_word_carriers;
  int total_letter_carriers;
  int total_sorters;
  int i;

  total_word_carriers = cfg->num_floors * cfg->word_carriers_per_floor;
  total_letter_carriers = cfg->num_floors * cfg->letter_carriers_per_floor;
  total_sorters = cfg->num_floors * cfg->sorting_processes_per_floor;

  g_core = map_shared_region(sizeof(*g_core));
  g_floors = map_shared_region((size_t)cfg->num_floors * sizeof(*g_floors));
  g_words = map_shared_region((size_t)total_words * sizeof(*g_words));
  g_word_stats =
      map_shared_region((size_t)total_word_carriers * sizeof(*g_word_stats));
  g_letter_stats = map_shared_region((size_t)total_letter_carriers *
                                     sizeof(*g_letter_stats));
  g_sorter_stats =
      map_shared_region((size_t)total_sorters * sizeof(*g_sorter_stats));
  g_delivery_requests = map_shared_region((size_t)total_letter_carriers *
                                          sizeof(*g_delivery_requests));
  g_reposition_requests = map_shared_region((size_t)total_letter_carriers *
                                            sizeof(*g_reposition_requests));

  g_core->cfg = *cfg;
  g_core->total_words = total_words;

  for (i = 0; i < cfg->num_floors; i++) {
    g_floors[i].carrier_count = cfg->letter_carriers_per_floor;
  }

  for (i = 0; i < total_words; i++) {
    initialize_word_from_input(&g_words[i], &input_words[i]);
  }

  init_sync_objects(cfg->num_floors, total_words, total_word_carriers,
                    total_letter_carriers, total_sorters);
}

static void increment_word_epoch_locked(void) {
  g_core->word_epoch++;
  check_pthread(pthread_cond_broadcast(&g_core->word_cv),
                "pthread_cond_broadcast word_cv");
}

static void increment_char_epoch_locked(void) {
  g_core->char_epoch++;
  check_pthread(pthread_cond_broadcast(&g_core->char_cv),
                "pthread_cond_broadcast char_cv");
}

static void notify_sorters_locked(int floor) {
  g_floors[floor].sort_epoch++;
  check_pthread(pthread_cond_broadcast(&g_floors[floor].sorter_cv),
                "pthread_cond_broadcast sorter_cv");
}

static int word_can_be_admitted_locked(int word_index, int arrival_floor) {
  SharedWord *word = &g_words[word_index];
  int sorting_floor = word->sorting_floor;

  if (arrival_floor == sorting_floor) {
    return g_floors[arrival_floor].active_words <
           g_core->cfg.max_words_per_floor;
  }

  return g_floors[arrival_floor].active_words <
             g_core->cfg.max_words_per_floor &&
         g_floors[sorting_floor].active_words < g_core->cfg.max_words_per_floor;
}

static int try_admit_word_locked(int arrival_floor) {
  int scan_count;

  for (scan_count = 0; scan_count < g_core->total_words; scan_count++) {
    int idx = g_core->next_rr_index;
    SharedWord *word = &g_words[idx];

    g_core->next_rr_index = (g_core->next_rr_index + 1) % g_core->total_words;

    if (word->completed || word->admitted || word->claimed) {
      continue;
    }

    word->claimed = 1;
    if (!word_can_be_admitted_locked(idx, arrival_floor)) {
      word->claimed = 0;
      g_core->retry_count++;
      continue;
    }

    word->admitted = 1;
    word->arrival_floor = arrival_floor;
    word->all_chars_claimed = 0;
    g_core->total_unclaimed_chars += word->length;

    if (arrival_floor == word->sorting_floor) {
      word->arrival_slot_held = 0;
      word->sorting_slot_held = 1;
      g_floors[arrival_floor].active_words++;
      notify_sorters_locked(arrival_floor);
    } else {
      word->arrival_slot_held = 1;
      word->sorting_slot_held = 1;
      g_floors[arrival_floor].active_words++;
      g_floors[word->sorting_floor].active_words++;
      notify_sorters_locked(word->sorting_floor);
    }

    increment_word_epoch_locked();
    increment_char_epoch_locked();
    return idx;
  }

  return -1;
}

static int claim_random_character_from_floor(int floor, unsigned *seed,
                                             int *word_idx_out,
                                             int *char_idx_out, char *ch_out,
                                             int *origin_out, int *word_id_out,
                                             int *dest_floor_out) {
  int start;
  int attempt;

  start = rand_r(seed) % g_core->total_words;
  for (attempt = 0; attempt < g_core->total_words; attempt++) {
    int idx = (start + attempt) % g_core->total_words;
    SharedWord *word = &g_words[idx];
    int j;
    int seen = 0;
    int chosen = -1;
    int release_arrival = 0;

    check_pthread(pthread_mutex_lock(&word->lock), "pthread_mutex_lock word");

    if (!word->admitted || word->completed || word->arrival_floor != floor ||
        word->all_chars_claimed) {
      check_pthread(pthread_mutex_unlock(&word->lock),
                    "pthread_mutex_unlock word");
      continue;
    }

    for (j = 0; j < word->length; j++) {
      if (!word->tasks[j].claimed) {
        seen++;
        if ((rand_r(seed) % seen) == 0) {
          chosen = j;
        }
      }
    }

    if (chosen < 0) {
      if (!word->all_chars_claimed) {
        word->all_chars_claimed = 1;
      }
      check_pthread(pthread_mutex_unlock(&word->lock),
                    "pthread_mutex_unlock word");
      continue;
    }

    word->tasks[chosen].claimed = 1;

    for (j = 0; j < word->length; j++) {
      if (!word->tasks[j].claimed) {
        break;
      }
    }

    if (j == word->length) {
      word->all_chars_claimed = 1;
      if (word->arrival_slot_held) {
        word->arrival_slot_held = 0;
        release_arrival = 1;
      }
    }

    *word_idx_out = idx;
    *char_idx_out = chosen;
    *ch_out = word->tasks[chosen].ch;
    *origin_out = word->tasks[chosen].original_index;
    *word_id_out = word->word_id;
    *dest_floor_out = word->sorting_floor;

    check_pthread(pthread_mutex_unlock(&word->lock),
                  "pthread_mutex_unlock word");

    check_pthread(pthread_mutex_lock(&g_core->lock), "pthread_mutex_lock core");
    g_core->total_unclaimed_chars--;
    if (release_arrival) {
      g_floors[floor].active_words--;
      increment_word_epoch_locked();
    }
    increment_char_epoch_locked();
    check_pthread(pthread_mutex_unlock(&g_core->lock),
                  "pthread_mutex_unlock core");

    return 1;
  }

  return 0;
}

static int place_character_on_sorting_floor(int word_idx, int char_idx,
                                            int carrier_slot, int floor) {
  SharedWord *word = &g_words[word_idx];
  int i;
  int placed_index = -1;

  (void)carrier_slot;

  check_pthread(pthread_mutex_lock(&word->lock), "pthread_mutex_lock word");

  if (word->tasks[char_idx].delivered) {
    check_pthread(pthread_mutex_unlock(&word->lock),
                  "pthread_mutex_unlock word");
    return -1;
  }

  for (i = 0; i < word->length; i++) {
    if (!word->occupied[i] && !word->fixed[i]) {
      word->occupied[i] = 1;
      word->sorting_area[i] = word->tasks[char_idx].ch;
      word->slot_origin[i] = word->tasks[char_idx].original_index;
      word->tasks[char_idx].delivered = 1;
      word->delivered_count++;
      placed_index = i;
      break;
    }
  }

  check_pthread(pthread_mutex_unlock(&word->lock), "pthread_mutex_unlock word");

  if (placed_index < 0) {
    return -1;
  }

  check_pthread(pthread_mutex_lock(&g_core->lock), "pthread_mutex_lock core");
  g_core->transported_chars++;
  increment_char_epoch_locked();
  notify_sorters_locked(floor);
  check_pthread(pthread_mutex_unlock(&g_core->lock),
                "pthread_mutex_unlock core");

  return placed_index;
}

static int request_delivery_trip(int carrier_slot, int source_floor,
                                 int destination_floor, int word_idx,
                                 int char_idx) {
  DeliveryRequest *req = &g_delivery_requests[carrier_slot];

  check_pthread(pthread_mutex_lock(&g_core->lock), "pthread_mutex_lock core");

  req->state = REQUEST_PENDING;
  req->source_floor = source_floor;
  req->destination_floor = destination_floor;
  req->word_index = word_idx;
  req->char_index = char_idx;
  req->carrier_slot = carrier_slot;
  req->direction = (destination_floor >= source_floor) ? 1 : -1;

  check_pthread(pthread_cond_signal(&g_core->delivery_cv),
                "pthread_cond_signal delivery_cv");

  while (!g_core->shutdown_requested && req->state != REQUEST_DONE) {
    check_pthread(pthread_cond_wait(&req->cv, &g_core->lock),
                  "pthread_cond_wait delivery request");
  }

  if (g_core->shutdown_requested) {
    req->state = REQUEST_FREE;
    check_pthread(pthread_mutex_unlock(&g_core->lock),
                  "pthread_mutex_unlock core");
    return 0;
  }

  req->state = REQUEST_FREE;
  check_pthread(pthread_mutex_unlock(&g_core->lock),
                "pthread_mutex_unlock core");
  return 1;
}

static int request_reposition_trip(int carrier_slot, int source_floor,
                                   int destination_floor) {
  RepositionRequest *req = &g_reposition_requests[carrier_slot];

  check_pthread(pthread_mutex_lock(&g_core->lock), "pthread_mutex_lock core");

  req->state = REQUEST_PENDING;
  req->source_floor = source_floor;
  req->destination_floor = destination_floor;
  req->carrier_slot = carrier_slot;
  req->direction = (destination_floor >= source_floor) ? 1 : -1;

  check_pthread(pthread_cond_signal(&g_core->reposition_cv),
                "pthread_cond_signal reposition_cv");

  while (!g_core->shutdown_requested && req->state != REQUEST_DONE) {
    check_pthread(pthread_cond_wait(&req->cv, &g_core->lock),
                  "pthread_cond_wait reposition request");
  }

  if (g_core->shutdown_requested) {
    req->state = REQUEST_FREE;
    check_pthread(pthread_mutex_unlock(&g_core->lock),
                  "pthread_mutex_unlock core");
    return 0;
  }

  req->state = REQUEST_FREE;
  check_pthread(pthread_mutex_unlock(&g_core->lock),
                "pthread_mutex_unlock core");
  return 1;
}

static void update_carrier_floor(int old_floor, int new_floor) {
  if (old_floor == new_floor) {
    return;
  }

  check_pthread(pthread_mutex_lock(&g_core->lock), "pthread_mutex_lock core");
  g_floors[old_floor].carrier_count--;
  g_floors[new_floor].carrier_count++;
  check_pthread(pthread_mutex_unlock(&g_core->lock),
                "pthread_mutex_unlock core");
}

static void fix_slot_if_correct_locked(SharedWord *word, int idx) {
  if (!word->occupied[idx] || word->fixed[idx]) {
    return;
  }

  if (word->slot_origin[idx] == idx &&
      word->sorting_area[idx] == word->text[idx]) {
    word->fixed[idx] = 1;
    word->fixed_count++;
  }
}

static int sort_word_locked(SharedWord *word) {
  int progress = 0;
  int changed;

  do {
    int i;
    changed = 0;

    for (i = 0; i < word->length; i++) {
      int target;
      char tmp_char;
      int tmp_origin;

      if (!word->occupied[i] || word->fixed[i]) {
        continue;
      }

      fix_slot_if_correct_locked(word, i);
      if (word->fixed[i]) {
        progress = 1;
        changed = 1;
        continue;
      }

      target = word->slot_origin[i];
      if (target < 0 || target >= word->length || word->fixed[target]) {
        continue;
      }

      if (!word->occupied[target]) {
        word->occupied[target] = 1;
        word->sorting_area[target] = word->sorting_area[i];
        word->slot_origin[target] = word->slot_origin[i];
        word->occupied[i] = 0;
        word->sorting_area[i] = '\0';
        word->slot_origin[i] = -1;
        progress = 1;
        changed = 1;
        fix_slot_if_correct_locked(word, target);
        continue;
      }

      if (word->fixed[target]) {
        continue;
      }

      tmp_char = word->sorting_area[target];
      tmp_origin = word->slot_origin[target];
      word->sorting_area[target] = word->sorting_area[i];
      word->slot_origin[target] = word->slot_origin[i];
      word->sorting_area[i] = tmp_char;
      word->slot_origin[i] = tmp_origin;
      progress = 1;
      changed = 1;
      fix_slot_if_correct_locked(word, target);
      fix_slot_if_correct_locked(word, i);
    }
  } while (changed);

  return progress;
}

static int try_sort_word(int sorter_slot, int floor, unsigned *seed) {
  int start = rand_r(seed) % g_core->total_words;
  int attempt;

  for (attempt = 0; attempt < g_core->total_words; attempt++) {
    int idx = (start + attempt) % g_core->total_words;
    SharedWord *word = &g_words[idx];
    int progress;
    int completed_now = 0;
    int release_sorting_slot = 0;

    if (pthread_mutex_trylock(&word->lock) != 0) {
      continue;
    }

    if (!word->admitted || word->completed || word->sorting_floor != floor ||
        word->delivered_count == 0) {
      check_pthread(pthread_mutex_unlock(&word->lock),
                    "pthread_mutex_unlock word");
      continue;
    }

    progress = sort_word_locked(word);
    if (!word->completed && word->fixed_count == word->length) {
      word->completed = 1;
      word->completed_by_pid = getpid();
      completed_now = 1;
      if (word->sorting_slot_held) {
        word->sorting_slot_held = 0;
        release_sorting_slot = 1;
      }
    }

    check_pthread(pthread_mutex_unlock(&word->lock),
                  "pthread_mutex_unlock word");

    if (progress || completed_now) {
      g_sorter_stats[sorter_slot].sort_steps++;

      check_pthread(pthread_mutex_lock(&g_core->lock),
                    "pthread_mutex_lock core");
      notify_sorters_locked(floor);

      if (completed_now) {
        if (release_sorting_slot) {
          g_floors[floor].active_words--;
        }
        g_core->completed_words++;
        g_sorter_stats[sorter_slot].words_completed++;
        increment_word_epoch_locked();
        increment_char_epoch_locked();
        check_pthread(pthread_cond_broadcast(&g_core->completion_cv),
                      "pthread_cond_broadcast completion_cv");
      }

      check_pthread(pthread_mutex_unlock(&g_core->lock),
                    "pthread_mutex_unlock core");
      return 1;
    }
  }

  return 0;
}

static int any_delivery_request_locked(void) {
  int total_letter_carriers =
      g_core->cfg.num_floors * g_core->cfg.letter_carriers_per_floor;
  int i;

  for (i = 0; i < total_letter_carriers; i++) {
    if (g_delivery_requests[i].state == REQUEST_PENDING ||
        g_delivery_requests[i].state == REQUEST_ONBOARD) {
      return 1;
    }
  }

  return 0;
}

static int any_reposition_request_locked(void) {
  int total_letter_carriers =
      g_core->cfg.num_floors * g_core->cfg.letter_carriers_per_floor;
  int i;

  for (i = 0; i < total_letter_carriers; i++) {
    if (g_reposition_requests[i].state == REQUEST_PENDING ||
        g_reposition_requests[i].state == REQUEST_ONBOARD) {
      return 1;
    }
  }

  return 0;
}

static int first_pending_delivery_direction_locked(int current_floor) {
  int total_letter_carriers =
      g_core->cfg.num_floors * g_core->cfg.letter_carriers_per_floor;
  int i;

  for (i = 0; i < total_letter_carriers; i++) {
    DeliveryRequest *req = &g_delivery_requests[i];
    int anchor;

    if (req->state != REQUEST_PENDING) {
      continue;
    }

    anchor = req->source_floor;
    if (anchor > current_floor) {
      return 1;
    }
    if (anchor < current_floor) {
      return -1;
    }
    return (req->destination_floor >= current_floor) ? 1 : -1;
  }

  return 1;
}

static int first_pending_reposition_direction_locked(int current_floor) {
  int total_letter_carriers =
      g_core->cfg.num_floors * g_core->cfg.letter_carriers_per_floor;
  int i;

  for (i = 0; i < total_letter_carriers; i++) {
    RepositionRequest *req = &g_reposition_requests[i];
    int anchor;

    if (req->state != REQUEST_PENDING) {
      continue;
    }

    anchor = req->source_floor;
    if (anchor > current_floor) {
      return 1;
    }
    if (anchor < current_floor) {
      return -1;
    }
    return (req->destination_floor >= current_floor) ? 1 : -1;
  }

  return 1;
}

static int delivery_has_work_in_direction_locked(int current_floor,
                                                 int direction) {
  int total_letter_carriers =
      g_core->cfg.num_floors * g_core->cfg.letter_carriers_per_floor;
  int i;

  for (i = 0; i < total_letter_carriers; i++) {
    DeliveryRequest *req = &g_delivery_requests[i];

    if (req->state == REQUEST_PENDING) {
      if (direction > 0 && req->source_floor > current_floor) {
        return 1;
      }
      if (direction < 0 && req->source_floor < current_floor) {
        return 1;
      }
      if (req->source_floor == current_floor) {
        if (direction > 0 && req->destination_floor >= current_floor) {
          return 1;
        }
        if (direction < 0 && req->destination_floor <= current_floor) {
          return 1;
        }
      }
    } else if (req->state == REQUEST_ONBOARD) {
      if (direction > 0 && req->destination_floor > current_floor) {
        return 1;
      }
      if (direction < 0 && req->destination_floor < current_floor) {
        return 1;
      }
    }
  }

  return 0;
}

static int reposition_has_work_in_direction_locked(int current_floor,
                                                   int direction) {
  int total_letter_carriers =
      g_core->cfg.num_floors * g_core->cfg.letter_carriers_per_floor;
  int i;

  for (i = 0; i < total_letter_carriers; i++) {
    RepositionRequest *req = &g_reposition_requests[i];

    if (req->state == REQUEST_PENDING) {
      if (direction > 0 && req->source_floor > current_floor) {
        return 1;
      }
      if (direction < 0 && req->source_floor < current_floor) {
        return 1;
      }
      if (req->source_floor == current_floor) {
        if (direction > 0 && req->destination_floor >= current_floor) {
          return 1;
        }
        if (direction < 0 && req->destination_floor <= current_floor) {
          return 1;
        }
      }
    } else if (req->state == REQUEST_ONBOARD) {
      if (direction > 0 && req->destination_floor > current_floor) {
        return 1;
      }
      if (direction < 0 && req->destination_floor < current_floor) {
        return 1;
      }
    }
  }

  return 0;
}

static void delivery_elevator_main(void) {
  int current_floor = 0;
  int direction = 1;
  int total_letter_carriers =
      g_core->cfg.num_floors * g_core->cfg.letter_carriers_per_floor;

  signal(SIGINT, SIG_IGN);
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("[PID:%d] Delivery elevator process started\n", getpid());

  for (;;) {
    int i;
    int onboard = 0;

    check_pthread(pthread_mutex_lock(&g_core->lock), "pthread_mutex_lock core");

    while (!g_core->shutdown_requested && !any_delivery_request_locked()) {
      check_pthread(pthread_cond_wait(&g_core->delivery_cv, &g_core->lock),
                    "pthread_cond_wait delivery_cv");
    }

    if (g_core->shutdown_requested) {
      check_pthread(pthread_mutex_unlock(&g_core->lock),
                    "pthread_mutex_unlock core");
      _exit(EXIT_SUCCESS);
    }

    if (current_floor == 0) {
      direction = 1;
    } else if (current_floor == g_core->cfg.num_floors - 1) {
      direction = -1;
    }

    for (i = 0; i < total_letter_carriers; i++) {
      if (g_delivery_requests[i].state == REQUEST_ONBOARD &&
          g_delivery_requests[i].destination_floor == current_floor) {
        g_delivery_requests[i].state = REQUEST_DONE;
        g_core->delivery_operations++;
        check_pthread(pthread_cond_signal(&g_delivery_requests[i].cv),
                      "pthread_cond_signal delivery request");
      }
    }

    for (i = 0; i < total_letter_carriers; i++) {
      if (g_delivery_requests[i].state == REQUEST_ONBOARD) {
        onboard++;
      }
    }

    if (onboard == 0 &&
        !delivery_has_work_in_direction_locked(current_floor, direction)) {
      direction =
          delivery_has_work_in_direction_locked(current_floor, -direction)
              ? -direction
              : first_pending_delivery_direction_locked(current_floor);
    } else if (onboard > 0 && !delivery_has_work_in_direction_locked(
                                  current_floor, direction)) {
      direction = -direction;
    }

    if (onboard < g_core->cfg.delivery_elevator_capacity) {
      for (i = 0; i < total_letter_carriers &&
                  onboard < g_core->cfg.delivery_elevator_capacity;
           i++) {
        DeliveryRequest *req = &g_delivery_requests[i];

        if (req->state != REQUEST_PENDING ||
            req->source_floor != current_floor) {
          continue;
        }

        if ((direction > 0 && req->destination_floor < current_floor) ||
            (direction < 0 && req->destination_floor > current_floor)) {
          continue;
        }

        req->state = REQUEST_ONBOARD;
        onboard++;
      }
    }

    if (direction > 0 && current_floor < g_core->cfg.num_floors - 1) {
      current_floor++;
    } else if (direction < 0 && current_floor > 0) {
      current_floor--;
    }

    check_pthread(pthread_mutex_unlock(&g_core->lock),
                  "pthread_mutex_unlock core");
    usleep(20000);
  }
}

static void reposition_elevator_main(void) {
  int current_floor = 0;
  int direction = 1;
  int total_letter_carriers =
      g_core->cfg.num_floors * g_core->cfg.letter_carriers_per_floor;

  signal(SIGINT, SIG_IGN);
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("[PID:%d] Reposition elevator process started\n", getpid());

  for (;;) {
    int i;
    int onboard = 0;

    check_pthread(pthread_mutex_lock(&g_core->lock), "pthread_mutex_lock core");

    while (!g_core->shutdown_requested && !any_reposition_request_locked()) {
      check_pthread(pthread_cond_wait(&g_core->reposition_cv, &g_core->lock),
                    "pthread_cond_wait reposition_cv");
    }

    if (g_core->shutdown_requested) {
      check_pthread(pthread_mutex_unlock(&g_core->lock),
                    "pthread_mutex_unlock core");
      _exit(EXIT_SUCCESS);
    }

    if (current_floor == 0) {
      direction = 1;
    } else if (current_floor == g_core->cfg.num_floors - 1) {
      direction = -1;
    }

    for (i = 0; i < total_letter_carriers; i++) {
      if (g_reposition_requests[i].state == REQUEST_ONBOARD &&
          g_reposition_requests[i].destination_floor == current_floor) {
        g_reposition_requests[i].state = REQUEST_DONE;
        g_core->reposition_operations++;
        check_pthread(pthread_cond_signal(&g_reposition_requests[i].cv),
                      "pthread_cond_signal reposition request");
      }
    }

    for (i = 0; i < total_letter_carriers; i++) {
      if (g_reposition_requests[i].state == REQUEST_ONBOARD) {
        onboard++;
      }
    }

    if (onboard == 0 &&
        !reposition_has_work_in_direction_locked(current_floor, direction)) {
      direction =
          reposition_has_work_in_direction_locked(current_floor, -direction)
              ? -direction
              : first_pending_reposition_direction_locked(current_floor);
    } else if (onboard > 0 && !reposition_has_work_in_direction_locked(
                                  current_floor, direction)) {
      direction = -direction;
    }

    if (onboard < g_core->cfg.reposition_elevator_capacity) {
      for (i = 0; i < total_letter_carriers &&
                  onboard < g_core->cfg.reposition_elevator_capacity;
           i++) {
        RepositionRequest *req = &g_reposition_requests[i];

        if (req->state != REQUEST_PENDING ||
            req->source_floor != current_floor) {
          continue;
        }

        if ((direction > 0 && req->destination_floor < current_floor) ||
            (direction < 0 && req->destination_floor > current_floor)) {
          continue;
        }

        req->state = REQUEST_ONBOARD;
        onboard++;
      }
    }

    if (direction > 0 && current_floor < g_core->cfg.num_floors - 1) {
      current_floor++;
    } else if (direction < 0 && current_floor > 0) {
      current_floor--;
    }

    check_pthread(pthread_mutex_unlock(&g_core->lock),
                  "pthread_mutex_unlock core");
    usleep(20000);
  }
}

static void word_carrier_main(int slot, int floor) {
  unsigned seed = make_seed(slot + floor * 37);

  signal(SIGINT, SIG_IGN);
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("[PID:%d] Word-carrier-process_%d initialized on floor %d\n", getpid(),
         slot, floor);

  for (;;) {
    int admitted_idx = -1;
    unsigned long seen_epoch;

    check_pthread(pthread_mutex_lock(&g_core->lock), "pthread_mutex_lock core");
    seen_epoch = g_core->word_epoch;

    while (!g_core->shutdown_requested &&
           g_core->completed_words < g_core->total_words) {
      admitted_idx = try_admit_word_locked(floor);
      if (admitted_idx >= 0) {
        break;
      }

      while (!g_core->shutdown_requested &&
             g_core->completed_words < g_core->total_words &&
             g_core->word_epoch == seen_epoch) {
        check_pthread(pthread_cond_wait(&g_core->word_cv, &g_core->lock),
                      "pthread_cond_wait word_cv");
      }
      seen_epoch = g_core->word_epoch;
    }

    if (g_core->shutdown_requested ||
        g_core->completed_words == g_core->total_words) {
      check_pthread(pthread_mutex_unlock(&g_core->lock),
                    "pthread_mutex_unlock core");
      _exit(EXIT_SUCCESS);
    }

    check_pthread(pthread_mutex_unlock(&g_core->lock),
                  "pthread_mutex_unlock core");

    g_word_stats[slot].admissions++;
    printf("[PID:%d] Word-carrier-process_%d claimed word %d\n", getpid(), slot,
           g_words[admitted_idx].word_id);
    printf("[PID:%d] Word %d admitted to floor %d (sorting floor: %d)\n",
           getpid(), g_words[admitted_idx].word_id, floor,
           g_words[admitted_idx].sorting_floor);

    (void)seed;
  }
}

static void letter_carrier_main(int slot, int start_floor) {
  int current_floor = start_floor;
  unsigned seed = make_seed(slot + start_floor * 53);

  signal(SIGINT, SIG_IGN);
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("[PID:%d] Letter-carrier-process_%d initialized on floor %d\n",
         getpid(), slot, start_floor);

  for (;;) {
    int word_idx = -1;
    int char_idx = -1;
    int origin = -1;
    int word_id = -1;
    int dest_floor = -1;
    char ch = '\0';

    if (claim_random_character_from_floor(current_floor, &seed, &word_idx,
                                          &char_idx, &ch, &origin, &word_id,
                                          &dest_floor)) {
      printf("[PID:%d] Letter-carrier-process_%d selected char '%c' of word %d "
             "from floor %d\n",
             getpid(), slot, ch, word_id, current_floor);

      if (dest_floor == current_floor) {
        printf("[PID:%d] Letter-carrier-process_%d requested delivery elevator "
               "from floor %d to floor %d\n",
               getpid(), slot, current_floor, dest_floor);
        printf("[PID:%d] Destination is same floor -> direct placement\n",
               getpid());

        if (place_character_on_sorting_floor(word_idx, char_idx, slot,
                                             current_floor) >= 0) {
          g_letter_stats[slot].delivered++;
          g_letter_stats[slot].direct_deliveries++;
          printf("[PID:%d] Letter-carrier-process_%d brought char '%c' of word "
                 "%d to floor %d\n",
                 getpid(), slot, ch, word_id, current_floor);
        }
        continue;
      }

      printf("[PID:%d] Letter-carrier-process_%d requested delivery elevator "
             "from floor %d to floor %d\n",
             getpid(), slot, current_floor, dest_floor);

      if (!request_delivery_trip(slot, current_floor, dest_floor, word_idx,
                                 char_idx)) {
        _exit(EXIT_SUCCESS);
      }

      update_carrier_floor(current_floor, dest_floor);
      current_floor = dest_floor;

      if (place_character_on_sorting_floor(word_idx, char_idx, slot,
                                           current_floor) >= 0) {
        g_letter_stats[slot].delivered++;
        g_letter_stats[slot].elevator_deliveries++;
        printf("[PID:%d] Letter-carrier-process_%d brought char '%c' of word "
               "%d to floor %d\n",
               getpid(), slot, ch, word_id, current_floor);
      }
      continue;
    }

    printf("[PID:%d] Letter-carrier-process_%d found no available task on "
           "floor %d\n",
           getpid(), slot, current_floor);

    check_pthread(pthread_mutex_lock(&g_core->lock), "pthread_mutex_lock core");
    while (!g_core->shutdown_requested &&
           g_core->completed_words < g_core->total_words &&
           g_core->total_unclaimed_chars == 0) {
      unsigned long seen_epoch = g_core->char_epoch;
      while (!g_core->shutdown_requested &&
             g_core->completed_words < g_core->total_words &&
             g_core->char_epoch == seen_epoch) {
        check_pthread(pthread_cond_wait(&g_core->char_cv, &g_core->lock),
                      "pthread_cond_wait char_cv");
      }
    }

    if (g_core->shutdown_requested ||
        g_core->completed_words == g_core->total_words) {
      check_pthread(pthread_mutex_unlock(&g_core->lock),
                    "pthread_mutex_unlock core");
      _exit(EXIT_SUCCESS);
    }

    check_pthread(pthread_mutex_unlock(&g_core->lock),
                  "pthread_mutex_unlock core");

    if (g_core->cfg.num_floors <= 1) {
      continue;
    }

    dest_floor = random_floor_except(current_floor, &seed);
    printf("[PID:%d] Letter-carrier-process_%d requested reposition elevator "
           "from floor %d\n",
           getpid(), slot, current_floor);

    if (!request_reposition_trip(slot, current_floor, dest_floor)) {
      _exit(EXIT_SUCCESS);
    }

    update_carrier_floor(current_floor, dest_floor);
    current_floor = dest_floor;
    g_letter_stats[slot].repositions++;
    printf("[PID:%d] Letter-carrier-process_%d resumed work on floor %d\n",
           getpid(), slot, current_floor);
  }
}

static void sorting_process_main(int slot, int floor) {
  unsigned seed = make_seed(slot + floor * 79);

  signal(SIGINT, SIG_IGN);
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("[PID:%d] Sorting-process_%d initialized on floor %d\n", getpid(),
         slot, floor);

  for (;;) {
    unsigned long seen_epoch;

    if (try_sort_word(slot, floor, &seed)) {
      continue;
    }

    check_pthread(pthread_mutex_lock(&g_core->lock), "pthread_mutex_lock core");
    seen_epoch = g_floors[floor].sort_epoch;

    while (!g_core->shutdown_requested &&
           g_core->completed_words < g_core->total_words &&
           g_floors[floor].sort_epoch == seen_epoch) {
      check_pthread(
          pthread_cond_wait(&g_floors[floor].sorter_cv, &g_core->lock),
          "pthread_cond_wait sorter_cv");
    }

    if (g_core->shutdown_requested ||
        g_core->completed_words == g_core->total_words) {
      check_pthread(pthread_mutex_unlock(&g_core->lock),
                    "pthread_mutex_unlock core");
      _exit(EXIT_SUCCESS);
    }

    check_pthread(pthread_mutex_unlock(&g_core->lock),
                  "pthread_mutex_unlock core");
  }
}

static int word_id_compare(const void *lhs, const void *rhs) {
  int left_idx = *(const int *)lhs;
  int right_idx = *(const int *)rhs;
  const SharedWord *left = &g_words[left_idx];
  const SharedWord *right = &g_words[right_idx];

  if (left->word_id != right->word_id) {
    return (left->word_id < right->word_id) ? -1 : 1;
  }
  return 0;
}

static void print_completed_words_sorted_by_id(void) {
  int *order;
  int i;

  order = malloc((size_t)g_core->total_words * sizeof(*order));
  if (order == NULL) {
    die_perror("malloc");
  }

  for (i = 0; i < g_core->total_words; i++) {
    order[i] = i;
  }

  qsort(order, (size_t)g_core->total_words, sizeof(*order), word_id_compare);

  for (i = 0; i < g_core->total_words; i++) {
    SharedWord *word = &g_words[order[i]];
    printf("[PID:%d] Word %d COMPLETED\n", word->completed_by_pid,
           word->word_id);
  }

  printf("\n--------------------------------------------------\n\n");

  free(order);
}

static int output_compare(const void *lhs, const void *rhs) {
  int left_idx = *(const int *)lhs;
  int right_idx = *(const int *)rhs;
  const SharedWord *left = &g_sort_words[left_idx];
  const SharedWord *right = &g_sort_words[right_idx];

  if (left->sorting_floor != right->sorting_floor) {
    return (left->sorting_floor < right->sorting_floor) ? -1 : 1;
  }
  if (left->word_id != right->word_id) {
    return (left->word_id < right->word_id) ? -1 : 1;
  }
  return 0;
}

static void generate_output_file(void) {
  FILE *fp;
  int *order;
  int i;

  order = malloc((size_t)g_core->total_words * sizeof(*order));
  if (order == NULL) {
    die_perror("malloc");
  }

  for (i = 0; i < g_core->total_words; i++) {
    order[i] = i;
  }

  g_sort_words = g_words;
  qsort(order, (size_t)g_core->total_words, sizeof(*order), output_compare);

  fp = fopen(g_core->cfg.output_file, "w");
  if (fp == NULL) {
    die_perror("fopen output file");
  }

  for (i = 0; i < g_core->total_words; i++) {
    SharedWord *word = &g_words[order[i]];
    if (fprintf(fp, "%d %s %d\n", word->word_id, word->text,
                word->sorting_floor) < 0) {
      die_perror("fprintf output file");
    }
  }

  if (fclose(fp) != 0) {
    die_perror("fclose output file");
  }

  free(order);
}

static void broadcast_shutdown_locked(void) {
  int total_letter_carriers =
      g_core->cfg.num_floors * g_core->cfg.letter_carriers_per_floor;
  int i;

  check_pthread(pthread_cond_broadcast(&g_core->word_cv),
                "pthread_cond_broadcast word_cv");
  check_pthread(pthread_cond_broadcast(&g_core->char_cv),
                "pthread_cond_broadcast char_cv");
  check_pthread(pthread_cond_broadcast(&g_core->completion_cv),
                "pthread_cond_broadcast completion_cv");
  check_pthread(pthread_cond_broadcast(&g_core->delivery_cv),
                "pthread_cond_broadcast delivery_cv");
  check_pthread(pthread_cond_broadcast(&g_core->reposition_cv),
                "pthread_cond_broadcast reposition_cv");

  for (i = 0; i < g_core->cfg.num_floors; i++) {
    check_pthread(pthread_cond_broadcast(&g_floors[i].sorter_cv),
                  "pthread_cond_broadcast sorter_cv");
  }

  for (i = 0; i < total_letter_carriers; i++) {
    check_pthread(pthread_cond_broadcast(&g_delivery_requests[i].cv),
                  "pthread_cond_broadcast delivery request");
    check_pthread(pthread_cond_broadcast(&g_reposition_requests[i].cv),
                  "pthread_cond_broadcast reposition request");
  }
}

static void print_summary(void) {
  printf("System Summary:\n");
  printf("Total words: %d\n", g_core->total_words);
  printf("Completed words: %d\n", g_core->completed_words);
  printf("Retries: %d\n", g_core->retry_count);
  printf("Characters transported: %d\n", g_core->transported_chars);
  printf("Delivery elevator operations: %d\n", g_core->delivery_operations);
  printf("Reposition elevator operations: %d\n", g_core->reposition_operations);
}

static struct timespec make_timeout_ms(long milliseconds) {
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    die_perror("clock_gettime");
  }
  ts.tv_nsec += (milliseconds % 1000) * 1000000L;
  ts.tv_sec += milliseconds / 1000 + ts.tv_nsec / 1000000000L;
  ts.tv_nsec %= 1000000000L;
  return ts;
}

static pid_t spawn_child(void (*child_fn)(int, int), int slot, int floor) {
  pid_t pid = fork();

  if (pid < 0) {
    die_perror("fork");
  }

  if (pid == 0) {
    child_fn(slot, floor);
    _exit(EXIT_SUCCESS);
  }

  return pid;
}

static pid_t spawn_simple_child(void (*child_fn)(void)) {
  pid_t pid = fork();

  if (pid < 0) {
    die_perror("fork");
  }

  if (pid == 0) {
    child_fn();
    _exit(EXIT_SUCCESS);
  }

  return pid;
}

int main(int argc, char **argv) {
  Config cfg;
  InputWord *input_words;
  int total_words = 0;
  int total_word_carriers;
  int total_letter_carriers;
  int total_sorters;
  ChildInfo *children;
  int child_count = 0;
  int max_children;
  int floor;
  struct sigaction sa;

  setvbuf(stdout, NULL, _IONBF, 0);

  cfg = parse_args(argc, argv);
  input_words = read_input_file(&cfg, &total_words);
  initialize_shared_state(&cfg, input_words, total_words);

  total_word_carriers = cfg.num_floors * cfg.word_carriers_per_floor;
  total_letter_carriers = cfg.num_floors * cfg.letter_carriers_per_floor;
  total_sorters = cfg.num_floors * cfg.sorting_processes_per_floor;
  max_children =
      total_word_carriers + total_letter_carriers + total_sorters + 2;

  children = calloc((size_t)max_children, sizeof(*children));
  if (children == NULL) {
    die_perror("calloc");
  }

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = parent_signal_handler;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGINT, &sa, NULL) != 0) {
    die_perror("sigaction");
  }

  printf("Program is starting...\n");
  printf("Input file is being read...\n");
  printf("Shared memory is initialized...\n");
  printf("Synchronization primitives are created...\n");
  printf("Processes are being created...\n\n");
  printf("[PID:%d] Parent process started\n", getpid());

  int w_slot = 0;
  int l_slot = 0;
  int s_slot = 0;

  for (floor = 0; floor < cfg.num_floors; floor++) {
    int i;

    printf("--- Initializing Floor %d --\n", floor);

    for (i = 0; i < cfg.word_carriers_per_floor; i++) {
      children[child_count].pid = spawn_child(word_carrier_main, w_slot, floor);
      children[child_count].slot = w_slot;
      children[child_count].floor = floor;
      child_count++;
      w_slot++;
      usleep(5000);
    }

    for (i = 0; i < cfg.letter_carriers_per_floor; i++) {
      children[child_count].pid = spawn_child(letter_carrier_main, l_slot, floor);
      children[child_count].slot = l_slot;
      children[child_count].floor = floor;
      child_count++;
      l_slot++;
      usleep(5000);
    }

    for (i = 0; i < cfg.sorting_processes_per_floor; i++) {
      children[child_count].pid =
          spawn_child(sorting_process_main, s_slot, floor);
      children[child_count].slot = s_slot;
      children[child_count].floor = floor;
      child_count++;
      s_slot++;
      usleep(5000);
    }
  }

  children[child_count++].pid = spawn_simple_child(delivery_elevator_main);
  usleep(5000);
  children[child_count++].pid = spawn_simple_child(reposition_elevator_main);
  usleep(5000);

  for (;;) {
    int done = 0;
    struct timespec timeout;
    int wait_rc;

    check_pthread(pthread_mutex_lock(&g_core->lock), "pthread_mutex_lock core");
    done = (g_core->completed_words == g_core->total_words);
    if (!done && !g_parent_interrupted) {
      timeout = make_timeout_ms(250);
      wait_rc = pthread_cond_timedwait(&g_core->completion_cv, &g_core->lock,
                                       &timeout);
      if (wait_rc != 0 && wait_rc != ETIMEDOUT) {
        check_pthread(wait_rc, "pthread_cond_timedwait");
      }
      done = (g_core->completed_words == g_core->total_words);
    }
    check_pthread(pthread_mutex_unlock(&g_core->lock),
                  "pthread_mutex_unlock core");

    if (done || g_parent_interrupted) {
      break;
    }
  }

  check_pthread(pthread_mutex_lock(&g_core->lock), "pthread_mutex_lock core");
  g_core->shutdown_requested = 1;
  broadcast_shutdown_locked();
  check_pthread(pthread_mutex_unlock(&g_core->lock),
                "pthread_mutex_unlock core");

  for (floor = 0; floor < child_count; floor++) {
    int status;
    if (waitpid(children[floor].pid, &status, 0) < 0) {
      die_perror("waitpid");
    }
  }

  if (!g_parent_interrupted && g_core->completed_words == g_core->total_words) {
    print_completed_words_sorted_by_id();
    printf("All words have been transported and sorted...\n");
    printf("Output file is being created...\n");
    generate_output_file();
    print_summary();
    printf("Program terminated successfully.\n");
  } else {
    fprintf(stderr, "Program interrupted by SIGINT. Cleaning up...\n");
  }

  free(children);
  free(input_words);

  return g_parent_interrupted ? EXIT_FAILURE : EXIT_SUCCESS;
}
