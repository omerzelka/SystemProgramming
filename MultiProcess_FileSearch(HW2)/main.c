#include <ctype.h> // tolower() fonksiyonu için
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void print_usage_and_exit(const char *progName);
void searchDir(const char *current_path, const char *pattern,
               int min_size); // void yapıldı
int recursiveRegex(const char *pattern, const char *str);
int containsPattern(const char *pattern, const char *filename);
int isMatch(struct stat fileInfo, const char *currentFilename,
            const char *pattern, int min_size);
void signalHandler(int sig);
void workerSignalHandler(int sig);

volatile sig_atomic_t workersFinished = 0;  // SIGUSR1 flag
volatile sig_atomic_t sigintReceived = 0;   // Ctrl+C control flag
volatile sig_atomic_t workerMatchCount = 0; // Finded folder count by worker

int main(int argc, char *argv[]) {
  int opt;
  char *rootDir = NULL;
  char *pattern = NULL;
  int numWorkers = 0;
  int minSize = -1; // optional
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));     // Reset the struct
  sa.sa_handler = &signalHandler; // Point our signal handle function
  sigfillset(&sa.sa_mask); // when the signal in the process, this function
                           // blocks the other signal
  sa.sa_flags = 0;

  // SIGUSR1'i işletim sistemine kaydet
  if (sigaction(SIGUSR1, &sa, NULL) == -1) {
    perror("Error registering SIGUSR1");
    exit(EXIT_FAILURE);
  }

  // SIGINT'i (Ctrl+C) işletim sistemine kaydet
  if (sigaction(SIGINT, &sa, NULL) == -1) {
    perror("Error registering SIGINT");
    exit(EXIT_FAILURE);
  }

  // getopt reads argumants.
  while ((opt = getopt(argc, argv, "d:n:f:s:")) != -1) {
    switch (opt) {
    case 'd':
      rootDir = optarg;
      break;
    case 'n':
      numWorkers = atoi(optarg);
      break;
    case 'f':
      pattern = optarg;
      break;
    case 's':
      minSize = atoi(optarg);
      break;
    default:
      print_usage_and_exit(argv[0]);
    }
  }

  if (rootDir == NULL || pattern == NULL || numWorkers == 0) {
    fprintf(stderr, "Error: Missing required arguments.\n");
    print_usage_and_exit(argv[0]);
  }

  if (numWorkers < 2 || numWorkers > 8) {
    fprintf(stderr,
            "Error: <numWorkers> must be between 2 and 8 (inclusive).\n");
    print_usage_and_exit(argv[0]);
  }

  // These are from dirent.h thats necessary for reading process
  DIR *dir; // Folder pointer. Also opendir return DIR
  struct dirent
      *entry; // Items pointer thats from directory. Also readdir return dirent
  struct stat statbuf; // stat takes all metadata infos of dirent.

  // These provide to hold sub directories dynamically.
  int capacity = 10;
  int dirCount = 0;
  char **subDirs = malloc(capacity * sizeof(char *));

  if ((dir = opendir(rootDir)) == NULL) {
    perror("Error opening root directory");
    exit(EXIT_FAILURE);
  }

  // Reading Process
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", rootDir, entry->d_name);

    // The block controls whether directory or not.
    if (stat(path, &statbuf) == 0 /* 0's mean is sucsess */ &&
        S_ISDIR(statbuf.st_mode)) {
      if (dirCount >= capacity) { // Capacity Control
        capacity *= 2;
        subDirs = realloc(subDirs, capacity * sizeof(char *));
      }
      subDirs[dirCount] = strdup(path); // strdup technically moves data from
                                        // stack to heap with some operation.
      dirCount++;
    }
  }
  closedir(dir);

  // --- Worker-Parent Control ---
  if (dirCount ==
      0) { // If there are no sub directories, just the parent will work.
    printf(
        "Notice: no subdirectories found; parent will search root directly.\n");
    numWorkers = 0; // No workers.
    searchDir(rootDir, pattern, minSize);
  } else if (dirCount < numWorkers) {
    // If the number of workers are greater than sub directories count, the
    // workers count will be reduced by program.
    printf("Notice: only %d subdirectories found; using %d workers instead of "
           "%d.\n",
           dirCount, dirCount, numWorkers);
    numWorkers = dirCount;
  }

  // --- ROUND-ROBIN DAĞITIM HAZIRLIĞI ---
  if (numWorkers > 0) {
    printf("\n--- Directory Partitioning (Round-Robin) ---\n");
    for (int i = 0; i < dirCount; i++) {
      int workerID = i % numWorkers; // Round-robin formülü
      printf("Worker %d will search: %s\n", workerID, subDirs[i]);
    }
    printf("--------------------------------------------\n");
  }

  // İşçi süreçlerin PID (Process ID) değerlerini tutacağımız dizi
  pid_t *worker_pids = malloc(numWorkers * sizeof(pid_t));

  printf("\n--- Starting %d Workers ---\n", numWorkers);

  for (int i = 0; i < numWorkers; i++) {
    pid_t pid =
        fork(); // The system creates 2 pid value, the orginal value of pid
                // returns the value that is greater than zero, and the other
                // one that is the worker's itself that returns 0.

    if (pid < 0) { // If there is no process, fork() will return -1
      perror("Fork failed");
      exit(EXIT_FAILURE);
    } else if (pid == 0) { // The worker process block

      // Worker's SIGTERM catcher
      struct sigaction wsa;
      memset(&wsa, 0, sizeof(wsa));
      wsa.sa_handler = &workerSignalHandler;
      sigemptyset(&wsa.sa_mask);
      wsa.sa_flags = 0;
      sigaction(SIGTERM, &wsa, NULL);

      int workerID = i;

      for (int j = 0; j < dirCount; j++) {
        if (j % numWorkers == workerID) { // Provides respectivity
          searchDir(subDirs[j], pattern, minSize);
        }
      }
      kill(getppid(), SIGUSR1); // Sends signal to parent
      exit(workerMatchCount %
           256); // Mod 256 of match count for operation system limits.
    } else {     // If pid greater than 0, it's mean is parent process step.
      worker_pids[i] = pid; // Saves workers id to processID list to control.
    }
  }

  // --- PARENTS'S WAITING AND CONTROL MECHANİSM ---
  // İşçi süreçlerin sonuçlarını saklayacağımız dizi (Sinyal kaybı önlemi için)
  int *worker_matches = NULL;
  if (numWorkers > 0) {
    worker_matches = malloc(numWorkers * sizeof(int));
    for (int i = 0; i < numWorkers; i++) {
      worker_matches[i] = -1; // -1: Henüz bitmedi
    }
  }

  // pause() yarış durumu ve SIGUSR1 sinyal kaybını (lost signals) önlemek için
  // waitpid ile güvenli bekleme (polling) döngüsü.
  int reapedCount = 0;
  while (reapedCount < numWorkers && !sigintReceived) {
    for (int i = 0; i < numWorkers; i++) {
      if (worker_matches[i] == -1) {
        int status;
        // WNOHANG ile non-blocking olarak worker'ın bitip bitmediğini kontrol
        // et
        if (waitpid(worker_pids[i], &status, WNOHANG) > 0) {
          if (WIFEXITED(status)) {
            worker_matches[i] = WEXITSTATUS(status);
          } else {
            worker_matches[i] = 0;
          }
          reapedCount++;
        }
      }
    }

    if (reapedCount < numWorkers && !sigintReceived) {
      usleep(10000); // 10 ms bekle (CPU'yu gereksiz yormamak için)
    }
  }

  if (sigintReceived) { // CTRL+C case
    printf("\n[Parent] SIGINT received. Terminating all workers...\n");

    for (int i = 0; i < numWorkers; i++) {
      if (worker_matches[i] == -1) {
        kill(worker_pids[i],
             SIGTERM); // Sadece henüz bitmemiş processleri sonlandır
      }
    }

    // sleep(3); // The sleep function gains times to send signal parent by
    // workers.

    // If there is any worker that lives now, SIGKILL will close them
    int totalMatches = 0;
    printf("\n--- Partial Summary ---\n");
    for (int i = 0; i < numWorkers; i++) {
      if (worker_matches[i] == -1) {
        int status;
        // WNOHANG: Controls workers status and if worker was not closed(return
        // 0), kill() function kills worker suddenly.
        if (waitpid(worker_pids[i], &status, WNOHANG) == 0) {
          kill(worker_pids[i], SIGKILL); // kills worker
          waitpid(worker_pids[i], &status,
                  0); // for being sure, worker's status equals to zero.
        }

        // If the worker was not killed by kill function, the compiler enters
        // the block.
        if (WIFEXITED(status)) {
          worker_matches[i] = WEXITSTATUS(status); // Seperates the exit code.
        } else {
          worker_matches[i] = 0;
        }
      }
      totalMatches += worker_matches[i];
      printf("Worker PID %d : %d matches\n", worker_pids[i], worker_matches[i]);
    }
    printf("Total matches found so far: %d\n", totalMatches);
  } else { // SIGUSR1 case (Successful)
    int totalMatches = 0;
    printf("\n--- Summary ---\n");
    printf("Total workers used : %d\n", numWorkers);

    for (int i = 0; i < numWorkers; i++) {
      int matches = worker_matches[i];
      if (matches == -1)
        matches = 0; // Güvenlik amaçlı
      totalMatches += matches;
      printf("Worker PID %d : %d matches\n", worker_pids[i], matches);
    }
    if (numWorkers == 0) { // When there is no worker
      totalMatches = workerMatchCount;
    }
    printf("Total matches found: %d\n", totalMatches);
  }

  // Clean the heap
  for (int i = 0; i < dirCount; i++)
    free(subDirs[i]);
  free(subDirs);
  free(worker_pids);
  if (worker_matches != NULL)
    free(worker_matches);

  return 0;
}

