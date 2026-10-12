#include "kv_store.hpp"
#include <iostream>

int main() {
    // Lesson 3: outside the loop means entries survive between commands.
    KeyValueStore store;
    std::string line;

    std::cout << "Commands: SET key value, GET key, DEL key\n";
    std::cout << "Type EXIT to quit.\n";
    while (std::cout << "> " && std::getline(std::cin, line)) {
        if (line == "EXIT") {
            break;
        }
        std::cout << execute_command(store, line) << '\n';
    }
    // Owned strings and the map are automatically destroyed here.
    return 0;
}
