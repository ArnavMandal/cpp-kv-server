# Lesson 4: data that survives a restart

**Goal:** `SET` a key, kill the server as rudely as possible, start it again, and `GET` the key back.

Until now the store lived only in memory: when the process ended, the data was gone. We fix that the way Redis's AOF does: **append every change to a file, and replay the file at startup.**

## Start with the experiment

Rebuild first (the new code is in `append_log.hpp` and `server_multi.cpp`):

```sh
clang++ -std=c++20 -Wall -Wextra -Wpedantic -g \
  -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1 \
  server_multi.cpp -o kvserver_multi
```

Terminal 1:

```sh
./kvserver_multi --aof data.aof
```

Terminal 2:

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
```

Now crash the server from terminal 3. `kill -9` gives the program no chance to clean up:

```sh
pkill -9 kvserver_multi
cat data.aof
```

```text
SET user:42 Arnav
SET city New York
DEL city
```

Start it again in terminal 1. It prints `Loaded 3 records from data.aof`, and `GET user:42` returns `VALUE Arnav`. Without `--aof`, the server behaves exactly as in Lesson 3.

## Three ideas behind the change

**1. Log first, then apply, then reply (write-ahead).** For a `SET`, the server writes the record to the file, *then* changes the map, *then* queues `OK`. If writing fails, the client gets `ERROR persistence failed` and the map is unchanged. A client that saw `OK` can trust the change was recorded. `GET`s and `DEL`s of missing keys change nothing, so they are not logged.

**2. Replay rebuilds the state.** The log is a history, not a snapshot. Running every record in order, starting from an empty map, ends in the same state the server had. That is why the log reuses our own command format: replay is just `parse_command` + `apply_command`, the same code clients use.

**3. "Written" has levels.** `write()` copies bytes into the operating system's memory (the page cache). That survives our process crashing, which is what `kill -9` tests. It does **not** survive a power cut, because the OS may not have reached the disk yet. `fsync()` asks the OS to push the file to the storage device and waits. It is slow, so `--fsync` picks the trade-off:

| `--fsync` | When we call `fsync()` | If the power fails |
| --- | --- | --- |
| `always` | After every change, before replying | Nothing acknowledged is lost, but every write waits for the disk |
| `everysec` (default) | At most once per second, from the event loop | Up to about one second of changes may be lost |
| `no` | Never; the OS flushes when it likes | Possibly many seconds of changes may be lost |

On macOS, `fsync()` reaches the drive but the drive may still cache it; full protection needs `fcntl(fd, F_FULLFSYNC)`. We keep plain `fsync()` because it is portable, but production systems on Macs use the stronger call.

## Code and lessons side by side

| Code | What it means |
| --- | --- |
| `parse_command` / `apply_command` | Lesson 1's `execute_command`, split in two so the server can log between parsing and applying. |
| `changes_data` | Only log commands that actually modify the map. |
| `to_log_record` | Canonical form: `SET key value\n` or `DEL key\n`. Extra spaces before the key and CRLF are normalized; the value is kept byte for byte. |
| `O_APPEND` | Every `write()` lands at the current end of the file. |
| `handle_command` (server) | Write-ahead order: parse, log, apply, reply. |
| `AppendLog::open` | Read the whole file, replay each complete line, and report how many records were loaded. |
| `ftruncate` | Cut the file back to a given length. Used to drop torn records and undo failed writes. |
| `poll_timeout_ms` / `sync_if_due` | With `everysec`, `poll()` wakes up when an `fsync` is due, even if no client is talking. |

## What happens when the crash hits mid-write?

A record can be cut short: `SET user:42 Arn` with no `\n`. We can't tell whether that was meant to be `Arn` or `Arnav`, so a **final record without a newline is dropped**. Even if it happens to parse, we can't trust that it's complete.

Dropping it in memory is not enough. The next `SET a 1\n` would be appended right after the fragment, creating `SET user:42 ArnSET a 1\n`: a valid-looking line with the wrong value. So startup also **truncates the file** back to the last complete record. The test suite checks exactly this.

Damage *before* the last line is different. A crash only cuts off the end, so a bad record in the middle means something else went wrong (a disk error or a human edit). The server refuses to start, reports `corrupt record N at byte B`, and leaves the file untouched for you to inspect. Silently skipping it could resurrect deleted keys or lose updates.

If a `write()` fails halfway (for example, a full disk), `append` truncates the file back to its previous length so it never contains half a record. If an `everysec` `fsync` fails, we can't know which acknowledged changes were lost, so the server stops instead of pretending.

## Trade-offs we accepted

- **Disk writes block the event loop.** Writes to a regular file don't support `EAGAIN` the way sockets do. With `--fsync always`, every client waits for every disk flush. Redis moves `fsync` to a background thread for `everysec`.
- **The log only grows.** A thousand `SET`s of one key leave a thousand lines, and startup replays all of them. The fix is **compaction**: write a fresh log containing one `SET` per live key, then atomically `rename()` it over the old one.
- **Startup reads the whole file into memory.** Fine for a learning project; a large log would be streamed.
- **No checksums.** Bit flips that still form a valid command would go undetected. Real logs store a length and CRC per record.

## Checks included

```sh
python3 tests/test_persistence.py ./kvserver_multi
```

The test kills the server with `SIGKILL` and restarts it on the same log file. It checks that values survive (including spaces and empty values), that GET and no-op DEL are not logged, that a torn final record is dropped and repaired, that a corrupt middle record blocks startup without modifying the file, and that all three fsync policies work. The Lesson 3 tests still run without `--aof`. These suites also pass with AddressSanitizer and UndefinedBehaviorSanitizer.

## Explain these before moving on

1. Why do we write to the log *before* changing the map and replying, instead of after?
2. `kill -9` doesn't lose data even with `--fsync no`. What kind of failure would, and why?
3. Why is it unsafe to only skip a torn final record in memory without truncating the file?
4. Why does the server refuse to start on a corrupt middle record but accept a torn final one?

Next is **compaction**: rewriting the log so it holds the current state instead of the entire history, without stopping the server.

References: [Redis persistence](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/), [POSIX fsync](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fsync.html).