void print_usage_and_exit(const char *progName) {
  fprintf(stderr,
          "Usage: %s -d <rootDir> -n <numWorkers> -f <pattern> [-s "
          "<min_size_bytes>]\n",
          progName);
  exit(EXIT_FAILURE);
}

// Recursive directory search function
void searchDir(const char *current_path, const char *pattern, int min_size) {
  DIR *dir;
  struct dirent *entry;
  struct stat statbuf;

  if ((dir = opendir(current_path)) == NULL) {
    return;
  }

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", current_path, entry->d_name);

    if (stat(path, &statbuf) == -1)
      continue;

    // Directory block
    if (S_ISDIR(statbuf.st_mode)) {
      searchDir(path, pattern, min_size); // localMatchCount kalıntısı silindi
    }
    // File block
    else if (isMatch(statbuf, entry->d_name, pattern, min_size)) {
      printf("[Worker PID:%d] MATCH: %s (%ld bytes)\n", getpid(), path,
             (long)statbuf.st_size);
      workerMatchCount++;
    }
  }
  closedir(dir);
}

// My recursive Regex function
int recursiveRegex(const char *pattern, const char *str) {
  if (*pattern == '\0') {
    return 1;
  }

  // Check if current characters match (case-insensitive)
  int isFirstMatch = (*str != '\0' && tolower((unsigned char)*str) ==
                                          tolower((unsigned char)*pattern));

  if (*(pattern + 1) == '+') {
    if (isFirstMatch) {
      // left side: greedy (keep '+' rule, move to next char in str)
      // right side: skip '+' rule (move pattern + 2), move to next char in str
      return recursiveRegex(pattern, str + 1) ||
             recursiveRegex(pattern + 2, str + 1);
    } else {
      return 0;
    }
  } else {
    return isFirstMatch && recursiveRegex(pattern + 1, str + 1);
  }
}

int containsPattern(const char *pattern, const char *filename) {
  if (pattern == NULL || pattern[0] == '\0')
    return 1;

  // Try matching with wrapper loop (ex: report -> eport -> port ...)
  for (const char *t = filename; *t != '\0'; t++) {
    if (recursiveRegex(pattern, t)) {
      return 1;
    }
  }
  return 0;
}

int isMatch(struct stat fileInfo, const char *currentFilename,
            const char *pattern, int min_size) {

  // Just files must control by the function
  if (!S_ISREG(fileInfo.st_mode)) {
    return 0;
  }

  if (min_size != -1) { // Size control
    if (fileInfo.st_size < min_size) {
      return 0;
    }
  }

  if (pattern != NULL) {
    if (!containsPattern(pattern, currentFilename)) {
      return 0;
    }
  }
  return 1;
}

void signalHandler(int sig) {
  if (sig == SIGUSR1) { // For worker's finish successfully
    workersFinished++;
  } else if (sig == SIGINT) { // For CTRL+C
    sigintReceived = 1;
  }
}

void workerSignalHandler(int sig) {
  if (sig == SIGTERM) { // For worker's finish suddenly
    _exit(workerMatchCount % 256);
  }
}