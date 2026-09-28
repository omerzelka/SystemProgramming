# HW2 — Multi-Process File Search (procSearch)

A multi-process parallel file search system that distributes directory scanning across worker processes using `fork()`, round-robin partitioning, and inter-process signaling for coordination and graceful shutdown.

## Architecture

```
Parent Process (Orchestrator)
  │
  ├── Scan root directory for subdirectories
  ├── Round-robin partition subdirs across workers
  ├── Fork N worker processes
  │     ├── Worker-0 → searches assigned subdirs recursively
  │     ├── Worker-1 → ...
  │     └── Worker-N → ...
  ├── waitpid() polling loop (non-blocking with WNOHANG)
  ├── Collect match counts via exit codes
  └── Print summary report
```

## Key Concepts

| Concept | Implementation |
|---|---|
| **Concurrency** | `fork()` — multiple worker processes |
| **IPC** | Exit codes (`WEXITSTATUS`) for match count transfer |
| **Signaling** | `SIGUSR1` (worker done), `SIGINT` (Ctrl+C), `SIGTERM`/`SIGKILL` (worker termination) |
| **Load Balancing** | Round-robin directory assignment (`j % numWorkers`) |
| **Pattern Matching** | Custom recursive regex with `+` operator (case-insensitive substring match) |
| **Graceful Shutdown** | `sigaction()` — parent catches `SIGINT`, sends `SIGTERM` to workers |

## Files

| File | Description |
|---|---|
| `main.c` | All logic: argument parsing, forking, directory search, signal handling, summary |
| `Makefile` | Build configuration (`-Wall -g`) |
| `test_root/` | Test directory with sample file hierarchy |
| `test_empty_root/` | Empty directory for edge-case testing |
| `hw2.pdf` | Assignment specification |

## Build & Run

```bash
make
./procSearch -d <rootDir> -n <numWorkers> -f <pattern> [-s <min_size_bytes>]
```

### Arguments

| Flag | Description |
|---|---|
| `-d` | Root directory to search |
| `-n` | Number of worker processes (2–8) |
| `-f` | Filename pattern to match (case-insensitive substring with `+` regex) |
| `-s` | Minimum file size in bytes (optional) |

### Examples

```bash
./procSearch -d ./test_root -n 4 -f "report"
./procSearch -d /var/log -n 3 -f "error" -s 1024
./procSearch -d ./test_root -n 2 -f "log"
```

## Output

### Console
```
--- Directory Partitioning (Round-Robin) ---
Worker 0 will search: ./test_root/subdir1
Worker 1 will search: ./test_root/subdir2
Worker 0 will search: ./test_root/subdir3
--------------------------------------------

--- Starting 2 Workers ---
[Worker PID:12345] MATCH: ./test_root/subdir1/report.txt (2048 bytes)
[Worker PID:12346] MATCH: ./test_root/subdir2/error_log.txt (512 bytes)

--- Summary ---
Total workers used : 2
Worker PID 12345 : 3 matches
Worker PID 12346 : 1 matches
Total matches found: 4
```

## System Calls Used

`fork()`, `waitpid()`, `kill()`, `sigaction()`, `getopt()`, `opendir()`, `readdir()`, `stat()`, `getpid()`, `getppid()`, `_exit()`, `usleep()`
