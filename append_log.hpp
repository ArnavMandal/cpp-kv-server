#pragma once

#include "kv_store.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
        if (child_ > 0) { // Abandon an unfinished compaction.
            ::kill(child_, SIGKILL);
            ::waitpid(child_, nullptr, 0);
            ::close(pipe_fd_);
            ::unlink(temp_path_.c_str());
        }
        if (fd_ >= 0) {
            if (dirty_) { ::fsync(fd_); }
            ::close(fd_);
        }
    }

    // Open (or create) the log and replay it into store.
    // A final record without '\n' was torn by a crash: it is dropped and cut off.
    // Any other unreadable record means corruption, and we refuse to start.
    bool open(const std::string& path, FsyncPolicy policy, std::size_t compact_min,
              KeyValueStore& store, std::string& message) {
        path_ = path;
        temp_path_ = path + ".rewrite";
        policy_ = policy;
        compact_min_ = compact_min;
        // A leftover temp file is from a compaction that never finished.
        // The main log is still complete without it, so discard it.
        ::unlink(temp_path_.c_str());
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
        base_size_ = size_;
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
        if (!write_all(fd_, record)) { return roll_back(); }
        if (policy_ == FsyncPolicy::Always && ::fsync(fd_) < 0) { return roll_back(); }
        size_ += record.size();
        if (policy_ == FsyncPolicy::EverySecond && !dirty_) {
            dirty_ = true;
            sync_due_ = Clock::now() + std::chrono::seconds(1);
        }
        // Lesson 5: the child's snapshot can't see this change, so remember it.
        if (child_ > 0) { rewrite_buffer_.append(record); }
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

    // ----- Lesson 5: compaction -------------------------------------------

    // Automatic trigger: the log has at least doubled since it was last
    // loaded or compacted, and is big enough to be worth rewriting.
    bool should_compact() const {
        return child_ < 0 && !broken_ && size_ >= compact_min_ && size_ >= 2 * base_size_;
    }

    // While a compaction runs, the server watches this pipe. It becomes
    // readable when the child finishes (or dies). -1 means no compaction.
    int compaction_fd() const { return pipe_fd_; }

    // Fork a child that writes one SET per live key to a temporary file.
    // fork() gives the child a frozen copy of the whole store for free.
    // inherited_fds: sockets the child must close (it never uses them, and
    // holding them open would keep clients' connections from fully closing).
    bool start_compaction(const KeyValueStore& store, const std::vector<int>& inherited_fds,
                          std::string& message) {
        if (child_ > 0) { message = "compaction already running"; return false; }
        if (broken_) { message = "log is unwritable"; return false; }
        const int out = ::open(temp_path_.c_str(),
                               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (out < 0) { return give_up(message, "open " + temp_path_); }
        int done[2];
        if (::pipe(done) < 0) {
            ::close(out);
            return give_up(message, "pipe");
        }
        const pid_t pid = ::fork();
        if (pid < 0) {
            const int error = errno;
            ::close(out); ::close(done[0]); ::close(done[1]);
            errno = error;
            return give_up(message, "fork");
        }
        if (pid == 0) {
            // ---- Child process: a copy of the server, frozen at fork time. ----
            ::close(done[0]);
            ::close(fd_);
            for (const int fd : inherited_fds) { ::close(fd); }
            // Test hook: slow the child down so tests can write during compaction.
            if (const char* delay = std::getenv("KV_TEST_COMPACT_DELAY_MS")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(delay)));
            }
            const bool ok = write_snapshot(out, store) && ::fsync(out) == 0;
            const char status = ok ? 0 : 1;
            write_all(done[1], std::string_view(&status, 1));
            // _exit, not exit: skip destructors and stdio flushing, which
            // belong to the parent (they'd close its log and repeat its output).
            ::_exit(ok ? 0 : 1);
        }
        // ---- Parent: back to serving clients immediately. ----
        ::close(out);
        ::close(done[1]);
        child_ = pid;
        pipe_fd_ = done[0];
        rewrite_buffer_.clear();
        return true;
    }

    // Called when compaction_fd() is readable. Appends changes made during the
    // rewrite, then atomically replaces the old log with the new one.
    // Returns false only if the server can no longer trust its log (fatal).
    bool finish_compaction(std::string& message) {
        char status = 1;
        ssize_t count;
        do { count = ::read(pipe_fd_, &status, 1); } while (count < 0 && errno == EINTR);
        ::close(pipe_fd_);
        pipe_fd_ = -1;
        int wait_status = 0;
        while (::waitpid(child_, &wait_status, 0) < 0 && errno == EINTR) {}
        child_ = -1;
        std::string buffered;
        buffered.swap(rewrite_buffer_);

        if (count != 1 || status != 0 || !WIFEXITED(wait_status)
            || WEXITSTATUS(wait_status) != 0) {
            errno = EIO;
            give_up(message, "compaction child");
            return true;
        }
        const int fd = ::open(temp_path_.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd < 0 || !write_all(fd, buffered) || ::fsync(fd) < 0) {
            give_up(message, "finish " + temp_path_);
            if (fd >= 0) { ::close(fd); }
            return true;
        }
        const off_t new_size = ::lseek(fd, 0, SEEK_END);
        // rename() replaces the old name in one step: anyone opening the path
        // sees either the complete old log or the complete new one.
        if (new_size < 0 || ::rename(temp_path_.c_str(), path_.c_str()) < 0) {
            give_up(message, "rename " + temp_path_);
            ::close(fd);
            return true;
        }
        const std::size_t old_size = size_;
        ::close(fd_); // The old file's last name is gone; closing frees it.
        fd_ = fd;
        size_ = base_size_ = static_cast<std::size_t>(new_size);
        dirty_ = false; // The new file was fsynced above.

        // The rename itself lives in the directory: fsync it so it survives power loss.
        if (!sync_directory()) {
            fail(message, "fsync directory of " + path_);
            return false;
        }
        message = "Compacted " + path_ + ": " + std::to_string(old_size) + " -> "
                + std::to_string(size_) + " bytes";
        return true;
    }

