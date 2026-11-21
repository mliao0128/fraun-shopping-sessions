#include "../shared.hpp"

#include <deque>
#include <iostream>
#include <random>

int main() {
    std::mt19937 random(713);
    std::deque<std::string> inputs;
    std::vector<Key> keys;

    for (size_t i = 0; i < 1200; ++i) {
        // Cross the compact-pointer length limit and exercise every short-key
        // width, both for ASCII and for arbitrary bytes (including zeros).
        const size_t length = i < 3 ? 16382 + i : i % 24;
        std::string& text = inputs.emplace_back(length, '\0');
        for (char& byte : text)
            byte = static_cast<char>(random() % (i % 2 ? 128 : 256));

        Key key = Key::from(text);
        Key::Buffer decoded;
        if (key.view(decoded) != text || key.empty() != text.empty()) return 1;
        std::string duplicate(text);
        Key other = Key::from(duplicate);
        if (!(key == other) || key.hash() != other.hash()) return 2;
        keys.push_back(key);
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        for (size_t j = 0; j < keys.size(); ++j) {
            if ((keys[i] == keys[j]) != (inputs[i] == inputs[j])) return 3;
            if ((keys[i] < keys[j]) != (inputs[i] < inputs[j])) return 4;
        }
    }
    std::cout << "All binary key comparisons matched.\n";
}
