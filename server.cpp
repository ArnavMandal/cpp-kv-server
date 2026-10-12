#include "kv_store.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string_view>

// A socket descriptor is an integer owned by this object. Leaving scope closes
// it, including on early returns. This resource ownership pattern is called RAII.
class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { if (fd_ >= 0) ::close(fd_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};

// send() can write only PART of a response. Keep sending the remaining bytes.
bool send_all(int fd, std::string_view response) {
    while (!response.empty()) {
        const auto sent = ::send(fd, response.data(), response.size(), 0);
        if (sent < 0 && errno == EINTR) { continue; }
        if (sent <= 0) { return false; }
        response.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

void serve_client(int fd, KeyValueStore& store) {
    constexpr std::size_t max_line_bytes = 4096; // Excludes newline; includes CR.
    char chunk[1024];
    std::string line;
    while (true) {
        const auto received = ::recv(fd, chunk, sizeof(chunk), 0);
        if (received < 0 && errno == EINTR) { continue; }
        if (received <= 0) {
            // Zero means the client ended its input. Discard an unfinished line.
            return;
        }

        // TCP is a byte stream: a read may contain half a line or several lines.
        // Keep the unfinished line across reads; execute only at a newline.
        for (ssize_t i = 0; i < received; ++i) {
            if (chunk[i] == '\n') {
                // Accept both LF and CRLF line endings.
                if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                if (line == "EXIT") {
                    send_all(fd, "BYE\n");
                    return; // Disconnect this client; keep the server alive.
                }
                const auto response = execute_command(store, line) + '\n';
                if (!send_all(fd, response)) { return; }
                line.clear();
            } else {
                if (line.size() == max_line_bytes) {
                    send_all(fd, "ERROR command too long\n");
                    return; // Close instead of interpreting a truncated command.
                }
                line.push_back(chunk[i]);
            }
        }
    }
}

int main() {
    // A disconnected peer must cause a send error, not terminate our process.
    std::signal(SIGPIPE, SIG_IGN);
    Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
    if (listener.get() < 0) { std::perror("socket"); return 1; }

    const int reuse = 1;
    if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                     &reuse, sizeof(reuse)) < 0) {
        std::perror("setsockopt"); return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(6379); // Convert to the network's byte order.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // This computer only.
    if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) < 0) {
        std::perror("bind"); return 1;
    }
    if (::listen(listener.get(), 8) < 0) {
        std::perror("listen"); return 1;
    }

    KeyValueStore store; // Data survives client disconnections, not restarts.
    std::cout << "Listening on 127.0.0.1:6379 (one client at a time)\n"
              << "Press Ctrl+C to stop the server.\n" << std::flush;
    while (true) {
        // accept() waits for a connection and creates a NEW connected socket.
        Socket client(::accept(listener.get(), nullptr, nullptr));
        if (client.get() < 0) {
            if (errno == EINTR) { continue; }
            std::perror("accept"); return 1;
        }
        serve_client(client.get(), store);
        // Client socket closes here, then we accept the next connection.
    }
}
