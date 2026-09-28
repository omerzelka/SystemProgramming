# HW3 — Multi-Process Word Transport System

A complex multi-process concurrent system that simulates a building with multiple floors, where words are transported, their letters delivered by carriers via elevators, and then sorted into correct order — using POSIX shared memory, process-shared mutexes, and condition variables.

## Architecture

```
Main Process (Orchestrator)
  │
  ├── Word Carrier Processes (per floor)
  │     └── Claim unclaimed words, admit to floor system
  │
  ├── Letter Carrier Processes (per floor)
  │     ├── Claim characters from arrival floor
  │     ├── Request elevator delivery (if cross-floor)
  │     └── Place character in sorting area
  │
  ├── Sorting Processes (per floor)
  │     └── Bubble-sort characters into correct positions
  │
  ├── Delivery Elevator Process
  │     └── Batch transport carriers between floors
  │
  ├── Reposition Elevator Process
  │     └── Relocate idle carriers to floors with work
  │
  └── Output Generator Process
        └── Write completed words to output file
```

## Key Concepts

| Concept | Implementation |
|---|---|
| **Concurrency** | `fork()` — word carriers, letter carriers, sorters, elevators |
| **Shared Memory** | `mmap(MAP_SHARED \| MAP_ANONYMOUS)` for all shared state |
| **Synchronization** | Process-shared `pthread_mutex_t` and `pthread_cond_t` |
| **Coordination** | Epoch-based signaling with condition variable broadcasts |
| **Producer-Consumer** | Elevator request queues with pending/onboard/done states |
| **Signal Handling** | `SIGINT` for graceful multi-process shutdown |
| **Load Balancing** | Round-robin word admission, random character claiming |

## Files

| File | Description |
|---|---|
| `hw3.c` | All logic: shared memory setup, process spawning, carriers, sorters, elevators, output |
| `Makefile` | Build configuration (`-Wall -Wextra -pedantic -std=c11 -pthread`) |
| `input.txt` | Sample input: 12 words with word IDs, text, and sorting floor assignments |
| `input1.txt` | Alternative test input |
| `input2.txt` | Minimal test input |
| `output.txt` | Sample output with sorted words |
| `hw3.pdf` | Assignment specification |

## Build & Run

```bash
make
./hw3 -f <floors> -w <word_carriers> -l <letter_carriers> -s <sorters> -c <floor_capacity> -d <delivery_capacity> -r <reposition_capacity> -i <input_file> -o <output_file>
```

### Arguments

| Flag | Description |
|---|---|
| `-f` | Number of floors in the building |
| `-w` | Word carrier processes per floor |
| `-l` | Letter carrier processes per floor |
| `-s` | Sorting processes per floor |
| `-c` | Maximum words per floor (capacity) |
| `-d` | Delivery elevator capacity (passengers per trip) |
| `-r` | Reposition elevator capacity (passengers per trip) |
| `-i` | Path to input file |
| `-o` | Path to output file |

### Examples

```bash
./hw3 -f 5 -w 2 -l 3 -s 2 -c 4 -d 3 -r 2 -i input.txt -o output.txt
./hw3 -f 3 -w 1 -l 2 -s 1 -c 3 -d 2 -r 1 -i input1.txt -o result.txt
```

## Input Format

Each line: `<wordID> <word> <sortingFloor>`

```
101 apple 3
102 level 1
103 kernel 4
104 system 2
```

## System Calls Used

`fork()`, `waitpid()`, `mmap()`, `munmap()`, `pthread_mutex_lock/unlock()`, `pthread_cond_wait/broadcast/signal()`, `sigaction()`, `kill()`, `clock_gettime()`, `getopt()`, `fopen()`, `getline()`
