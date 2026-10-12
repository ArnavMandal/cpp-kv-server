# Lesson 3: multiple clients on your Mac

**Goal:** leave client A connected and idle while client B still gets responses.

We use `poll()`, available on macOS and Linux. It asks the operating system which sockets are ready. This teaches the event-loop design without installing a VM. Linux `epoll` can be a later implementation of the same idea; this project does not implement it yet.

## Start with the experiment

Stop the earlier server with Ctrl+C if it is using port 6379. In terminal 1:

```sh
cd cpp-kv-server
./kvserver_multi
```

Build the executable before running it, and rebuild after changing the code:

```sh
clang++ -std=c++20 -Wall -Wextra -Wpedantic -g \
  -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1 \
  server_multi.cpp -o kvserver_multi
```

In terminal 2, connect and store a value:

```sh
nc 127.0.0.1 6379
```

```text
SET user:42 Arnav
OK
```

**Leave terminal 2 connected.** Open terminal 3:

```sh
nc 127.0.0.1 6379
```

```text
GET user:42
VALUE Arnav
SET user:42 Updated
OK
```

Back in terminal 2, `GET user:42` now returns `VALUE Updated`. Neither client had to disconnect. `EXIT` disconnects only that client; Ctrl+C in terminal 1 stops the server.

If another program needs port 6379, use `./kvserver_multi 6380` and connect with `nc 127.0.0.1 6380` instead.

## Three ideas behind the change

**1. Nonblocking sockets:** a normal `recv()` waits when there is no data. A nonblocking `recv()` immediately returns an error code such as `EAGAIN`, meaning “nothing available now; try again later.” We apply this to both the listening socket and every accepted client socket. Sending must also be nonblocking: a client can stop reading.

**2. One event loop:** `poll()` sleeps until something can make progress. The loop then handles ready connections and repeats. There is one server thread, so commands execute one at a time, but many clients can remain connected. We do only a bounded amount of reading, writing, and accepting per turn so one busy connection cannot monopolize the loop.

**3. Per-client memory:** each client owns its unfinished command and pending responses. The key-value map is shared. When a write sends only part of a response, we save our position and send the rest when the socket is writable again.

```text
Client A → [A's unfinished line / pending responses] ─┐
                                                   ├→ one event loop → shared store
Client B → [B's unfinished line / pending responses] ─┘
```

## Code and lessons side by side

All new networking code is in `server_multi.cpp`. Start at `main()` and its `while (true)` loop, then read the helpers.

| Code | What it means |
| --- | --- |
| `fcntl(fd, F_SETFL, flags \| O_NONBLOCK)` | Make socket operations return instead of waiting for data or space. |
| `Client` | Remember one connection's input, output, send position, and closing state. |
| `std::unordered_map<int, Client> clients` | Look up each client's state by its socket descriptor. |
| `pollfd` | A socket plus the events we want and the events that occurred. |
| `POLLIN` | A read can make progress, or a listening socket can accept a connection. |
| `POLLOUT` | A write can make progress; it may still send only part of a response. |
| `poll(..., -1)` | Sleep until an event occurs, without repeatedly checking in a tight loop. |
| `read_ready()` | Read up to 1,024 bytes, find complete lines, and queue responses. |
| `write_ready()` | Try sending up to 64 KiB, saving the position if any bytes remain. |
| `closing` | Stop accepting commands from this connection, send its pending replies, then close. |

The shape of the loop is:

```cpp
// Simplified outline; see server_multi.cpp for the complete implementation.
while (true) {
    // Build the list of sockets and the events we care about.
    // poll(): wait for any of them to become ready.
    // Accept a small batch of new clients.
    // For each ready client, do a little reading and/or writing.
    // Remove connections that have finished or failed.
}
```

We request `POLLOUT` only when we have a response queued. Otherwise an idle but writable socket could keep waking the loop unnecessarily.

## What happens when a client stops reading?

Its queued responses eventually grow. At 64 KiB of pending output, we pause reads from that client. This prevents us from endlessly executing more commands and generating more replies. When its replies drain below that threshold, we resume reading. Slowing the producer to match the consumer is called **backpressure**.

The 64 KiB threshold is checked between reads; one read can produce more output than that. A separate 1 MiB hard limit guards each client's pending response bytes. Exceeding it disconnects that client, and outstanding responses may be lost. Socket buffers maintained by the operating system are separate from these application buffers. The stored key-value data has no total memory limit yet.

Other limits: 4,096 bytes per command, up to 128 accepted clients, and at most 16 accepts per loop turn. Connections beyond the client limit are closed. Idle clients have no timeout yet.

## Checks included

```sh
python3 tests/test_multi.py ./kvserver_multi
```

The test starts its own server on an available local port and stops it afterward. It checks idle and concurrent clients, input isolation, shared data, fragmented/batched commands, pending responses after input EOF, line limits, a slow reader draining about 4 MB in order, and abrupt resets. These are correctness tests, not performance benchmarks.

`kvserver` / `server.cpp` remain the one-client example. `kvserver_multi` / `server_multi.cpp` are this lesson. Both use `kv_store.hpp`.

## Explain these before moving on

1. If `recv()` returns `EAGAIN`, should we disconnect the client or wait for another readiness event?
2. Why does every client need its own unfinished-line buffer?
3. Why do we keep an output buffer instead of calling the old blocking `send_all()`?

Next is **persistence**: recording changes to disk so data survives a server restart. `poll()` examines a list of sockets each turn; Linux `epoll` and macOS `kqueue` are optional later directions when exploring larger connection counts.

Reference: [POSIX poll specification](https://pubs.opengroup.org/onlinepubs/9799919799/functions/poll.html).
