# Lesson 4: data that survives a restart

**Goal:** `SET` a key, kill the server as rudely as possible, start it again, and `GET` the key back.

Until now, Ctrl+C erased everything: the map lived only in memory. The fix is the one Redis calls an **append-only file (AOF)**: write every change to the end of a file, and replay the file when the server starts.

**Client → command → append to file → change the map → `OK`**

## Build and run

Stop any running server with Ctrl+C, then rebuild. The new code is in `append_log.hpp`, and `server_multi.cpp` uses it:

```sh
cd cpp-kv-server
clang++ -std=c++20 -Wall -Wextra -Wpedantic -g \
  -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1 \
  server_multi.cpp -o kvserver_multi
./kvserver_multi --aof data.aof
```

The server prints `Loaded 0 records from data.aof` and then its usual `Listening on ...` line. `--aof data.aof` names the log file; it is created if it doesn't exist. Without `--aof`, the server behaves exactly as in Lesson 3.

## Try it

In terminal 2:

```sh
nc 127.0.0.1 6379
```

```text
SET user:42 Arnav
OK
SET city New York
OK
DEL city
DELETED
GET user:42
VALUE Arnav
DEL nothing
NOT_FOUND
```

Now crash the server from terminal 3. `kill -9` stops a process instantly, with no chance to clean up:

```sh
pkill -9 kvserver_multi
cat data.aof
```

```text
SET user:42 Arnav
SET city New York
DEL city
```

The `GET` and the failed `DEL nothing` are not in the file: they changed nothing, so there is nothing to replay. Start the server again in terminal 1 with the same command. It prints `Loaded 3 records from data.aof`. Connect, run `GET user:42`, and you get `VALUE Arnav`.

## Code and lessons side by side

Read `kv_store.hpp` first: Lesson 1's `execute_command` was split into smaller pieces. Then read `append_log.hpp` in this order: `open`, `append`, `roll_back`, `sync_if_due`. Finally, find `handle_command` and the argument loop in `server_multi.cpp`.

| Code | What it means |
| --- | --- |
| `struct Command` | A parsed command: what to do (`kind`), which `key`, and for `SET`, which `value`. |
| `enum class Kind { Get, Set, Del }` | A type with exactly three named values. `enum class` names must be written as `Command::Kind::Set`, so they can't be confused with plain numbers. |
| `std::optional<Command>` | “Either a `Command` or nothing.” `parse_command` returns nothing (`std::nullopt`) when the line is invalid. `if (!command)` tests for that; `*command` gets the value. |
| `switch (command.kind)` | Choose a branch by value. The compiler warns if an enum value has no `case`. |
| `changes_data(...)` | True only for `SET`, or `DEL` of an existing key. Only these are logged. |
| `to_log_record(...)` | The line written to disk: `SET key value\n` or `DEL key\n`. |
| `::open(path, O_RDWR \| O_CREAT \| O_APPEND ...)` | Open a file and get a file descriptor: the same kind of integer handle as a socket. `O_APPEND` makes every write go to the end. |
| `::write(fd, ...)` | Like `send()`, but for files. It may write fewer bytes than asked, so `write_all` loops. |
| `::fsync(fd)` | Wait until the file's data has been handed to the storage device. |
| `::ftruncate(fd, size)` | Cut a file back to `size` bytes. |
| `std::chrono::steady_clock` | A clock that only moves forward, which is right for measuring “one second from now.” |
| `AppendLog* log = nullptr` | A pointer that may point to the log or to nothing. `nullptr` means persistence is off. |

## The most important idea: log first, then change, then reply

`handle_command` in `server_multi.cpp` does the steps in this order:

1. **Parse** the line into a `Command`.
2. **Append** it to the log, if it changes data.
3. **Apply** it to the map.
4. **Reply** `OK`.

If step 2 fails (for example, the disk is full), the client gets `ERROR persistence failed`, and the map is not changed. So a client that saw `OK` can trust the change was written down. This order is called **write-ahead logging**.

