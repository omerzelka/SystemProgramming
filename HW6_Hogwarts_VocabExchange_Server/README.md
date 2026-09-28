# HW6 — Hogwarts Potion Brewing & Inventory Server

An event-driven, single-threaded TCP client-server system themed around Hogwarts. Wizards brew and consume ingredients while Professors inspect inventory — all managed through a central server with role-based access control, idle timeouts, and full event logging.

## Architecture

```
                  ┌────────────────────────────┐
                  │     hogwarts (Server)       │
                  │  ┌──────────────────────┐   │
                  │  │  select() event loop │   │
                  │  │  ┌────────────────┐  │   │
                  │  │  │ Ingredient DB  │  │   │
                  │  │  │ Client Pool    │  │   │
                  │  │  │ Role-Based ACL │  │   │
                  │  │  └────────────────┘  │   │
                  │  └──────────────────────┘   │
                  └─────┬──────────────┬────────┘
                   TCP  │              │  TCP
              ┌─────────┘              └──────────┐
              ▼                                   ▼
     ┌─────────────┐                     ┌──────────────┐
     │   wizard     │                    │  professor    │
     │  (Student)   │                    │  (Faculty)    │
     │ BREW/CONSUME │                    │ INSPECT/SCROLL│
     │ SPELLBOOK    │                    │ ROSTER        │
     └─────────────┘                     └──────────────┘
```

## Key Concepts

| Concept | Implementation |
|---|---|
| **Networking** | IPv4 TCP sockets (`AF_INET`, `SOCK_STREAM`) |
| **Concurrency** | `select()` I/O multiplexing (single-threaded, no fork) |
| **Protocol** | Line-delimited ASCII over TCP with buffer accumulation |
| **Access Control** | Role-based (Wizard vs. Professor) command enforcement |
| **Idle Timeout** | Dynamic `select()` timeout disconnects inactive clients |
| **Signal Handling** | `SIGINT` graceful shutdown, `SIGPIPE` ignored |

## Protocol & Commands

| Command | Role | Description |
|---|---|---|
| `ENROLL <ROLE> <name>` | Both | Register with username and role |
| `BREW <ingredient> <qty>` | Wizard | Add to global stock & personal spellbook |
| `CONSUME <ingredient> <qty>` | Wizard | Remove from stock (must have brewed enough) |
| `SPELLBOOK` | Wizard | List personally brewed ingredients |
| `INSPECT <ingredient>` | Professor | Query stock of one ingredient |
| `SCROLL` | Professor | List entire global inventory |
| `ROSTER` | Professor | List all enrolled users |
| `APPARATE` | Both | Graceful disconnect |

## Files

| File | Description |
|---|---|
| `hogwarts.c` | Server — event loop, inventory, client management, logging |
| `wizard.c` | Wizard client — brew, consume, spellbook |
| `professor.c` | Professor client — inspect, scroll, roster |
| `Makefile` | Builds `hogwarts`, `wizard`, `professor` |
| `vocabs.doc` | Sample ingredient/vocabulary data |
| `*.png` | Execution screenshots and scenario demonstrations |
| `230104004020_report.pdf` | Project report |

## Build & Run

```bash
make
```

### Server
```bash
./hogwarts -p <port> -s <ingredients.txt> -l <logfile> -n <maxClients> -t <timeout>
```

| Flag | Description |
|---|---|
| `-p` | TCP port to listen on (≥ 1024) |
| `-s` | Path to ingredients file |
| `-l` | Path to server log file |
| `-n` | Maximum concurrent clients |
| `-t` | Idle timeout in seconds |

### Clients
```bash
./wizard <serverIP> <port> <username>
./professor <serverIP> <port> <username>
```

### Example Session
```bash
# Terminal 1 — Start server
./hogwarts -p 8080 -s vocabs.doc -l hogwarts.log -n 5 -t 30

# Terminal 2 — Wizard
./wizard 127.0.0.1 8080 Harry
> BREW unicorn_hair 10
> CONSUME unicorn_hair 3
> SPELLBOOK
> APPARATE

# Terminal 3 — Professor
./professor 127.0.0.1 8080 Snape
> INSPECT unicorn_hair
> SCROLL
> ROSTER
> APPARATE
```

## System Calls Used

`socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `select()`, `read()`, `write()`, `close()`, `setsockopt()`, `sigaction()`, `getopt()`, `clock_gettime()`
