# CSE344 — System Programming

Gebze Technical University — CSE344 System Programming course assignments and final project.

> **Language:** C (C11, POSIX)  
> **Topics:** Multi-process/multi-threaded programming, IPC, sockets, synchronization, signal handling  
> **Environment:** macOS / Linux, gcc/clang, Makefile, Valgrind

## Assignments

| # | Directory | Project | Key Concepts |
|---|---|---|---|
| HW1 | [`HW1_FileSearch`](HW1_FileSearch/) | Advanced POSIX File Search | File system traversal, regex, permissions, tree output |
| HW2 | [`HW2_MultiProcess_FileSearch`](HW2_MultiProcess_FileSearch/) | Multi-Process File Search | `fork`, worker processes, pattern matching |
| HW3 | [`HW3_WordTransport_System`](HW3_WordTransport_System/) | Multi-Process Word Transport | Shared memory, semaphores, elevators, sorting processes |
| HW4 | [`HW4_MultiProcess_LogAnalyzer`](HW4_MultiProcess_LogAnalyzer/) | Multi-Process Concurrent Log Analyzer | `fork`, shared memory, semaphores, pipes, `select()` watchdog |
| HW5 | [`HW5_Multithreaded_CargoDelivery`](HW5_Multithreaded_CargoDelivery/) | Multi-Threaded Cargo Delivery Simulator | pthreads, mutex, condition variables, atomics, priority queue |
| HW6 | [`HW6_Hogwarts_VocabExchange_Server`](HW6_Hogwarts_VocabExchange_Server/) | Hogwarts Potion Brewing & Inventory Server | TCP sockets, `select()` I/O multiplexing, role-based access control |
| Final | [`FINAL_StockExchange_Server`](FINAL_StockExchange_Server/) | Concurrent Stock Exchange Simulator | TCP + UDP sockets, `select()`, broadcasting, portfolio management |

## Build

Each directory has its own `Makefile`:

```bash
cd <project_directory>
make
```

## Author

Ömer Zelka — 230104004020
