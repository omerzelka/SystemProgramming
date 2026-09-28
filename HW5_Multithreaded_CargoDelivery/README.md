# HW5 — Multi-Threaded Cargo Delivery Simulator (cargoGTU)

A multi-threaded courier delivery simulation using the **producer-consumer pattern** with a priority queue. Orders are dispatched to courier threads based on delivery priority, with full statistics tracking and graceful signal handling.

## Architecture

```
Main Thread (Dispatcher)
  │
  ├─ Reads orders from file
  ├─ Pushes into priority min-heap queue
  └─ Signals courier threads via condition variable
          │
          ├── Courier-1 ──→ pops highest-priority order, simulates delivery
          ├── Courier-2 ──→ ...
          └── Courier-N ──→ ...
```

### Priority System
| Priority | Level | Delivery Order |
|---|---|---|
| `EXPRESS` | 1 (highest) | First |
| `STANDARD` | 2 | Second |
| `ECONOMY` | 3 (lowest) | Last |

Ties are broken by order ID (FIFO).

## Key Concepts

| Concept | Implementation |
|---|---|
| **Concurrency** | POSIX threads (`pthread_create`, `pthread_join`) |
| **Sync — Mutual Exclusion** | `pthread_mutex_t` for queue and log protection |
| **Sync — Signaling** | `pthread_cond_t` (`wait`, `broadcast`) |
| **Atomic Counters** | `<stdatomic.h>` — `atomic_fetch_add`, `atomic_load` |
| **Signal Handling** | `sigaction` for `SIGINT` — cancels pending, finishes active |
| **Data Structure** | Min-heap priority queue (array-backed) |

## Files

| File | Description |
|---|---|
| `main.c` | All logic: argument parsing, priority queue, thread pool, delivery simulation |
| `Makefile` | Build configuration (`-Wall -Wextra -std=c11 -pthread`) |
| `10orders_economy.txt` | Test input: 10 economy-only orders |
| `10orders_mix.txt` | Test input: 10 mixed-priority orders |
| `20orders_mix.txt` | Test input: 20 mixed-priority orders |

## Build & Run

```bash
make
./cargoGTU -n <numCouriers> -i <orderFile> -s <statsFile>
```

### Arguments

| Flag | Description |
|---|---|
| `-n` | Number of courier worker threads |
| `-i` | Path to input order file |
| `-s` | Path to output statistics file |

### Examples

```bash
./cargoGTU -n 3 -i 10orders_mix.txt -s stats.txt
./cargoGTU -n 5 -i 20orders_mix.txt -s stats_20.txt
./cargoGTU -n 2 -i 10orders_economy.txt -s stats_eco.txt
```

## Input Format

Each line: `<orderID> <recipientName> <priority> <durationUnits>`

```
1 Ahmet_Yilmaz EXPRESS 8
2 Mehmet_Demir STANDARD 5
3 Ayse_Kaya ECONOMY 3
```

## Output

### Console Log
```
[CARGOGTU] SHIFT_START couriers=3 orders=10
[CARGOGTU] ORDER_QUEUED id=1 recipient=Ahmet_Yilmaz priority=EXPRESS duration=8
[COURIER-1] DELIVERY_START id=1 recipient=Ahmet_Yilmaz priority=EXPRESS
[COURIER-1] DELIVERY_COMPLETE id=1 recipient=Ahmet_Yilmaz duration=4000ms
[CARGOGTU] SHIFT_END completed=10 cancelled=0 total_time=48500ms
```

### Stats File
```
SHIFT_SUMMARY
Total orders : 10
Completed    : 10
Cancelled    : 0
Total time   : 48500ms
Avg per order: 4850ms

COURIER_STATS
Courier-1 completed=4 total_time=18500ms
Courier-2 completed=3 total_time=15000ms
```

## System Calls Used

`pthread_create()`, `pthread_join()`, `pthread_mutex_lock/unlock()`, `pthread_cond_wait/broadcast()`, `sigaction()`, `usleep()`, `getopt()`, `fopen()`, `fscanf()`
