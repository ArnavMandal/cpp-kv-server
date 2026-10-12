#pragma once

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

    bool remove(const std::string& key) {
        return data_.erase(key) > 0;
    }
};

// Lesson 2: translate one line into an operation and a response.
// The reference means we change the original store, not a temporary copy.
inline std::string execute_command(KeyValueStore& store, const std::string& line) {
    std::istringstream input(line);
    std::string command;
    std::string key;

    if (!(input >> command >> key)) {
        return "ERROR expected COMMAND key";
    }

    if (command == "SET") {
        std::string value;
        // Consume exactly one separating space; further spaces belong to value.
        if (input.get() != ' ') {
            return "ERROR expected SET key value";
        }
        std::getline(input, value);
        store.set(key, value);
        return "OK";
    }

    std::string extra;
    if (input >> extra) {
        return "ERROR unexpected argument";
    }

    if (command == "GET") {
        std::string value;
        return store.get(key, value) ? "VALUE " + value : "NOT_FOUND";
    }
    if (command == "DEL") {
        return store.remove(key) ? "DELETED" : "NOT_FOUND";
    }
    return "ERROR unknown command";
}
