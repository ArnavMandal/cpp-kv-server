#pragma once

#include "kv_store.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>

// Lesson 4: every change is appended to a file before the client hears "OK".
// On startup, replaying the file rebuilds the in-memory store.
enum class FsyncPolicy {
    Always,      // fsync after every change: slowest, survives power loss.
    EverySecond, // fsync at most once per second: may lose ~1 s on power loss.
    Never,       // Let the OS decide when to flush to disk.
};

class AppendLog {
public:
    using Clock = std::chrono::steady_clock;

    AppendLog() = default;
    AppendLog(const AppendLog&) = delete;
    AppendLog& operator=(const AppendLog&) = delete;
    ~AppendLog() {
        if (fd_ >= 0) {
            if (dirty_) { ::fsync(fd_); }
            ::close(fd_);
        }
    }

    // Open (or create) the log and replay it into store.
    // A final record without '\n' was torn by a crash: it is dropped and cut off.
    // Any other unreadable record means corruption, and we refuse to start.
    bool open(const std::string& path, FsyncPolicy policy, KeyValueStore& store,
              std::string& message) {
        policy_ = policy;
        // O_APPEND: every write goes to the current end of the file.
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd_ < 0) { return fail(message, "open " + path); }

        std::string contents;
        char buffer[64 * 1024];
        while (true) {
            const auto count = ::read(fd_, buffer, sizeof(buffer));
            if (count < 0) {
                if (errno == EINTR) { continue; }
                return fail(message, "read " + path);
            }
            if (count == 0) { break; }
            contents.append(buffer, static_cast<std::size_t>(count));
        }

        std::size_t records = 0;
        std::size_t start = 0;
        while (true) {
            const auto newline = contents.find('\n', start);
            if (newline == std::string::npos) { break; }
            // Do NOT strip '\r' here: the log is always LF, so '\r' is data.
            const std::string line = contents.substr(start, newline - start);
            std::string error;
            const auto command = parse_command(line, error);
            if (!command || command->kind == Command::Kind::Get) {
                message = path + ": corrupt record " + std::to_string(records + 1)
                        + " at byte " + std::to_string(start) + "; refusing to start";
                return false;
            }
            apply_command(store, *command);
            ++records;
            start = newline + 1;
        }

        size_ = start; // Everything up to here is complete, valid records.
        message = "Loaded " + std::to_string(records) + " records from " + path;
        if (start != contents.size()) {
            // Cut the torn tail off, or the next append would be glued onto it.
            if (::ftruncate(fd_, static_cast<off_t>(size_)) < 0 || ::fsync(fd_) < 0) {
                return fail(message, "truncate torn record in " + path);
            }
            message += " (dropped " + std::to_string(contents.size() - start)
                     + " bytes of an incomplete final record)";
        }
        return true;
    }

    // Write one record. Returns false if it could not be stored; the file is
    // then restored to its previous length so it never holds half a record.
    bool append(std::string_view record) {
        if (broken_) { return false; }
        std::size_t written = 0;
        while (written < record.size()) {
            const auto count = ::write(fd_, record.data() + written, record.size() - written);
            if (count < 0) {
                if (errno == EINTR) { continue; }
                return roll_back();
            }
            written += static_cast<std::size_t>(count);
        }
        if (policy_ == FsyncPolicy::Always && ::fsync(fd_) < 0) { return roll_back(); }
        size_ += record.size();
        if (policy_ == FsyncPolicy::EverySecond && !dirty_) {
            dirty_ = true;
            sync_due_ = Clock::now() + std::chrono::seconds(1);
        }
        return true;
    }

    // Milliseconds poll() may sleep before the next fsync is due; -1 = forever.
    int poll_timeout_ms() const {
        if (!dirty_) { return -1; }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            sync_due_ - Clock::now()).count();
        return left > 0 ? static_cast<int>(left) : 0;
    }

    // Called every event-loop turn. Returns false if fsync failed.
    bool sync_if_due() {
        if (!dirty_ || Clock::now() < sync_due_) { return true; }
        dirty_ = false;
        return ::fsync(fd_) == 0;
    }

private:
    static bool fail(std::string& message, const std::string& what) {
        message = what + ": " + std::strerror(errno);
        return false;
    }

    bool roll_back() {
        if (::ftruncate(fd_, static_cast<off_t>(size_)) < 0) {
            broken_ = true; // Unknown file state: refuse all further writes.
        }
        return false;
    }

    int fd_ = -1;
    FsyncPolicy policy_ = FsyncPolicy::EverySecond;
    std::size_t size_ = 0;   // Bytes of complete records in the file.
    bool dirty_ = false;     // Written but not yet fsynced (EverySecond only).
    bool broken_ = false;
    Clock::time_point sync_due_{};
};
