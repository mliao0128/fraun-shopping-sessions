#include "../shared.hpp"
#include <iostream>

template<size_t N>
bool checkShape() {
    std::array<std::string_view, N> reference, optimized;
    for (size_t length = 31; length <= 97; ++length) {
        for (size_t split = 0; split + N + 10 <= length; ++split) {
            for (std::string_view middle : {"x", "\"\"", "\"x\"", "\"", "x\"y", "\",x\"y", ",", "\"\"y"}) {
                std::string line(split, 'p');
                line += ',';
                line += middle;
                // Keep the proper number of columns for the paired-malformation case.
                size_t commas = std::count(line.begin(), line.end(), ',');
                while (commas++ < N - 1) line += ',';
                line.resize(length, 'f');
                bool expected = csvGeneric(line, reference);
                bool actual = csv(line, optimized);
                if (actual != expected || (expected && reference != optimized)) return false;
            }
        }
    }
    return true;
}

int main() {
    if (checkShape<4>() && checkShape<7>()) return 0;
    std::cerr << "Packed CSV parsing differed at a quote or chunk boundary\n";
    return 1;
}