On startup, `AppendLog::open` reads the file line by line and runs each line through the same `parse_command` and `apply_command` that clients use. A history of changes, replayed in order from an empty map, rebuilds the exact same map.

## How safe is “written”?

There are two levels:

- `write()` copies bytes into the operating system's memory. That survives **our program** crashing, which is what `kill -9` tests.
- `fsync()` waits until the bytes reach the disk. Only that survives the **whole computer** losing power. It is slow, so `--fsync` lets you choose:

| `--fsync` | When we call `fsync()` | If the power fails |
| --- | --- | --- |
| `always` | After every change, before replying | Nothing acknowledged is lost, but every write waits for the disk |
| `everysec` (default) | At most once per second | Up to about one second of changes may be lost |
| `no` | Never; the OS flushes when it likes | Possibly many seconds of changes may be lost |

For `everysec`, `poll()` gets a timeout instead of `-1`: “wake me when a client is ready, **or** when the next fsync is due.” That's `poll_timeout_ms()`.

On macOS, `fsync()` reaches the drive, but the drive itself may still cache the data. Full protection needs `fcntl(fd, F_FULLFSYNC)`. We use plain `fsync()` because it works on Linux too.

## What happens when the crash hits mid-write?

A crash can leave half a record at the end of the file: `SET user:42 Arn` with no `\n`. We can't know whether it was meant to be `Arn` or `Arnav`, so **a final line without a newline is dropped**.

Dropping it from memory isn't enough. The next change would be added right after the fragment, giving `SET user:42 ArnSET a 1`. So the server also **cuts the file back** with `ftruncate` to the end of the last complete line.

A bad line in the *middle* is different. Crashes only damage the end, so this means something else went wrong. The server refuses to start, prints `corrupt record N at byte B`, and leaves the file alone for you to inspect.

## What changed in the project

- `kv_store.hpp`: `parse_command`, `changes_data`, `to_log_record`, and `apply_command`. `execute_command` still exists and now calls these, so `main.cpp` and `server.cpp` did not change.
- `append_log.hpp`: the new `AppendLog` class (open and replay, append, fsync, repair).
- `server_multi.cpp`: the `--aof` and `--fsync` options, `handle_command`, and the `poll()` timeout.
- `tests/test_persistence.py`: kills the server with `SIGKILL`, restarts it on the same file, and checks the data, including torn and corrupt logs.

## Current limits

- **The log only grows.** A thousand `SET`s of one key leave a thousand lines, and startup replays all of them. Lesson 5 fixes this.
- **Disk writes pause the event loop.** Writing to a file can't return `EAGAIN` the way a socket can, so with `--fsync always` every client waits for every flush.
- **Startup reads the whole file into memory at once.** Fine for learning; a huge log would be read in pieces.
- **No checksums.** A flipped bit that still looks like a valid command would go unnoticed.

## Checks included

```sh
python3 tests/test_persistence.py ./kvserver_multi
```

The test uses a temporary file and a free port. It checks that data survives `SIGKILL` (including values with spaces and empty values), that `GET` and no-op `DEL` are not logged, that a torn final record is dropped and cut off, that a corrupt middle record stops startup without touching the file, and that all three fsync policies work. The Lesson 3 tests still pass without `--aof`.

## Your next learning exercise

1. With the server stopped, simulate a crash mid-write: `printf 'SET torn val' >> data.aof`. Predict what the server prints on startup, then run it. Check `GET torn` and `cat data.aof`.
2. Simulate corruption: put a line `BOGUS` in the middle of `data.aof` with a text editor. Start the server and read the error. Remove the line and start again.
3. Explain why we write to the log *before* changing the map and replying, instead of after.
4. Explain why `kill -9` loses nothing even with `--fsync no`, and what kind of failure would.

Next is **compaction**: rewriting the log so it holds the current data instead of the whole history, without stopping the server.

References: [Redis persistence](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/), [POSIX fsync](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fsync.html).
