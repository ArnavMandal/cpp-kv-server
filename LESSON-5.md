# Lesson 5: shrinking the log without stopping the server

**Goal:** turn a log of 500 `SET`s into one line per key, while clients keep getting answers the whole time.

The Lesson 4 log is a full history. Set one key 500 times and the file holds 500 lines, and every restart replays all of them to reach the one value that matters. **Compaction** writes a new log containing only the *current* data, then swaps it in.

**Server → copy of itself writes the new log → server swaps it in → old history gone**

## Build and run

Stop any running server, then rebuild (`append_log.hpp` and `server_multi.cpp` changed):

```sh
cd cpp-kv-server
clang++ -std=c++20 -Wall -Wextra -Wpedantic -g \
  -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1 \
  server_multi.cpp -o kvserver_multi
rm -f data.aof
./kvserver_multi --aof data.aof
```

## Try it

In terminal 2, overwrite one key 500 times. `seq` prints the numbers 1 to 500, and the loop turns each into a command for `nc`:

```sh
for i in $(seq 1 500); do echo "SET counter $i"; done | nc 127.0.0.1 6379 > /dev/null
wc -l data.aof
```

`wc -l` reports `500 data.aof`. Now connect and compact:

```sh
nc 127.0.0.1 6379
```

```text
SET name Arnav
OK
COMPACT
COMPACTING
```

Terminal 1 prints `Compacted data.aof: 7907 -> 31 bytes`. Back in terminal 3, `cat data.aof` shows:

```text
SET name Arnav
SET counter 500
```

Your order may differ: the new log follows the hash map's internal order, and replay doesn't care about order between different keys. Restart the server: both keys are still there.

You rarely need to type `COMPACT`. The server compacts automatically when the log is at least 1 MiB **and** has doubled since it was last loaded or compacted. To watch that on small data, start with `./kvserver_multi --aof data.aof --compact-min 1000` and run the loop again.

## Code and lessons side by side

Start in `append_log.hpp` with `start_compaction`, then `finish_compaction`, then `should_compact`. In `server_multi.cpp`, find the `compact` lambda and the lines that use `compaction_fd()`.

| Code | What it means |
| --- | --- |
| `fork()` | Create a second process that is an exact copy of this one, memory included. |
| `if (pid == 0)` | `fork()` returns twice: `0` in the new **child** process, and the child's ID in the original **parent**. The same code splits into two paths. |
| `pipe(done)` | A one-way channel between processes: `done[1]` is the write end, `done[0]` the read end. |
| `waitpid(child_, ...)` | Collect a finished child and its exit status. |
| `_exit(...)` | End the process immediately, skipping destructors and output flushing. |
| `rename(old, new)` | Give a file a new name, replacing whatever had that name, in one step. |
| `template <typename Visit> void for_each(Visit visit)` | A **template** works with any type. Here, `visit` can be any function or lambda that accepts a key and a value. |
| `[&](const std::string& key, ...) { ... }` | A **lambda**: a small unnamed function written inline. `[&]` lets it use local variables, such as `batch`, by reference. |
| `std::function<std::string()> compact` | A variable that holds any callable that takes nothing and returns a string. Both `COMPACT` and the automatic trigger call it. |
| `std::vector<int> inherited_fds` | The sockets the child must close (see below). |
| `rewrite_buffer_` | Changes that arrived while the child was working. |

## The hard part: not stopping the server

The simple way is to loop over the map and write a new file. But while that loop runs, our single-threaded server answers nobody. With a lot of data, every client freezes.

Instead we copy Redis's design and let the operating system make a copy:

```text
           fork()
server ─────┬──────────────────────────────────────────▶ keeps serving clients
            │   new changes → old log  AND  rewrite buffer
            │                                             ▲
            └─ child: frozen copy of the map             │ status byte through the pipe
               writes data.aof.rewrite, fsyncs, exits ───┘
                                                          then the server:
                                                          1. adds the rewrite buffer
                                                          2. fsyncs
                                                          3. renames over data.aof
                                                          4. fsyncs the directory
```

**1. `fork()` makes a frozen snapshot.** The child gets a copy of the server's memory at that instant. The parent keeps changing *its* map; the child never sees those changes, so it can take its time. The OS doesn't really copy all the memory up front. Both processes share it until one of them changes a page, and only that page gets copied. This is called **copy-on-write**.

