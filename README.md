# C++ Key-Value Server

A Redis-style learning project built in C++20: an in-memory key-value store exposed over TCP, with a single-threaded, nonblocking `poll()` event loop.

## Implemented

- `SET`, `GET`, and `DEL` for string keys and values.
- Multiple connected clients sharing one store.
- Per-client input/output buffers, newline framing, partial writes, and RAII socket cleanup.
- Read backpressure for slow clients, a 4,096-byte command limit, and bounded pending output.
- Automated TCP tests for fragmented requests, concurrent clients, reconnects, slow readers, and connection resets.

The server binds to localhost. Data is lost on restart. This uses a custom text protocol, not Redis RESP. Persistence, expiration, eviction, and performance benchmarks are planned. Linux `epoll` is not implemented.

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

## Tests

Requires Python 3; no third-party packages:

```sh
python3 tests/test_multi.py ./kvserver_multi
```

The test creates its own server on an available port and stops it afterward. It covers a slow client draining approximately 4 MB of replies while other clients continue working. These are correctness checks, not throughput benchmarks. The multi-client suite has also passed with AddressSanitizer and UndefinedBehaviorSanitizer on macOS.

Optional CMake build (CMake 3.16+):

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

CTest registers the Python tests when Python 3 is available. The one-client test requires port 6379 to be free. The multi-client test selects a free port automatically.

## Project layout and lessons

| Files | Purpose |
| --- | --- |
| `kv_store.hpp` | Shared storage and command processing |
| `main.cpp` | Local terminal program (`kvstore`) |
| `server.cpp` | One-client TCP example (`kvserver`) |
| `server_multi.cpp` | Multi-client event loop (`kvserver_multi`) |
| `tests/` | End-to-end TCP correctness tests |
| [Lesson 1](LESSON-1.md) | Local storage and C++ fundamentals |
| [Lesson 2](LESSON-2.md) | TCP sockets and message framing |
| [Lesson 3](LESSON-3.md) | Nonblocking I/O, readiness, and backpressure |

## Protocol and limits

Commands are uppercase and newline-delimited. Keys have no whitespace; a `SET` value is the remaining text after one separating space. Values may contain spaces, but not newlines. Missing keys return `NOT_FOUND`. Both LF and CRLF endings are supported.

The multi-client server accepts up to 128 clients. It pauses reads when a client's pending output reaches 64 KiB and enforces a 1 MiB hard limit on pending response bytes. Total stored key-value data is not yet bounded, and idle clients have no timeout. See Lesson 3 for details.
