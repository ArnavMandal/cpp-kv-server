# Lesson 5: shrinking the log without stopping the server

**Goal:** turn a log of a thousand `SET`s into one line per key, while clients keep getting answers the whole time.

The Lesson 4 log is a full history. Set one key 500 times and the file holds 500 lines; restart replays all of them to reach the one value that matters. **Compaction** writes a new log describing only the *current* state, then swaps it in.

## Start with the experiment

Rebuild (`append_log.hpp` and `server_multi.cpp` changed), then terminal 1:

```sh
./kvserver_multi --aof data.aof
```

Terminal 2: overwrite one key many times, then compact:

```sh
for i in $(seq 1 500); do echo "SET counter $i"; done | nc 127.0.0.1 6379 > /dev/null
wc -l data.aof          # 500 lines
nc 127.0.0.1 6379
```

```text
SET name Arnav
OK
COMPACT
COMPACTING
```

Terminal 1 prints `Compacted data.aof: 7907 -> 31 bytes` (sizes vary). Now `cat data.aof`:

```text
SET counter 500
SET name Arnav
```

The order may differ: the snapshot follows the hash map's internal order, and replay doesn't care. Restart the server and both keys are still there.

You rarely need to type `COMPACT`. The server compacts automatically once the log is at least 1 MiB **and** has doubled since it was last loaded or compacted. Try `--compact-min 1000` to watch it happen on small data.

## The hard part: not stopping the server

The simple approach is to loop over the map, write the file, and swap it. While that loop runs, the single-threaded event loop answers nobody. With a few gigabytes of data, every client freezes for seconds.

We copy Redis instead and let the operating system make the copy:

```text
           fork()
server ─────┬───────────────────────────────────────────────▶ keeps serving clients
            │   new changes → old log  AND  rewrite buffer
            │                                                  ▲
            └─ child: frozen copy of the store                │ pipe closes / status byte
               writes data.aof.rewrite, fsyncs, exits ────────┘
                                                               then parent:
                                                               1. append rewrite buffer
                                                               2. fsync
                                                               3. rename over data.aof
                                                               4. fsync the directory
```

**1. `fork()` makes a snapshot for free.** The child process starts as an exact copy of the server's memory, frozen at that instant. The parent continues changing *its* map; the child never sees those changes. The OS doesn't copy all the memory up front. Parent and child share pages until one of them writes to a page, and only that page is copied (**copy-on-write**). A busy server can still end up copying a lot, which is why Redis warns about memory use during rewrites.

**2. The parent remembers what the snapshot missed.** Any change made after `fork()` is written to the old log as usual (still safe if we crash) *and* copied into `rewrite_buffer_`. When the child finishes, the parent appends the buffer to the new file. Snapshot + buffer = current state.

**3. `rename()` makes the swap atomic.** Replacing a file by name is a single step: anyone opening `data.aof` gets either the complete old log or the complete new one, never a mix. Before the rename we `fsync` the new file, so its *contents* are on disk. After it we `fsync` the **directory**, because a file's name lives in its directory, so the rename itself also has to reach the disk.

**4. The event loop learns when the child is done.** The child gets the write end of a pipe and the parent `poll()`s the read end alongside its sockets. The child writes one status byte and exits. If it crashes, the pipe simply closes. Either way `poll()` wakes up the parent, which then calls `waitpid()` to collect the child's exit status. The same readiness idea from Lesson 3 now covers processes too.

## What can go wrong, and why it's safe

| Failure | Result |
| --- | --- |
| Child fails or is killed | Temp file deleted; old log untouched; the buffer is discarded (its changes are already in the old log). |
| Server crashes during compaction | Old log is complete. The stray `data.aof.rewrite` is deleted at the next start. |
| Crash after `rename()` | The new log already holds snapshot + buffer, fsynced. |
| `fsync` of the directory fails | We can't know whether the rename will survive a power cut: the server stops, as it does for other failed fsyncs. |
| Compaction keeps failing (e.g. disk full) | The automatic trigger waits for the log to double again instead of retrying every loop turn. |

## Two details that bit us

**The child must close the sockets it inherited.** `fork()` copies every open file descriptor, including client connections. Suppose a client sends `EXIT` and the parent closes the socket. The connection doesn't actually end while the child still holds a copy, so the client waits for the compaction to finish before it sees EOF. The server passes the child its list of sockets to close. The test suite catches this: removing that loop makes it fail.

**The child calls `_exit()`, not `exit()` or `return`.** Those would run destructors and flush `std::cout` buffers that are copies of the parent's. The child would then fsync and close the parent's log, and could print the parent's pending output a second time. `_exit()` ends the process immediately.

## Code and lessons side by side

| Code | What it means |
| --- | --- |
| `KeyValueStore::for_each` | Visit every entry through a lambda without exposing the map. |
| `should_compact` | Automatic trigger: size ≥ `--compact-min` and ≥ twice `base_size_`. |
| `start_compaction` | Open the temp file, create a pipe, `fork()`. The child writes and exits; the parent returns right away. |
| `pid == 0` | `fork()` returns 0 in the child and the child's process ID in the parent: the same code goes down two paths. |
| `rewrite_buffer_` | Records appended while the child is running. |
| `compaction_fd` | The pipe the event loop watches; `-1` when nothing is running. |
| `finish_compaction` | `waitpid`, append the buffer, `fsync`, `rename`, `fsync` the directory, switch to the new file. |
| `std::function<std::string()> compact` | One lambda used by both the `COMPACT` command and the automatic trigger, so both close the same sockets. |
| `KV_TEST_COMPACT_DELAY_MS` | Test hook: makes the child sleep, so tests can reliably send commands *during* a compaction. |

## Trade-offs we accepted

- **The rewrite buffer has no limit.** A burst of writes during a slow compaction grows it in memory. Redis 7 avoids this by switching new writes to a fresh *incremental* file instead (its "multi-part AOF").
- **Memory can double in the worst case**, if the parent changes every page while the child is still writing.
- **`fork()` itself pauses briefly**, to copy the page tables, longer for big processes. Fine here; noticeable at tens of gigabytes.
- **The parent's final step is synchronous.** Appending the buffer and fsyncing happen in the event loop. That is fast because the buffer is small compared with the snapshot.

## Checks included

```sh
python3 tests/test_compaction.py ./kvserver_multi
```

The tests check that history collapses to one `SET` per live key, deleted keys vanish, and appends continue in the new file. They make the child slow and verify the server keeps answering within 0.5 s, refuses a second `COMPACT`, closes a client's connection promptly, and carries over changes made during the rewrite. They also crash the server mid-compaction and check that the old log survives and the stray temp file is removed. Finally, they check that a constantly overwritten key keeps the log small through automatic compaction. Earlier suites still pass, all with AddressSanitizer and UndefinedBehaviorSanitizer too.

## Explain these before moving on

1. After `fork()`, the parent sets `counter` to 7. Why doesn't the child's snapshot contain 7, and how does 7 still end up in the new log?
2. Why must the new file be fsynced *before* the `rename()`, and the directory *after*?
3. What would a client notice if the child forgot to close inherited sockets?
4. Why is it harmless to throw away the rewrite buffer when the child fails?

Next directions: **key expiration** (`SET ... EX seconds`, a sorted structure of deadlines, and a `poll()` timeout, much like `everysec`), or a **benchmark** to measure what `--fsync always` really costs.

References: [Redis persistence](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/), [POSIX fork](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fork.html), [POSIX rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html).
