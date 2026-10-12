# Lesson 1: local storage

## Run it

Open a terminal in this folder, then:

```sh
clang++ -std=c++20 -Wall -Wextra -Wpedantic -g main.cpp -o kvstore
./kvstore
```

The compiler turns the source into an executable. `-std=c++20` selects our project's C++ version, the warning flags catch suspicious code, and `-g` includes debugging information. On Linux, you can use `g++` instead of `clang++`.

On some macOS Command Line Tools installations, the compiler needed the installed SDK's C++ header path explicitly. Use this build command if you see `'iostream' file not found`:

```sh
clang++ -std=c++20 -Wall -Wextra -Wpedantic -g \
  -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1 \
  main.cpp -o kvstore
./kvstore
```

CMake is optional for this milestone. If installed:

```sh
cmake -S . -B build
cmake --build build
./build/kvstore
```

## Code and lessons side by side

Read `kv_store.hpp`, then `main.cpp`. Their three parts match these lessons:

| Code | What it teaches |
| --- | --- |
| `KeyValueStore` | An object owns the map and exposes only the operations other code needs. |
| `std::unordered_map<std::string, std::string>` | A hash table associates a key with a value. Lookup is O(1) on average, O(n) in the worst case. |
| `const std::string& key` | Borrow a string without copying it; this function cannot change it through that reference. |
| `data_[key] = value` | Insert or replace an entry. |
| `find(key)` and `it->second` | Find an entry without inserting; read its value. |
| `bool get(..., std::string& value) const` | Return whether the key exists and put its value in the caller's string. The final `const` means the store stays unchanged. |
| `KeyValueStore& store` | Execute commands against the original store, so updates persist. |
| `input >> command >> key` | Read two whitespace-separated words. |
| `std::getline(input, value)` | Read the rest of the line, including spaces. |
| `KeyValueStore store;` before the loop | Keep one store alive across every command in the session. |

The flow is **keyboard → line → command function → map → response**. Later, TCP will supply the lines to the same command function.

## Try it

```text
> SET user:42 Arnav Mandal
OK
> GET user:42
VALUE Arnav Mandal
> SET user:42 Updated Name
OK
> GET user:42
VALUE Updated Name
> DEL user:42
DELETED
> GET user:42
NOT_FOUND
> GET
ERROR expected COMMAND key
> GET user:42 extra
ERROR unexpected argument
> EXIT
```

## Command format

- Commands are uppercase. Keys are nonempty and contain no whitespace.
- Each line contains one command. Values cannot contain newlines.
- `SET key value` requires a literal space after the key. The rest is the value, with spaces preserved. `SET key ` stores an empty string; `SET key` is an error.
- The stream parser accepts leading whitespace and whitespace between the command and key. `GET` and `DEL` accept trailing whitespace but reject extra words.
- Missing keys return `NOT_FOUND`; deleting an existing key returns `DELETED`.
- `EXIT` by itself quits the terminal program. End of input also quits.

The local terminal program has no input size limit. The TCP server limits each command to 4,096 bytes before the newline and closes connections that exceed it.

## Your next learning exercise

1. Store a value containing spaces, replace it, then delete it twice. Predict each response before running it.
2. Explain why creating `store` inside the loop would lose data.
3. Explain why removing `&` from `execute_command` would stop updates from surviving between commands.

## Next milestones

1. **Done:** local storage and command parsing.
2. **Done:** one-client TCP server: sockets, partial reads, complete-line buffering, bounded input, partial writes, and connection cleanup.
3. **Current:** multiple clients with nonblocking sockets and a `poll()` event loop on macOS. Linux `epoll` is an optional later extension, not implemented yet.
4. Persistence: append operations to a file and recover after restart.
5. Expiration and eviction: time limits and a memory budget.
6. Benchmarks: measure throughput and latency, then explain one measured improvement.

We will add each layer after understanding the previous one. Next: persistence so values survive a server restart.
