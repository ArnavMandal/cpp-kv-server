// Lesson 6: a load generator for kvserver_multi.
//
// Each client thread owns one connection and runs a closed loop: send one
// command, wait for its reply, record how long that took, repeat. Throughput
// is total measured operations divided by the wall time of the measured phase.
//
// The workload is deterministic: the same --seed produces the same sequence of
// commands on every machine, so two runs differ only in how fast they ran.
// Prints one JSON object on stdout; the driver (bench/run_bench.py) collects it.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Options {
    int port = 6379;
    int clients = 4;
    long ops = 50000;       // Measured operations, split across all clients.
    long warmup = 2000;     // Unmeasured operations before the timer starts.
    long keys = 10000;      // Size of the key space; all keys are preloaded.
    int value_size = 64;
    int get_percent = 50;   // The rest are SETs.
    std::uint64_t seed = 1;
};

[[noreturn]] void die(const std::string& message) {
    std::cerr << "kvbench: " << message << '\n';
    std::exit(1);
}

template <typename T>
T parse_number(std::string_view text, const char* flag) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        die(std::string("invalid value for ") + flag + ": " + std::string(text));
    }
    return value;
}

Options parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (i + 1 >= argc) { die("missing value for " + std::string(flag)); }
        const std::string_view value(argv[++i]);
        if (flag == "--port") { o.port = parse_number<int>(value, "--port"); }
        else if (flag == "--clients") { o.clients = parse_number<int>(value, "--clients"); }
        else if (flag == "--ops") { o.ops = parse_number<long>(value, "--ops"); }
        else if (flag == "--warmup") { o.warmup = parse_number<long>(value, "--warmup"); }
        else if (flag == "--keys") { o.keys = parse_number<long>(value, "--keys"); }
        else if (flag == "--value-size") { o.value_size = parse_number<int>(value, "--value-size"); }
        else if (flag == "--get-percent") { o.get_percent = parse_number<int>(value, "--get-percent"); }
        else if (flag == "--seed") { o.seed = parse_number<std::uint64_t>(value, "--seed"); }
        else { die("unknown option " + std::string(flag)); }
    }
    if (o.port < 1 || o.port > 65535 || o.clients < 1 || o.clients > 100 || o.ops < 1
        || o.warmup < 0 || o.keys < 1 || o.value_size < 0 || o.value_size > 3000
        || o.get_percent < 0 || o.get_percent > 100) {
        die("option out of range (clients 1..100, value-size 0..3000, get-percent 0..100)");
    }
    return o;
}

// A blocking connection that reads replies one line at a time.
class Connection {
public:
    explicit Connection(int port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) { die("socket failed"); }
        // Send each small command immediately instead of waiting to batch it.
        const int on = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<unsigned short>(port));
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            die("connect to 127.0.0.1:" + std::to_string(port) + " failed");
        }
    }
    ~Connection() { ::close(fd_); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void send_all(std::string_view bytes) {
        while (!bytes.empty()) {
            const auto count = ::send(fd_, bytes.data(), bytes.size(), 0);
            if (count < 0) {
                if (errno == EINTR) { continue; }
                die("send failed");
            }
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
    }

    // Returns the next reply without its '\n'. Valid until the next call.
    std::string_view read_line() {
        if (start_ != 0 && start_ == buffer_.size()) { buffer_.clear(); start_ = 0; }
        while (true) {
            const auto newline = buffer_.find('\n', start_);
            if (newline != std::string::npos) {
                const std::string_view line(buffer_.data() + start_, newline - start_);
                start_ = newline + 1;
                return line;
            }
            char chunk[16 * 1024];
            const auto count = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (count < 0 && errno == EINTR) { continue; }
            if (count <= 0) { die("server closed the connection"); }
            if (start_ != 0) { buffer_.erase(0, start_); start_ = 0; }
            buffer_.append(chunk, static_cast<std::size_t>(count));
        }
    }

private:
    int fd_ = -1;
    std::string buffer_;
    std::size_t start_ = 0;
};

std::string key_name(std::uint64_t index) { return "key:" + std::to_string(index); }

// Generates the same command sequence for the same (seed, client) everywhere.
// std::mt19937_64's output is fixed by the C++ standard; distributions are
// not, so we use the raw numbers ourselves instead of std::uniform_int_distribution.
class Workload {
public:
    Workload(const Options& o, int client)
        : rng_(o.seed * 1000003 + static_cast<std::uint64_t>(client)), options_(o),
          value_(static_cast<std::size_t>(o.value_size), 'v') {}

    // Fills command; returns true for GET, false for SET.
    bool next(std::string& command) {
        const std::uint64_t key = rng_() % static_cast<std::uint64_t>(options_.keys);
        const bool get = static_cast<int>(rng_() % 100) < options_.get_percent;
        command = get ? "GET " : "SET ";
        command += key_name(key);
        if (!get) { command += ' '; command += value_; }
        command += '\n';
        return get;
    }

private:
    std::mt19937_64 rng_;
    const Options& options_;
    std::string value_;
};

struct ClientResult {
    std::vector<std::uint32_t> get_ns; // Latency of each measured GET.
    std::vector<std::uint32_t> set_ns;
    long errors = 0;
};

// Release all clients at once, so the timer measures them running together.
class StartGate {
public:
    explicit StartGate(int parties) : waiting_(parties) {}
    void arrive_and_wait() {
        std::unique_lock lock(mutex_);
        if (--waiting_ == 0) { start_ = Clock::now(); open_.notify_all(); return; }
        open_.wait(lock, [&] { return waiting_ == 0; });
    }
    Clock::time_point start() const { return start_; }
private:
    std::mutex mutex_;
    std::condition_variable open_;
    int waiting_;
    Clock::time_point start_;
};

// Write every key once, pipelined in batches, so GETs always find a value.
void preload(const Options& o) {
    Connection connection(o.port);
    const std::string value(static_cast<std::size_t>(o.value_size), 'v');
    constexpr long batch = 256;
    for (long first = 0; first < o.keys; first += batch) {
        const long last = std::min(o.keys, first + batch);
        std::string commands;
        for (long k = first; k < last; ++k) {
            commands += "SET " + key_name(static_cast<std::uint64_t>(k)) + ' ' + value + '\n';
        }
        connection.send_all(commands);
        for (long k = first; k < last; ++k) {
            if (connection.read_line() != "OK") { die("preload SET was not acknowledged"); }
        }
    }
}

void run_client(const Options& o, int id, long warmup, long measured, StartGate& gate,
                ClientResult& result) {
    Connection connection(o.port);
    Workload workload(o, id);
    std::string command;
    const std::string expected_value = "VALUE " + std::string(static_cast<std::size_t>(o.value_size), 'v');
    // Correctness still matters in a benchmark: a fast wrong answer is a failure.
    auto check = [&](bool get, std::string_view reply) {
        if (get ? reply != expected_value : reply != "OK") { ++result.errors; }
    };
    for (long i = 0; i < warmup; ++i) {
        const bool get = workload.next(command);
        connection.send_all(command);
        check(get, connection.read_line());
    }
    result.get_ns.reserve(static_cast<std::size_t>(measured));
    result.set_ns.reserve(static_cast<std::size_t>(measured));
    gate.arrive_and_wait();
    for (long i = 0; i < measured; ++i) {
        const bool get = workload.next(command);
        const auto sent = Clock::now();
        connection.send_all(command);
        const auto reply = connection.read_line();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - sent).count();
        // uint32 nanoseconds holds up to ~4.3 s; clamp anything slower.
        const auto ns = static_cast<std::uint32_t>(std::min<long long>(elapsed, UINT32_MAX));
        (get ? result.get_ns : result.set_ns).push_back(ns);
        check(get, reply);
    }
}

