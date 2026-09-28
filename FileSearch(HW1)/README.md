# HW1 — Recursive File Search (myFind)

A custom `find`-like command-line tool that recursively traverses directories and filters files based on multiple criteria — with a built-in regex engine, signal handling, and tree-style output.

## Architecture

```
myFind
  │
  ├── Parse CLI arguments (getopt)
  ├── Register SIGINT handler (Ctrl+C graceful exit)
  └── searchDirectory(targetDir, depth=0)
        ├── opendir / readdir
        ├── lstat (file metadata)
        ├── isMatch (multi-criteria filter)
        │     ├── Size check (-b)
        │     ├── Link count check (-l)
        │     ├── File type check (-t)
        │     ├── Permissions check (-p)
        │     └── Filename regex (-f) → recursiveRegex
        ├── Print result (bold if matched)
        └── Recurse into subdirectories
```

## Key Concepts

| Concept | Implementation |
|---|---|
| **Directory Traversal** | `opendir()`, `readdir()`, `closedir()` |
| **File Metadata** | `lstat()` — size, type, permissions, link count |
| **Pattern Matching** | Custom recursive regex engine (supports `+` operator) |
| **Signal Handling** | `signal(SIGINT, ...)` for graceful Ctrl+C exit |
| **Tree Output** | Depth-based indentation with bold highlights for matches |

## Files

| File | Description |
|---|---|
| `main.c` | All logic: argument parsing, directory traversal, regex engine, signal handling |
| `Makefile` | Build configuration (`-Wall -g`) |
| `derin_test/` | Test directory structure for deep traversal testing |
| `hw1.pdf` | Assignment specification |

## Build & Run

```bash
make
./myFind -w <target_dir> [-f <filename>] [-b <size>] [-t <type>] [-p <permissions>] [-l <link_count>]
```

### Arguments

| Flag | Description |
|---|---|
| `-w` | Target directory to search (required) |
| `-f` | Filename pattern (supports `+` regex operator, case-insensitive) |
| `-b` | Exact file size in bytes |
| `-t` | File type: `f` (regular), `d` (directory), `l` (symlink), `p` (FIFO), `s` (socket), `b` (block), `c` (char) |
| `-p` | Permission string (e.g., `rwxr-xr--`) |
| `-l` | Exact number of hard links |

### Examples

```bash
./myFind -w /home/user -f "test" -t f
./myFind -w ./derin_test -b 1024 -p "rw-r--r--"
./myFind -w /var/log -t d -l 2
```

## System Calls Used

`opendir()`, `readdir()`, `closedir()`, `lstat()`, `signal()`, `getopt()`, `snprintf()`, `_exit()`
