# Lesson 6: measuring what persistence costs

**Goal:** answer “how much slower is `--fsync always`?” with a number you can reproduce tomorrow, on another machine, or after changing the code.

The tests from Lessons 2–5 answer “is it correct?” A benchmark answers “how fast is it?” Benchmarks are easy to get wrong in ways that still produce convincing numbers. This lesson builds one that shows how much you can trust its output.

**Driver → fresh server + empty log → load generator → JSON result → repeat, interleaved → medians + spread**

## Build and run

Benchmarks must use optimized builds. Code compiled without `-O2` can be several times slower, and that changes which parts are expensive. Build both programs with `-O2`:

```sh
cd cpp-kv-server
clang++ -std=c++20 -O2 -g -Wall -Wextra -Wpedantic \
  -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1 \
  server_multi.cpp -o kvserver_multi
clang++ -std=c++20 -O2 -g -Wall -Wextra -Wpedantic \
  -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1 \
  bench/kvbench.cpp -o kvbench
python3 bench/run_bench.py --note "clang++ -O2"
```

On Linux, use `g++` or `clang++` without the `-isystem` line, and add `-pthread` when building `kvbench`. With CMake: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`.

It takes about a minute. Close other programs first, and on a laptop, plug in the power.

## Try it

The run prints one line per run, then a summary:

```text
mode            ops/s (median)   spread   GET p50   GET p99   SET p50   SET p99  SET p99.9
------------------------------------------------------------------------------------------
memory                  87,133    15.7%      43.5      74.3      44.4      75.1       98.0
aof-no                  67,957     6.8%      50.3      92.5      57.2     104.5      495.8
aof-everysec            59,704    34.0%      55.4     126.7      62.5     144.7      730.1
aof-always              29,910    13.1%      86.8     228.5     147.8     300.2      624.3
Latencies are microseconds, medians across repeats.
```

Measured on an Apple M2 (8 cores) running macOS 26.0.1, with the server built from commit `2d8879d` using Apple clang 17 `-O2`. The defaults were 4 clients, 200,000 operations per run, 50% GETs, 10,000 keys, 64-byte values, and 5 repeats. Other programs were running (load average about 5), which explains the large spread column. Expect different numbers on your machine; compare *ratios*, not absolute values.

Every result is saved to `bench/results/<time>.json`. To compare against an earlier result:

```sh
python3 bench/run_bench.py --compare bench/results/20261110-140512.json
```

## Code and lessons side by side

Read `bench/kvbench.cpp` from top to bottom, then `run_once` and `main` in `bench/run_bench.py`.

| Code | What it means |
| --- | --- |
| `run_client` | One thread, one connection. Send a command, wait for the reply, record the time. Repeat. |
| `Clock::now()` before `send_all`, after `read_line` | Latency is the time the client waits, including the network stack and the server's queue. |
| `std::mt19937_64` | A random number generator whose output the C++ standard defines exactly. The same `--seed` sends the same commands everywhere. |
| `rng_() % keys` instead of `std::uniform_int_distribution` | Distributions may differ between standard libraries, so different compilers could produce different workloads. Plain `%` is identical everywhere. |
| `preload` | Sets every key first, so every `GET` finds a value and does equal work. |
| `warmup` | Untimed commands first: connections, caches, and the CPU's clock speed settle before measuring. |
| `StartGate` | All clients start the measured phase together, so throughput is measured with all of them running. |
| `check` | Every reply is verified. A fast wrong answer is a failure, and `kvbench` exits nonzero. |
| `percentile_us` | Sort all samples; p99 is the value 99% of requests were faster than. |
| `MODES` in `run_bench.py` | The four persistence settings: no log, and the log with each fsync policy. |
| `--compact-min 2**62` | Turns off automatic compaction during runs. A `fork()` in the middle would add noise unrelated to fsync. |
| `for repeat ...: for mode ...:` | Interleaving: A B C D, A B C D, … rather than A A A A, B B B B, … |
| `environment()` | Records the git commit, binary hashes, OS, CPU, and compiler with the numbers. |

## The most important idea: make the noise visible

A single run of a benchmark is an anecdote. Your laptop is running dozens of other programs; any one of them can steal the CPU for a few milliseconds. Five things in this benchmark deal with that:

1. **Same work every time.** Fixed seed, fixed operation count, fixed key space, fresh server, empty log.
2. **Interleaved repeats.** If the machine slows down halfway (another program starts, the CPU gets hot), every mode is affected equally instead of whichever mode happened to run last.
3. **Median, not mean, across repeats.** One unlucky run cannot move a median much.
4. **Spread is printed.** `(max − min) / median` shows how far apart the runs were. The program warns above 10%.
5. **The environment is saved.** In a month, you can tell whether a difference came from the code or from a different compiler or machine.

Here is what that looked like in practice. The same benchmark was run four times on the busy laptop above. Median throughput in thousands of operations per second:

| Session | memory | aof-no | aof-everysec | aof-always |
| --- | ---: | ---: | ---: | ---: |
| 1 (50k ops per run) | 84.8 | 69.8 | 68.5 | 30.9 |
| 2 (50k ops per run) | 76.1 | 58.3 | 39.4 | 30.2 |
| 3 (200k ops per run) | 87.1 | 68.0 | 59.7 | 29.9 |
| 4 (200k ops per run) | 86.0 | 62.1 | 58.0 | 30.9 |

Session 2 had runs at half speed; its spread column said 45–96%, so you would know not to trust it. Longer runs (sessions 3–4) were steadier. `aof-always` was the most consistent: about 30k in every session. On this machine, a difference under about 10% between two sessions is noise. Treat a difference as real only if it is larger than the spread and appears again when you rerun the benchmark.

## Reading the results

**Throughput and latency are two views of the same thing.** Each client waits for one reply before sending its next command. So with 4 clients and an average wait of 45.9 µs, the most you can do is 4 ÷ 0.0000459 s ≈ 87,000 per second, which matches the memory row. This is **Little's law**: requests in flight = throughput × latency. It's a quick way to check that a benchmark measures what you think.

**`aof-no` costs about 20%.** Each `SET` now makes a `write()` system call. The data only reaches the operating system's memory, not the disk, so the cost is the trip into the kernel.

**`aof-always` costs about 3× on `SET`s.** SET p50 rises from 44 µs to about 148 µs, so each `fsync()` takes roughly 100 µs here.

**`GET`s got slower too, though they never touch the disk.** GET p50 went from 44 µs to 87 µs. The server has one thread. While it waits inside `fsync()` for one client's `SET`, every other client's command waits in line. This is **head-of-line blocking**, and it's the price of the simple single-threaded design from Lesson 3.

**`everysec` looks like `aof-no` at the median, but not in the tail.** Once per second the event loop runs one `fsync()` covering a whole second of writes, and every client waits for it. That appears in p99.9 and max, not in p50. Always look at the tail: users who do many requests hit it often.

**The worst single request (`max_us` in the JSON) was 7–17 ms with any log, versus 0.3 ms without.** The filesystem occasionally stalls. Only a benchmark that records every sample can see this.

## A macOS warning: `fsync` does not mean on the platter

On macOS, `fsync()` hands data to the drive but does not ask the drive to empty its own cache. Apple's `man 2 fsync` says to use `fcntl(fd, F_FULLFSYNC)` for that, and it is much slower. So `--fsync always` on a Mac survives our program crashing and a kernel panic, but not necessarily a power cut. On Linux, `fsync()` flushes the drive cache too, so `aof-always` is usually slower there. That is another reason not to compare numbers across machines.

## What changed in the project

- `bench/kvbench.cpp`: the load generator (`kvbench`). Prints one JSON object per run.
- `bench/run_bench.py`: the driver. Runs every mode, repeats, summarizes, saves, and compares.
- `CMakeLists.txt`: builds `kvbench` and adds a `bench_smoke` CTest.
- `.gitignore`: `kvbench` and `bench/results/`.
- No server code changed. The benchmark measures Lesson 5's server as it is.

## Current limits

- **Client and server share one machine,** so they compete for CPU cores. Loopback TCP is also much faster than a real network.
- **One request in flight per client.** Real Redis clients often *pipeline*, sending many commands before reading replies. That gives far higher throughput. This benchmark measures the request-by-request case.
- **The workload is uniform:** every key is equally likely. Real traffic usually has a few very popular keys.
- **macOS cannot pin threads to cores,** so the scheduler may move the server between fast and efficiency cores mid-run. That is part of the spread.

## Checks included

```sh
python3 bench/run_bench.py --quick
```

`--quick` runs every mode once with a small workload. It checks that everything works; its numbers mean nothing. CTest runs it as `bench_smoke`. Every benchmark run also checks every reply, and fails if any reply is wrong.

## Your next learning exercise

1. Predict, then measure: run `--get-percent 90` and `--get-percent 10`. Which mode changes most, and why?
2. Run `--clients 1` and `--clients 16`. Use Little's law to explain what happens to throughput and latency in `memory` mode. Does `aof-always` behave the same way?
3. Run with `--dir` pointing at a USB drive or a network folder. Which rows change?
4. Explain why the driver starts a new server for every run instead of reusing one.

Next direction: **group commit**. With `--fsync always`, the server currently runs one `fsync()` per `SET`. If several clients' `SET`s arrive in the same event-loop turn, one `fsync()` could cover all of them. That only works if every reply waits until the `fsync()` finishes. Implement it, then use `--compare` to show the improvement and whether it is larger than the spread.

References: [Redis benchmark guide](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/benchmarks/), [Little's law](https://en.wikipedia.org/wiki/Little%27s_law), Apple `man 2 fsync` and `man 2 fcntl` (`F_FULLFSYNC`).