// Nearest-rank percentile of sorted samples, in microseconds.
double percentile_us(const std::vector<std::uint32_t>& sorted, double p) {
    if (sorted.empty()) { return 0; }
    const auto rank = static_cast<std::size_t>(p / 100.0 * static_cast<double>(sorted.size()));
    return sorted[std::min(rank, sorted.size() - 1)] / 1000.0;
}

void print_latency(const char* name, std::vector<std::uint32_t>& samples) {
    std::sort(samples.begin(), samples.end());
    double total = 0;
    for (const auto s : samples) { total += s; }
    const double mean = samples.empty() ? 0 : total / static_cast<double>(samples.size()) / 1000.0;
    std::printf("\"%s\":{\"count\":%zu,\"mean_us\":%.2f,\"p50_us\":%.2f,\"p90_us\":%.2f,"
                "\"p99_us\":%.2f,\"p999_us\":%.2f,\"max_us\":%.2f}",
                name, samples.size(), mean, percentile_us(samples, 50), percentile_us(samples, 90),
                percentile_us(samples, 99), percentile_us(samples, 99.9),
                samples.empty() ? 0.0 : samples.back() / 1000.0);
}
} // namespace

int main(int argc, char** argv) {
    const Options o = parse_options(argc, argv);
    preload(o);

    std::vector<ClientResult> results(static_cast<std::size_t>(o.clients));
    StartGate gate(o.clients);
    std::vector<std::thread> threads;
    for (int id = 0; id < o.clients; ++id) {
        // Split operations evenly; the first clients take any remainder.
        const long share = o.ops / o.clients + (id < o.ops % o.clients ? 1 : 0);
        const long warm = o.warmup / o.clients + (id < o.warmup % o.clients ? 1 : 0);
        threads.emplace_back(run_client, std::cref(o), id, warm, share, std::ref(gate),
                             std::ref(results[static_cast<std::size_t>(id)]));
    }
    for (auto& thread : threads) { thread.join(); }
    const double seconds = std::chrono::duration<double>(Clock::now() - gate.start()).count();

    std::vector<std::uint32_t> gets, sets;
    long errors = 0;
    for (auto& r : results) {
        gets.insert(gets.end(), r.get_ns.begin(), r.get_ns.end());
        sets.insert(sets.end(), r.set_ns.begin(), r.set_ns.end());
        errors += r.errors;
    }
    std::vector<std::uint32_t> all = gets;
    all.insert(all.end(), sets.begin(), sets.end());

    std::printf("{\"ops\":%ld,\"seconds\":%.6f,\"ops_per_sec\":%.1f,\"errors\":%ld,",
                o.ops, seconds, static_cast<double>(o.ops) / seconds, errors);
    print_latency("all", all);
    std::printf(",");
    print_latency("get", gets);
    std::printf(",");
    print_latency("set", sets);
    std::printf("}\n");
    return errors == 0 ? 0 : 2;
}
