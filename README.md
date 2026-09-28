# CSE344 — System Programming

Gebze Technical University — CSE344 System Programming course assignments and final project.

> **Language:** C (C11, POSIX)  
> **Topics:** Multi-process/multi-threaded programming, IPC, sockets, synchronization, signal handling  
> **Environment:** macOS / Linux, gcc/clang, Makefile, Valgrind

## Assignments

| # | Directory | Project | Key Concepts |
|---|---|---|---|
| HW1 | [`FileSearch(HW1)`](FileSearch(HW1)/) | Advanced POSIX File Search | File system traversal, regex, permissions, tree output |
| HW2 | [`MultiProcess_FileSearch(HW2)`](MultiProcess_FileSearch(HW2)/) | Multi-Process File Search | `fork`, worker processes, pattern matching |
| HW3 | [`WordTransport_System(HW3)`](WordTransport_System(HW3)/) | Multi-Process Word Transport | Shared memory, semaphores, elevators, sorting processes |
| HW4 | [`MultiProcess_LogAnalyzer(HW4)`](MultiProcess_LogAnalyzer(HW4)/) | Multi-Process Concurrent Log Analyzer | `fork`, shared memory, semaphores, pipes, `select()` watchdog |
| HW5 | [`Multithreaded_CargoDelivery(HW5)`](Multithreaded_CargoDelivery(HW5)/) | Multi-Threaded Cargo Delivery Simulator | pthreads, mutex, condition variables, atomics, priority queue |
| HW6 | [`Hogwarts_VocabExchange_Server(HW6)`](Hogwarts_VocabExchange_Server(HW6)/) | Hogwarts Potion Brewing & Inventory Server | TCP sockets, `select()` I/O multiplexing, role-based access control |
| Final | [`StockExchange_Server(HW7)`](StockExchange_Server(HW7)/) | Concurrent Stock Exchange Simulator | TCP + UDP sockets, `select()`, broadcasting, portfolio management |

## Build

Each directory has its own `Makefile`:

```bash
cd <project_directory>
make
```

## Author

Ömer Zelka — 230104004020
