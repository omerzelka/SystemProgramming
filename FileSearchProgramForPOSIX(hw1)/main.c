#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BOLD_WHITE "\033[1;37m" // The text format provides bold print
#define RESET_FMT "\033[0m"     // to reset format

typedef struct {
  char *filename;
  int size;
  char fileType;
  char *permissions;
  int numLinks;
} SearchCriteria;

void searchDirectory(const char *dirPath, int depth, SearchCriteria crits);
void getPermissions(mode_t mode, char *str);
int recursiveRegex(const char *pattern, const char *str);
int isMatch(struct stat fileInfo, const char *currentFilename,
            SearchCriteria crits);
void handle_signal(int sig);

int main(int argc, char *argv[]) {
  int opt;
  char *targetDir = NULL;

  // Initialize struct with default values
  SearchCriteria crits;
  crits.filename = NULL;
  crits.size = -1;       // -1 means no size criteria provided
  crits.fileType = '\0'; // '\0' means no type criteria provided
  crits.permissions = NULL;
  crits.numLinks = -1; // -1 means no link criteria provided

  signal(SIGINT, handle_signal); // SIGINT = CTRL+C

  while ((opt = getopt(argc, argv, "w:f:b:t:p:l:")) != -1) {
    switch (opt) {
    case 'w':
      targetDir = optarg;
      break;
    case 'f':
      crits.filename = optarg;
      break;
    case 'b':
      crits.size = atoi(optarg);
      break;
    case 't':
      crits.fileType = optarg[0];
      break;
    case 'p':
      crits.permissions = optarg;
      break;
    case 'l':
      crits.numLinks = atoi(optarg);
      break;
    case '?':
      if (optopt == 'w' || optopt == 'f' || optopt == 'b' || optopt == 't' ||
          optopt == 'p' || optopt == 'l') {
        fprintf(stderr, "Error: -%c requires an argument.\n", optopt);
      } else {
        fprintf(stderr, "Error: Unknown option '-%c'.\n", optopt);
      }
      fprintf(
          stderr,
          "Usage: ./myFind -w <target_dir> [-f <filename>] [-b <size>] ...\n");
      exit(EXIT_FAILURE);
    }
  }

  if (targetDir == NULL) { //-w must be provided
    fprintf(stderr, "ERROR: Target directory (-w) must be specified.\n");
    exit(EXIT_FAILURE);
  }

  // At least one search criteria must be provided
  if (crits.filename == NULL && crits.size == -1 && crits.fileType == '\0' &&
      crits.permissions == NULL && crits.numLinks == -1) {
    fprintf(stderr, "ERROR: At least one search criteria (-f, -b, -t, -p, -l) "
                    "must be specified!\n");
    exit(EXIT_FAILURE);
  }

  // Start searching with the target directory and initial depth 0
  printf("\n--- SEARCH RESULTS ---\n");
  searchDirectory(targetDir, 0, crits);

  return 0;
}

