# Lesson 2: talk to the store over TCP

You already proved the storage works. Now the keyboard input comes from a separate program over a connection. Both programs can run on your Mac.

**Client → TCP connection → complete line → execute_command → store → response**

## Build and run

If you are still at the old store's `>` prompt, type `EXIT` first. In terminal 1:

```sh
cd cpp-kv-server
clang++ -std=c++20 -Wall -Wextra -Wpedantic -g \
  -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1 \
  server.cpp -o kvserver
./kvserver
```

Leave it running. In a second terminal:

```sh
nc 127.0.0.1 6379
```

`nc` (netcat) is our client for this lesson. `127.0.0.1` means your own computer; `6379` identifies the listening service. If the server reports “Address already in use”, stop the other process using that port before running it.

Type these lines in terminal 2. Unlike our local program, netcat does not print a `>` prompt:

```text
SET user:42 Arnav Mandal
OK
GET user:42
VALUE Arnav Mandal
EXIT
BYE
```

Connect again with `nc 127.0.0.1 6379` and run `GET user:42`: the value is still there. `EXIT` disconnects the client. Ctrl+C in terminal 1 stops the server and loses the data.

## Code and lessons side by side

| In `server.cpp` | Simple explanation |
| --- | --- |
| `socket(...)` | Ask the operating system for a TCP socket, identified by an integer. |
| `bind(...)` | Assign the listening socket an address and port. |
| `listen(...)` | Allow incoming connections to queue. |
| `accept(...)` | Wait for a client; return a new socket for talking to it. The listening socket stays open. |
| `recv(...)` | Read whatever bytes are currently available, waiting if necessary. Zero means the peer ended its input. |
| `line.push_back(...)` | Save bytes until a newline marks a complete command. |
| `execute_command(store, line)` | Reuse the exact command processor from lesson 1. |
| `send_all(...)` | Keep sending until the complete response is sent or the connection fails. |
| `Socket` destructor | Close the owned socket automatically when its object leaves scope. |

Read the file in this order: `main`, `serve_client`, `send_all`, then `Socket`.

## The most important idea: TCP sends bytes, not commands

You might send `SET name Arnav\n` once, but the server could receive `SET na` and then `me Arnav\n`. Or one read could contain `GET a\nGET b\n`. Newlines tell our program where commands end; a call to `recv` does not.

The unfinished line lives outside the receive loop, so it survives between reads. It has a 4,096-byte limit before the newline. A longer line gets an error and the connection closes. A connection ending halfway through a command discards that unfinished command. CRLF is accepted too; its CR counts toward the limit and is removed before parsing.

## What changed in the project

- `kv_store.hpp`: the same map and command function, shared by both programs. `#pragma once` prevents repeated inclusion in one compilation; `inline` allows this function definition in a shared header.
- `main.cpp`: the original terminal loop, still buildable as `kvstore`.
- `server.cpp`: the new TCP program, built as `kvserver`.
- `tests/test_tcp.py`: exercises the actual TCP connection, including split commands and oversized input. Run `python3 tests/test_tcp.py ./kvserver` with your server stopped first.

## Current limits and next lesson

This server handles **one connected client at a time**. An idle client or a client that does not read responses can hold it up. Other connections may queue, but their commands are not processed until the current client leaves. The command buffer is bounded; the total stored data is not limited yet.

Try two client terminals: keep the first connected, send a command from the second, then type `EXIT` in the first. The second receives its response once the server moves on to it. This is the problem the next milestone—nonblocking sockets and an event loop—will solve.

Before moving on, explain: **Why do we need a listening socket and a separate client socket? Why can't every `recv` result be treated as one command?**
