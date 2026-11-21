#include "../shared.hpp"
#include <deque>
#include <iostream>
#include <random>
int main() {
    std::deque<std::string> text;
    DimensionIndex d;
    Index simple;
    std::mt19937 random(10053);
    auto add = [&](std::string s) {
        text.push_back(std::move(s));
        Key k = Key::from(text.back());
        size_t i = text.size();
        if (d.insert(k, i) != simple.insert(k, i))
            std::exit(1);
    };
    for (unsigned i = 0; i <= 50000; ++i)
        add("p" + std::to_string(i));
    for (unsigned i = 0; i <= 2000; ++i)
        add("c_" + std::to_string(i));
    for (auto s : {"", "00", "p00", "p0000", "c_0001", "xxxxxxxxxxx", "12345678", "123456789"})
        add(s);
    for (unsigned i = 0; i < 16; ++i)
        add(std::string(2, '\0') + char(255 - i));
    auto check = [&](const DimensionIndex &x, const std::string &s) {
        auto padded = s + "12345678";
        auto wanted = simple.find(Key::from(s));
        for (unsigned int n : {0u, 8u})
            if (x.lookup(std::string_view(padded).substr(0, s.size()), padded.data() + s.size() + n) !=
                wanted)
                std::exit(2);
    };
    d.finish();
    DimensionIndex copy = d;
    for (auto &t : text) {
        check(d, t);
        check(copy, t);
    }
    for (unsigned i = 0; i < 200000; ++i) {
        std::string s =
            (i & 1) ? "p" + std::to_string(random() % 80000) : "c_" + std::to_string(random() % 3000);
        if ((i % 3) == 0)
            s[random() % s.size()] = random() & 255;
        check(d, s);
    }
    add("other");
    d.finish();
    for (auto &t : text)
        check(d, t);
    std::cout << "Dense, sparse, malformed, copied and rebuilt dictionaries matched Index::find\n";
}
