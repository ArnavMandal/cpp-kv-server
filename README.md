# C++ Key-Value Server

A Redis-style learning project built in C++20: an in-memory key-value store exposed over TCP, with a single-threaded, nonblocking `poll()` event loop.

## Implemented

- `SET`, `GET`, and `DEL` for string keys and values.
- Multiple connected clients sharing one store.
- Per-client input/output buffers, newline framing, partial writes, and RAII socket cleanup.
- Read backpressure for slow clients, a 4,096-byte command limit, and bounded pending output.
- Optional append-only-file persistence (`--aof`), crash recovery, and three fsync policies.
- Background log compaction (`COMPACT`, or automatic) using `fork()` and an atomic `rename()`.
- Automated TCP tests for fragmented requests, concurrent clients, reconnects, slow readers, and connection resets.
- A repeatable throughput/latency benchmark comparing no log and each fsync policy (`bench/`).

The server binds to localhost. Without `--aof`, data is lost on restart. This uses a custom text protocol, not Redis RESP. Expiration and eviction are planned. Linux `epoll` is not implemented.

## Build and run

Requires a C++20 compiler on macOS or Linux. From the repository root:

```sh
c++ -std=c++20 -Wall -Wextra -Wpedantic -g server_multi.cpp -o kvserver_multi
./kvserver_multi
```

If macOS reports `iostream` not found, supply the installed SDK header path:

```sh
clang++ -std=c++20 -Wall -Wextra -Wpedantic -g \
  -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1 \
  server_multi.cpp -o kvserver_multi
./kvserver_multi
```

In another terminal:

```sh
nc 127.0.0.1 6379
```

```text
SET user:42 Arnav
OK
GET user:42
VALUE Arnav
DEL user:42
DELETED
EXIT
BYE
```

Keep one client connected and open another to try concurrent connections. `EXIT` disconnects a client; Ctrl+C stops the server. To choose another port, run `./kvserver_multi 6380`.

To keep data across restarts, add a log file:

```sh
./kvserver_multi --aof data.aof                  # fsync once per second (default)
./kvserver_multi --aof data.aof --fsync always   # or: everysec, no
```

The log is compacted automatically once it is at least 1 MiB and has doubled since the last compaction (`--compact-min BYTES` changes the 1 MiB). Send `COMPACT` to compact it now.

## Tests

Requires Python 3; no third-party packages:

```sh
python3 tests/test_multi.py ./kvserver_multi
python3 tests/test_persistence.py ./kvserver_multi
python3 tests/test_compaction.py ./kvserver_multi
```

The test creates its own server on an available port and stops it afterward. It covers a slow client draining approximately 4 MB of replies while other clients continue working. These are correctness checks; for performance, see [Benchmark](#benchmark). The multi-client suite has also passed with AddressSanitizer and UndefinedBehaviorSanitizer on macOS.

## Benchmark

Build both programs with optimization, then run every persistence mode five times, interleaved:

```sh
c++ -std=c++20 -O2 server_multi.cpp -o kvserver_multi
c++ -std=c++20 -O2 -pthread bench/kvbench.cpp -o kvbench
python3 bench/run_bench.py --note "c++ -O2"
```

Each run starts a fresh server with an empty log on a free port. Workloads are seeded and verify every reply. The script prints median throughput, p50/p99/p99.9 latency for `GET` and `SET`, and the spread across repeats. It saves these results to `bench/results/`, along with the git commit, binary hashes, OS, CPU, and compiler. Use `--compare OLD.json` to see the change between runs, and `--help` for workload options. On an Apple M2, `--fsync always` cut throughput from about 87k to 30k operations per second. [Lesson 6](LESSON-6.md) explains how to read the results and how far to trust them.

Optional CMake build (CMake 3.16+):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CTest registers the Python tests, plus a quick benchmark smoke run, when Python 3 is available. The one-client test requires port 6379 to be free. The multi-client test selects a free port automatically.

## Project layout and lessons

| Files | Purpose |
| --- | --- |
| `kv_store.hpp` | Shared storage, command parsing, and execution |
| `append_log.hpp` | Append-only log: write, fsync, replay, repair, compact |
| `main.cpp` | Local terminal program (`kvstore`) |
| `server.cpp` | One-client TCP example (`kvserver`) |
| `server_multi.cpp` | Multi-client event loop (`kvserver_multi`) |
| `tests/` | End-to-end TCP correctness tests |
| `bench/` | Load generator (`kvbench`) and benchmark driver |
| [Lesson 1](LESSON-1.md) | Local storage and C++ fundamentals |
| [Lesson 2](LESSON-2.md) | TCP sockets and message framing |
| [Lesson 3](LESSON-3.md) | Nonblocking I/O, readiness, and backpressure |
| [Lesson 4](LESSON-4.md) | Persistence with an append-only log |
| [Lesson 5](LESSON-5.md) | Log compaction with `fork()` and `rename()` |
| [Lesson 6](LESSON-6.md) | Benchmarking persistence: throughput, latency, repeatability |

## Protocol and limits

Commands are uppercase and newline-delimited. `COMPACT` and `EXIT` take no arguments. Keys have no whitespace; a `SET` value is the remaining text after one separating space. Values may contain spaces, but not newlines. Missing keys return `NOT_FOUND`. Both LF and CRLF endings are supported.

The multi-client server accepts up to 128 clients. It pauses reads when a client's pending output reaches 64 KiB and enforces a 1 MiB hard limit on pending response bytes. Total stored key-value data is not yet bounded, and idle clients have no timeout. See Lesson 3 for details. While a compaction runs, changes made in the meantime are buffered in memory without a limit; see Lesson 5.
