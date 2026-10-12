#include "kv_store.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
constexpr std::size_t max_line = 4096;
constexpr std::size_t pause_read_at = 64 * 1024;
constexpr std::size_t max_pending = 1024 * 1024;
constexpr std::size_t max_clients = 128;

class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { if (fd_ >= 0) { ::close(fd_); } }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int get() const { return fd_; }
    int release() { const int fd = fd_; fd_ = -1; return fd; }
private:
    int fd_;
};

bool nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }

// Each connection remembers its own progress between event-loop iterations.
struct Client {
    explicit Client(int fd) : socket(fd) {}
    Socket socket;
    std::string line;       // Incomplete command, waiting for a newline.
    std::string output;     // Responses not yet completely sent.
    std::size_t sent = 0;  // How much of output has already been sent.
    bool closing = false; // Stop reading; flush responses, then disconnect.

    std::size_t pending() const { return output.size() - sent; }
};

bool queue_response(Client& client, std::string_view response) {
    if (response.size() > max_pending - client.pending()) { return false; }
    // Remove an already-sent prefix before appending, bounding stored bytes.
    if (client.sent != 0) {
        client.output.erase(0, client.sent);
        client.sent = 0;
    }
    client.output.append(response);
    return true;
}

// Do only one small read per turn. A busy client must let others take turns.
bool read_ready(Client& client, KeyValueStore& store) {
    char bytes[1024];
    const auto count = ::recv(client.socket.get(), bytes, sizeof(bytes), 0);
    if (count < 0) { return errno == EINTR || would_block(); }
    if (count == 0) {
        client.line.clear(); // EOF discards an unfinished command.
        client.closing = true; // But complete commands still get their replies.
        return true;
    }
    for (ssize_t i = 0; i < count; ++i) {
        if (bytes[i] == '\n') {
            if (!client.line.empty() && client.line.back() == '\r') {
                client.line.pop_back();
            }
            if (client.line == "EXIT") {
                client.closing = true;
                client.line.clear();
                return queue_response(client, "BYE\n");
            }
            if (!queue_response(client, execute_command(store, client.line) + '\n')) {
                return false; // Hard output limit: disconnect this client only.
            }
            client.line.clear();
        } else {
            if (client.line.size() == max_line) {
                client.closing = true;
                client.line.clear();
                return queue_response(client, "ERROR command too long\n");
            }
            client.line.push_back(bytes[i]);
        }
    }
    return true;
}

bool write_ready(Client& client) {
    if (client.pending() == 0) { return true; }
    const auto count = ::send(client.socket.get(),
                              client.output.data() + client.sent,
                              std::min(client.pending(), pause_read_at), 0);
    if (count < 0) { return errno == EINTR || would_block(); }
    if (count == 0) { return false; }
    client.sent += static_cast<std::size_t>(count);
    if (client.pending() == 0) {
        client.output.clear();
        client.sent = 0;
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    // An optional port lets tests request port 0 (an OS-selected free port).
    int port = 6379;
    if (argc > 2) { std::cerr << "Usage: kvserver_multi [port]\n"; return 1; }
    if (argc == 2) {
        const std::string_view text(argv[1]);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
            || port < 0 || port > 65535) {
            std::cerr << "Port must be an integer from 0 to 65535\n"; return 1;
        }
    }

    std::signal(SIGPIPE, SIG_IGN);
    Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
    if (listener.get() < 0) { std::perror("socket"); return 1; }
    const int reuse = 1;
    if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                     &reuse, sizeof(reuse)) < 0 || !nonblocking(listener.get())) {
        std::perror("configure listener"); return 1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0
        || ::listen(listener.get(), 128) < 0) {
        std::perror("bind/listen"); return 1;
    }
    socklen_t address_size = sizeof(address);
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &address_size) < 0) {
        std::perror("getsockname"); return 1;
    }
    std::cout << "Listening on 127.0.0.1:" << ntohs(address.sin_port)
              << " (multiple clients, one thread, poll)\n" << std::flush;

    KeyValueStore store; // Shared by all clients; only this thread accesses it.
    std::unordered_map<int, Client> clients;
    while (true) {
        // Rebuild a snapshot each turn. Only request writes when output exists.
        std::vector<pollfd> watched{{listener.get(), POLLIN, 0}};
        for (const auto& [fd, client] : clients) {
            short events = 0;
            if (!client.closing && client.pending() < pause_read_at) { events |= POLLIN; }
            if (client.pending() != 0) { events |= POLLOUT; }
            watched.push_back({fd, events, 0});
        }
        // Sleep until ANY watched socket is ready; no busy waiting.
        if (::poll(watched.data(), watched.size(), -1) < 0) {
            if (errno == EINTR) { continue; }
            std::perror("poll"); return 1;
        }
        if (watched[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "Listening socket failed\n"; return 1;
        }
        if (watched[0].revents & POLLIN) {
            // Bound accepts per turn, so connection bursts cannot hog the loop.
            for (int n = 0; n < 16; ++n) {
                Socket accepted(::accept(listener.get(), nullptr, nullptr));
                if (accepted.get() < 0) {
                    if (errno == EINTR) { continue; }
                    if (would_block()) { break; }
                    std::perror("accept"); return 1;
                }
                if (clients.size() >= max_clients) { continue; } // RAII closes it.
                if (!nonblocking(accepted.get())) { std::perror("fcntl client"); continue; }
                const int fd = accepted.get();
                clients.try_emplace(fd, accepted.release());
            }
        }
        for (std::size_t i = 1; i < watched.size(); ++i) {
            const auto& event = watched[i];
            auto it = clients.find(event.fd);
            auto& client = it->second;
            bool alive = !(event.revents & (POLLERR | POLLNVAL));
            // A hangup can coexist with unread bytes: receive before closing.
            if (alive && !client.closing && client.pending() < pause_read_at
                && (event.revents & (POLLIN | POLLHUP))) {
                alive = read_ready(client, store);
            }
            if (alive && (event.revents & (POLLOUT | POLLHUP))) {
                alive = write_ready(client);
            }
            // A fully hung-up peer cannot consume output that would block.
            if ((event.revents & POLLHUP) && client.pending() != 0) { alive = false; }
            if (!alive || (client.closing && client.pending() == 0)) {
                clients.erase(it); // Destruction closes exactly this socket.
            }
        }
    }
}