**2. The parent remembers what the snapshot missed.** A change made after `fork()` is still written to the old log, so it's safe if we crash. It is *also* saved in `rewrite_buffer_`. When the child is done, the parent adds the buffer to the end of the new file. Snapshot + buffer = current data.

**3. `rename()` swaps the files in one step.** Anyone opening `data.aof` gets either the complete old log or the complete new one, never a mix. Before renaming, we `fsync` the new file so its contents are on disk. After renaming, we `fsync` the **directory**, because a file's name is stored in its directory.

**4. `poll()` tells us when the child is done.** The parent keeps the pipe's read end and watches it in `poll()`, next to the client sockets. The child writes one status byte and exits. If the child crashes, the pipe simply closes. Either way `poll()` wakes up, and `finish_compaction` runs. This is the readiness idea from Lesson 3, applied to a process.

## Two details that are easy to get wrong

**The child must close the sockets it inherited.** `fork()` copies every open file descriptor, including client connections. Suppose a client sends `EXIT` and the server closes its socket. If the child still holds a copy, the connection stays open, and the client waits until the compaction finishes. That is why `compact` passes the child a list of sockets to close.

**The child must call `_exit()`, not `exit()` or `return`.** Those run destructors and flush `std::cout`, and the child's copies of those belong to the parent. `AppendLog`'s destructor would fsync and close the log, and buffered output could be printed twice. `_exit()` stops the process immediately.

## What changed in the project

- `kv_store.hpp`: `for_each`, so code can visit every entry without seeing the map itself.
- `append_log.hpp`: `should_compact`, `start_compaction`, `finish_compaction`, and the rewrite buffer. Startup now deletes a leftover `data.aof.rewrite`.
- `server_multi.cpp`: the `COMPACT` command, `--compact-min`, watching the child's pipe in `poll()`, and the automatic trigger.
- `tests/test_compaction.py`: compaction, changes made during it, crashes in the middle, and automatic compaction.

## What can go wrong

| Failure | Result |
| --- | --- |
| The child fails or is killed | Temp file deleted, old log untouched. The buffer is discarded, which is safe because those changes are already in the old log. |
| The server crashes during compaction | The old log is complete. The leftover `data.aof.rewrite` is deleted at the next start. |
| A crash after `rename()` | The new log already holds snapshot + buffer, fsynced. |
| Compaction keeps failing (e.g. disk full) | The automatic trigger waits for the log to double again instead of retrying every loop turn. |

## Current limits

- **The rewrite buffer has no size limit.** Many writes during a slow compaction all wait in memory.
- **Memory can double** in the worst case, if the parent changes every page while the child is writing.
- **`fork()` pauses briefly** to set up the copy; longer for very large processes.
- **The final swap runs in the event loop.** It is fast because the buffer is small compared with the whole snapshot.

## Checks included

```sh
python3 tests/test_compaction.py ./kvserver_multi
```

The test checks that history becomes one `SET` per live key, deleted keys disappear, and new changes go to the new file. It uses the test-only environment variable `KV_TEST_COMPACT_DELAY_MS` to make the child slow. Then it confirms the server keeps answering within half a second, refuses a second `COMPACT`, closes an exiting client's connection promptly, and keeps changes made during compaction. It also crashes the server mid-compaction and checks that the old log survives. Last, it checks that automatic compaction keeps a constantly overwritten key's log small.

## Your next learning exercise

1. Watch the two processes. Start the server with a slow child:
   `KV_TEST_COMPACT_DELAY_MS=10000 ./kvserver_multi --aof data.aof`
   Send `COMPACT`, then within 10 seconds run `ps -ax -o pid,ppid,command | grep kvserver_multi` and `ls`. Find the child (its parent ID is the server's ID) and `data.aof.rewrite`. While you wait, `SET` and `GET` from another client still answer.
2. In the same setup, send `SET late yes` during those 10 seconds. After `Compacted ...` appears, find `late` in `data.aof`. Which part of the code put it there?
3. Explain why the new file must be fsynced *before* `rename()`, and the directory *after*.
4. Explain why throwing away the rewrite buffer is harmless when the child fails.

Next directions: **key expiration** (`SET ... EX seconds`, using a `poll()` timeout as with `everysec`), or a **benchmark** to measure what `--fsync always` really costs.

References: [Redis persistence](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/), [POSIX fork](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fork.html), [POSIX rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html).
