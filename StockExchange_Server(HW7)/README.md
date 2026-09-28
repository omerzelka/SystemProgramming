# FINAL — Concurrent Stock Exchange Simulator

A single-threaded, event-driven stock exchange server using `select()` I/O multiplexing with TCP (for traders and analysts) and UDP broadcasting (for real-time price tickers). Implements role-based access control, portfolio management, and dynamic price adjustments.

## Architecture

```
                   ┌──────────────────────────────────┐
                   │       ServerTrd (Exchange)        │
                   │  ┌──────────────────────────┐    │
                   │  │  select() event loop      │    │
                   │  │  ┌──────────┬───────────┐ │    │
                   │  │  │ Stock DB │ Portfolios│ │    │
                   │  │  └──────────┴───────────┘ │    │
                   │  └──────────────────────────┘    │
                   └──┬────────────┬────────────┬─────┘
                 TCP  │       TCP  │       UDP  │
            ┌────────┘            │            └────────┐
            ▼                     ▼                     ▼
    ┌──────────────┐     ┌──────────────┐      ┌──────────────┐
    │    trader     │     │   analyst    │      │    ticker    │
    │  BUY / SELL   │     │ PRICE/REPORT │      │  UDP Listen  │
    │  PORTFOLIO    │     │ LIST         │      │  Price Feed  │
    └──────────────┘     └──────────────┘      └──────────────┘
```

## Key Concepts

| Concept | Implementation |
|---|---|
| **I/O Multiplexing** | `select()` — single-threaded event-driven server |
| **TCP Sockets** | Traders & Analysts connect for interactive commands |
| **UDP Broadcasting** | Real-time price updates to all tickers (`255.255.255.255`) |
| **Stream Framing** | Line-delimited protocol with accumulator buffers + `send_all()` |
| **Price Dynamics** | BUY increases price by `qty × 0.01`, SELL decreases (floor: 0.01) |
| **Role Enforcement** | Traders can't query; Analysts can't trade |
| **Signal Handling** | `SIGINT` graceful shutdown, `SIGPIPE` ignored |
| **Periodic Broadcast** | Every 5 seconds via `clock_gettime(CLOCK_MONOTONIC)` + `select()` timeout |

## Protocol & Commands

### Trader Commands
| Command | Description | Response |
|---|---|---|
| `BUY <symbol> <qty>` | Buy shares, increases price | `OK BUY <symbol> <qty> <newPrice>` |
| `SELL <symbol> <qty>` | Sell shares, decreases price | `OK SELL <symbol> <qty> <newPrice>` |
| `PORTFOLIO` | View owned shares | `OK PORTFOLIO <sym>:<qty>,...` |
| `QUIT` | Disconnect | Connection closed |

### Analyst Commands
| Command | Description | Response |
|---|---|---|
| `PRICE <symbol>` | Query single stock price | `OK PRICE <symbol> <price>` |
| `REPORT` | Full market snapshot | `OK REPORT <sym>:<price>,...` |
| `LIST` | List connected users | `OK LIST <user1>,<user2>,...` |
| `QUIT` | Disconnect | Connection closed |

### Ticker (UDP)
Receives broadcasts: `PRICE_UPDATE <sym>:<price> <sym>:<price> ...`

## Files

| File | Description |
|---|---|
| `ServerTrd.c` | Exchange server — event loop, stock DB, portfolios, dual logging |
| `trader.c` | Trader client — BUY, SELL, PORTFOLIO |
| `analyst.c` | Analyst client — PRICE, REPORT, LIST |
| `ticker.c` | Ticker — passive UDP price monitor |
| `Makefile` | Builds all 4 executables |
| `stocks.txt` | Initial stock data (30 stocks with prices) |
| `CSE344_final.pdf` | Exam/project specification |

## Build & Run

```bash
make
```

### Server
```bash
./ServerTrd -p <tcpPort> -u <udpPort> -s <stockFile> -l <logFile>
```

| Flag | Description |
|---|---|
| `-p` | TCP port for trader/analyst connections |
| `-u` | UDP port for ticker broadcasts |
| `-s` | Path to initial stock data file |
| `-l` | Path to server log file |

### Clients
```bash
./trader <serverIP> <tcpPort> <username>
./analyst <serverIP> <tcpPort> <username>
./ticker <udpPort>
```

### Example Session
```bash
# Terminal 1 — Start server
./ServerTrd -p 8080 -u 8081 -s stocks.txt -l server.log

# Terminal 2 — Ticker (passive listener)
./ticker 8081

# Terminal 3 — Trader
./trader 127.0.0.1 8080 alice
> BUY AAPL 100
> SELL GOOGL 50
> PORTFOLIO
> QUIT

# Terminal 4 — Analyst
./analyst 127.0.0.1 8080 bob
> PRICE AAPL
> REPORT
> LIST
> QUIT
```

## System Calls Used

`socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `select()`, `read()`, `write()`, `sendto()`, `recvfrom()`, `setsockopt()`, `sigaction()`, `clock_gettime()`, `getopt()`, `inet_pton()`