// Custom, library-free, case-insensitive regex matching for '+' operator
int recursiveRegex(const char *pattern, const char *str) {
  if (*pattern == '\0') {
    return (*str == '\0');
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

// Converts from MODE BITS to string for comparing (e.g., "rwxr-xr--")
void getPermissions(mode_t mode, char *str) {
  str[0] = (mode & S_IRUSR) ? 'r' : '-'; // User Read
  str[1] = (mode & S_IWUSR) ? 'w' : '-'; // User Write
  str[2] = (mode & S_IXUSR) ? 'x' : '-'; // User Execute

  str[3] = (mode & S_IRGRP) ? 'r' : '-'; // Group Read
  str[4] = (mode & S_IWGRP) ? 'w' : '-'; // Group Write
  str[5] = (mode & S_IXGRP) ? 'x' : '-'; // Group Execute

  str[6] = (mode & S_IROTH) ? 'r' : '-'; // Others Read
  str[7] = (mode & S_IWOTH) ? 'w' : '-'; // Others Write
  str[8] = (mode & S_IXOTH) ? 'x' : '-'; // Others Execute

  str[9] = '\0'; // Null terminator
}

// Filter function to check if a file matches ALL given criteria
int isMatch(struct stat fileInfo, const char *currentFilename,
            SearchCriteria crits) {

  if (crits.size != -1) { //(-b)
    if (fileInfo.st_size != crits.size)
      return 0;
  }

  if (crits.numLinks != -1) { //(-l)
    if (fileInfo.st_nlink != crits.numLinks)
      return 0;
  }

  if (crits.fileType != '\0') { //(-t)
    int typeMatch = 0;
    switch (crits.fileType) {
    case 'd':
      if (S_ISDIR(fileInfo.st_mode))
        typeMatch = 1;
      break;
    case 's':
      if (S_ISSOCK(fileInfo.st_mode))
        typeMatch = 1;
      break;
    case 'b':
      if (S_ISBLK(fileInfo.st_mode))
        typeMatch = 1;
      break;
    case 'c':
      if (S_ISCHR(fileInfo.st_mode))
        typeMatch = 1;
      break;
    case 'f':
      if (S_ISREG(fileInfo.st_mode))
        typeMatch = 1;
      break;
    case 'p':
      if (S_ISFIFO(fileInfo.st_mode))
        typeMatch = 1;
      break;
    case 'l':
      if (S_ISLNK(fileInfo.st_mode))
        typeMatch = 1;
      break;
    }
    if (typeMatch == 0)
      return 0;
  }

  if (crits.permissions != NULL) { //(-p)
    char filePerms[10];
    getPermissions(fileInfo.st_mode, filePerms);
    if (strcmp(crits.permissions, filePerms) != 0)
      return 0;
  }

  if (crits.filename != NULL) { //(-f)
    if (!recursiveRegex(crits.filename, currentFilename))
      return 0;
  }

  return 1; // All criteria matched perfectly
}

// Recursive function to traverse directories and find matches
void searchDirectory(const char *dirPath, int depth, SearchCriteria crits) {
  DIR *dir;
  struct dirent *entry;
  struct stat fileInfo;

  // usleep(1000000);// to control ctrl+c

  // 1. Try to open the directory
  if ((dir = opendir(dirPath)) == NULL) {
    if (errno == ENOENT) {
      fprintf(stderr, "ERROR: Directory '%s' not found!\n", dirPath);
    } else if (errno == EACCES) {
      fprintf(stderr, "ERROR: Permission denied for directory '%s'!\n",
              dirPath);
    } else if (errno == ENOTDIR) {
      fprintf(stderr, "ERROR: '%s' is not a directory!\n", dirPath);
    } else {
      fprintf(stderr, "ERROR: Cannot open '%s'. Reason: %s\n", dirPath,
              strerror(errno));
    }
    return; // Skip this directory to prevent crashes
  }

  // 2. Read directory entries one by one
  while ((entry = readdir(dir)) != NULL) {
    // Prevent infinite loops by skipping "." and ".."
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    // Create full path to pass to lstat
    char fullPath[1024];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPath, entry->d_name);

    // 3. Get file metadata using lstat
    if (lstat(fullPath, &fileInfo) == -1) {
      fprintf(stderr, "ERROR: Cannot read info for '%s': %s\n", fullPath,
              strerror(errno));
      continue;
    }

    // Print tree indentation based on depth
    printf("|--");
    for (int i = 1; i < depth; i++) {
      printf("--");
    }

    // 4. FILTERING: Check if file passes our isMatch function
    if (isMatch(fileInfo, entry->d_name, crits)) {
      // To write bold for matched files
      printf("%s%s%s\n", BOLD_WHITE, fullPath, RESET_FMT);
    } else {
      // Regular print for non-matched files
      printf("%s\n", fullPath);
    }

    // 5. If the current entry is a directory, dive into it (Recursive call)
    if (S_ISDIR(fileInfo.st_mode)) {
      searchDirectory(fullPath, depth + 1, crits);
    }
  }

  closedir(dir);
}

void handle_signal(int sig) {

  write(STDERR_FILENO, "\n[!] SIGINT received. Exiting safely...\n",
        40); // from signal.h

  _exit(0); // The _exit closes the program at core level
}