private:
    static bool fail(std::string& message, const std::string& what) {
        message = what + ": " + std::strerror(errno);
        return false;
    }

    // A failed compaction is harmless: the old log is still complete. Wait for
    // the log to double again before the automatic trigger retries.
    bool give_up(std::string& message, const std::string& what) {
        fail(message, what);
        message += "; keeping the old log";
        ::unlink(temp_path_.c_str());
        base_size_ = size_;
        return false;
    }

    static bool write_all(int fd, std::string_view bytes) {
        std::size_t written = 0;
        while (written < bytes.size()) {
            const auto count = ::write(fd, bytes.data() + written, bytes.size() - written);
            if (count < 0) {
                if (errno == EINTR) { continue; }
                return false;
            }
            written += static_cast<std::size_t>(count);
        }
        return true;
    }

    // Write one SET per entry, in 64 KiB batches to keep system calls few.
    static bool write_snapshot(int fd, const KeyValueStore& store) {
        std::string batch;
        bool ok = true;
        store.for_each([&](const std::string& key, const std::string& value) {
            if (!ok) { return; }
            batch += to_log_record(Command{Command::Kind::Set, key, value});
            if (batch.size() >= 64 * 1024) {
                ok = write_all(fd, batch);
                batch.clear();
            }
        });
        return ok && write_all(fd, batch);
    }

    bool sync_directory() const {
        const auto slash = path_.rfind('/');
        const std::string directory = slash == std::string::npos ? "."
                                    : slash == 0 ? "/" : path_.substr(0, slash);
        const int fd = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) { return false; }
        const bool ok = ::fsync(fd) == 0;
        ::close(fd);
        return ok;
    }

    bool roll_back() {
        if (::ftruncate(fd_, static_cast<off_t>(size_)) < 0) {
            broken_ = true; // Unknown file state: refuse all further writes.
        }
        return false;
    }

    int fd_ = -1;
    std::string path_;
    std::string temp_path_;
    FsyncPolicy policy_ = FsyncPolicy::EverySecond;
    std::size_t size_ = 0;   // Bytes of complete records in the file.
    bool dirty_ = false;     // Written but not yet fsynced (EverySecond only).
    bool broken_ = false;
    Clock::time_point sync_due_{};

    // Lesson 5 state.
    std::size_t compact_min_ = 0;
    std::size_t base_size_ = 0;   // Log size right after the last load/compaction.
    pid_t child_ = -1;            // Running compaction child, or -1.
    int pipe_fd_ = -1;            // Read end of the child's "done" pipe.
    std::string rewrite_buffer_;  // Records appended since the child forked.
};
