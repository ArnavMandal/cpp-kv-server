#pragma once

#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

// Lesson 1: this object owns the data. It knows nothing about terminals or TCP.
class KeyValueStore {
private:
    std::unordered_map<std::string, std::string> data_;

public:
    // const & borrows a string without copying it or allowing changes to it.
    void set(const std::string& key, const std::string& value) {
        data_[key] = value; // Insert a new entry or replace the existing value.
    }

    bool get(const std::string& key, std::string& value) const {
        const auto it = data_.find(key); // Unlike [], find does not insert.
        if (it == data_.end()) {
            return false;
        }
        value = it->second;
        return true; // An empty value is still a found value.
    }

    bool contains(const std::string& key) const {
        return data_.find(key) != data_.end();
    }

    bool remove(const std::string& key) {
        return data_.erase(key) > 0;
    }
};

// Lesson 4: parsing is separate from executing, so the server can decide
// what to do (for example, write to disk) between the two steps.
struct Command {
    enum class Kind { Get, Set, Del };
    Kind kind;
    std::string key;
    std::string value; // Only used by SET.
};

// Returns the command, or std::nullopt with an error message filled in.
inline std::optional<Command> parse_command(const std::string& line, std::string& error) {
    std::istringstream input(line);
    std::string name;
    Command command{Command::Kind::Get, "", ""};

    if (!(input >> name >> command.key)) {
        error = "ERROR expected COMMAND key";
        return std::nullopt;
    }

    if (name == "SET") {
        // Consume exactly one separating space; further spaces belong to value.
        if (input.get() != ' ') {
            error = "ERROR expected SET key value";
            return std::nullopt;
        }
        std::getline(input, command.value);
        command.kind = Command::Kind::Set;
        return command;
    }

    std::string extra;
    if (input >> extra) {
        error = "ERROR unexpected argument";
        return std::nullopt;
    }
    if (name == "GET") {
        command.kind = Command::Kind::Get;
        return command;
    }
    if (name == "DEL") {
        command.kind = Command::Kind::Del;
        return command;
    }
    error = "ERROR unknown command";
    return std::nullopt;
}

// True if running this command would change the stored data.
// GET never does; DEL of a missing key doesn't either.
inline bool changes_data(const KeyValueStore& store, const Command& command) {
    switch (command.kind) {
    case Command::Kind::Set: return true;
    case Command::Kind::Del: return store.contains(command.key);
    case Command::Kind::Get: return false;
    }
    return false;
}

// The exact line written to the log. Always LF, always one space separator,
// so replaying it reproduces the same key and value byte for byte.
inline std::string to_log_record(const Command& command) {
    if (command.kind == Command::Kind::Set) {
        return "SET " + command.key + ' ' + command.value + '\n';
    }
    return "DEL " + command.key + '\n';
}

// The reference means we change the original store, not a temporary copy.
inline std::string apply_command(KeyValueStore& store, const Command& command) {
    switch (command.kind) {
    case Command::Kind::Set:
        store.set(command.key, command.value);
        return "OK";
    case Command::Kind::Get: {
        std::string value;
        return store.get(command.key, value) ? "VALUE " + value : "NOT_FOUND";
    }
    case Command::Kind::Del:
        return store.remove(command.key) ? "DELETED" : "NOT_FOUND";
    }
    return "ERROR unknown command";
}

// Lesson 2: translate one line into an operation and a response.
inline std::string execute_command(KeyValueStore& store, const std::string& line) {
    std::string error;
    const auto command = parse_command(line, error);
    return command ? apply_command(store, *command) : error;
}
