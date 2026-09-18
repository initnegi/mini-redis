# Mini Redis

A Redis-inspired in-memory key-value store built from scratch in C++, implementing a custom TCP server, concurrent client handling, TTL-based expiry, LRU eviction, and crash-recoverable persistence - with no external frameworks or libraries beyond the C++ standard library and native Windows sockets (Winsock).

This project was built to deepen practical understanding of systems programming concepts - networking, concurrency, memory management, and cache design - that are usually only studied theoretically in OS/DBMS coursework.

## Features

- **TCP server** -> raw socket-based server (Winsock), no networking frameworks
- **Concurrent multi-client support** -> thread-per-connection model, with a mutex-protected shared store
- **Core commands** -> `SET`, `GET`, `DEL`, `EXISTS`
- **TTL / expiry** -> `EXPIRE key seconds`, `TTL key`, using lazy expiry (checked on access, matching real Redis's default behavior)
- **LRU eviction** -> a fixed-capacity store that evicts the least-recently-used key when full, implemented with a hand-written doubly linked list (no STL container shortcuts) for O(1) access, insertion, and eviction
- **Snapshot persistence** -> `SAVE` command writes the current store to disk; automatically reloaded on server startup, surviving restarts

## Tech Stack

| Component | Choice | Why |
|---|---|---|
| Language | C++23 | Leverages existing C++/OOP background; modern standard library features |
| Networking | Winsock2 (native Windows sockets) | No framework dependency; direct control over the socket lifecycle |
| Concurrency | `std::thread`, `std::mutex` | Standard library concurrency primitives, thread-per-connection model |
| Data structures | `std::unordered_map`, hand-written doubly linked list | Map for O(1) key lookup; custom list (not `std::list`) for full control over LRU pointer manipulation |
| Persistence | Plain text file I/O (`std::ofstream`/`std::ifstream`) | No database dependency; simple, inspectable snapshot format |
| Build | g++ (MinGW-w64 / MSYS2) | Free, no IDE lock-in, works on Windows without WSL or a Linux VM |

No external libraries or frameworks are used anywhere in this project - every networking, concurrency, and data-structure component is built directly on the C++ standard library and OS-level APIs.

## Architecture

```
                        ┌──────────────────┐
Client 1 ─────socket────▶                  │
Client 2 ─────socket────▶  Listening       │──▶ accept() ──▶ spawns thread per client
Client 3 ─────socket────▶  Socket (6379)   │
                        └──────────────────┘
                                                      │
                                                      ▼
                                      ┌───────────────────────────┐
                                      │   handleClient(thread)    │
                                      │   - recv() command        │
                                      │   - parse & dispatch      │
                                      │   - send() response       │
                                      └───────────────────────────┘
                                                      │
                                                      ▼
                                 ┌────────────────────────────────────┐
                                 │  Shared Store (mutex-protected)    │
                                 │                                    │
                                 │  unordered_map<string, Node*>      │
                                 │         │                          │
                                 │         ▼                          │
                                 │  [dummyHead] <-> Node <-> Node     │
                                 │             <-> ... <-> [dummyTail]│
                                 │   (most recent)      (least recent)│
                                 └────────────────────────────────────┘
                                                      │
                                                      ▼
                                          snapshot.txt (on SAVE)
```

Each `Node` holds a key, its value, an optional expiry timestamp, and `prev`/`next` pointers — combining the key-value entry and its LRU-list position into a single heap-allocated structure (avoiding a second lookup map for recency tracking).

## Design Decisions & Tradeoffs

**Why a hand-written doubly linked list instead of `std::list`?**
`std::list` (with `splice()`) would have made LRU reordering trivial, but implementing the pointer manipulation manually (sentinel nodes, `removeNode`/`addToFront`/`moveToFront`) more closely mirrors how this problem is expected to be solved in technical interviews, and builds a deeper understanding of pointer lifetime and manual memory management.

**Why sentinel (dummy) head/tail nodes?**
Using permanent placeholder nodes at both ends of the list eliminates null-pointer edge cases (empty list, single-node list, removing the head/tail) from every list operation - every real node always has a valid, non-null neighbor to relink against.

**Why `steady_clock` instead of `system_clock` for expiry?**
`steady_clock` is guaranteed to be monotonic (never moves backward, immune to system clock adjustments like NTP sync), which matters for correctness when measuring durations. The tradeoff: since `steady_clock` has no meaningful cross-restart reference point, persisted TTLs are stored as *remaining seconds* rather than absolute timestamps, and re-anchored to a fresh `steady_clock::now()` on reload - see Known Limitations below.

**Why lazy expiry instead of a background sweep?**
Checking expiry only when a key is accessed (rather than continuously scanning all keys) avoids unnecessary CPU work on keys that are never touched again - the same default strategy real Redis uses.

**Locking granularity**
A single mutex protects the entire store. This is simpler and safer than fine-grained per-key locking, at the cost of some contention under heavy concurrent load - a reasonable tradeoff for this project's scope, and a known avenue for further optimization (e.g., sharding the store across multiple locks).

## Commands

| Command | Description |
|---|---|
| `SET key value` | Store a value (resets any existing expiry) |
| `GET key` | Retrieve a value, or `(nil)` if missing/expired |
| `DEL key` | Remove a key, returns `1` if removed, `0` if it didn't exist |
| `EXISTS key` | Returns `1` or `0` |
| `EXPIRE key seconds` | Set a TTL on an existing key |
| `TTL key` | Remaining seconds (`-1` = no expiry set, `-2` = key doesn't exist) |
| `SAVE` | Write the current store to `snapshot.txt` |
| `QUIT` | Close the connection |

## Benchmark Results

Measured using a custom Python load-testing script (`benchmark.py`) simulating concurrent clients issuing a mixed SET/GET workload against a small rotating key set (to exercise realistic cache access patterns, including LRU reordering).

| Concurrent Clients | Total Ops | Throughput (ops/sec) | Avg Latency |
|---|---|---|---|
| 10 | 5,000 | ~54,000 | ~0.018 ms |
| 50 | 25,000 | ~54,000-66,500 | ~0.015-0.018 ms |
| 100 | 50,000 | ~55,000-61,000 | ~0.016-0.018 ms |

Throughput remained stable (no meaningful degradation) as concurrency scaled from 10 to 100 clients, indicating the single global mutex is not yet a bottleneck at this scale. All measurements taken on localhost (no network latency); results will vary across hardware.


## Known Limitations

- **Downtime is not accounted for in persistence.** TTLs are saved as "remaining seconds" and re-anchored on load - if the server is down for a while between a `SAVE` and a restart, keys will expire later than they "should" in wall-clock time. Real Redis avoids this by storing absolute Unix timestamps in its RDB format; this project prioritized `steady_clock`'s runtime correctness guarantees over this edge case.
- **Single global lock** -> no lock sharding/striping; all operations serialize on one mutex.
- **No AOF (append-only log)** -> only point-in-time snapshotting is implemented; a crash between two `SAVE`s loses any writes since the last snapshot.
- **Manual `SAVE` only** -> no automatic periodic snapshotting yet (planned).
- **`MAX_KEYS` is set low (3) for demo purposes** — this makes LRU eviction easy to observe/test manually. In a real deployment, this would be set much higher (e.g., 10,000+) or made configurable via a startup argument.

## Building & Running

**Requirements:** g++ with C++23 support (via MSYS2/MinGW-w64 on Windows), no other dependencies.

```bash
# Build the server
g++ -std=c++23 -Wall -Wextra -pthread src/main.cpp -lws2_32 -o server.exe

# Build the test client
g++ -std=c++23 -Wall -Wextra src/test_client.cpp -lws2_32 -o client.exe

# Run the server (in one terminal)
.\server.exe

# Run the client (in a separate terminal)
.\client.exe
```

The server listens on port `6379` (matching real Redis's default port) on all local interfaces.

## Example Session

```
> SET foo bar
OK
> GET foo
bar
> EXPIRE foo 30
1
> TTL foo
28
> SAVE
OK
> QUIT
BYE
```

## Possible Future Extensions

- Automatic periodic snapshotting (background thread)
- Append-only log (AOF) for finer-grained crash recovery
- Real RESP protocol support (so `redis-cli` can connect directly)
- Additional data types (lists, sets, hashes)
- Benchmark suite with throughput/latency measurements under concurrent load
