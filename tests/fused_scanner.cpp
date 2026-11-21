#include "../ingest.cpp"
#include <fstream>
#include <iostream>
#include <random>
int main(int argc, char **argv) {
    std::string file = "header\n";
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i]);
        if (!in)
            return 2;
        std::string row;
        while (std::getline(in, row))
            file += row + '\n';
    }
    std::mt19937 rng(21931);
    for (unsigned n = 0; n < 203; ++n)
        for (int i = 0; i < 5; ++i) {
            for (unsigned k = 0; k < n; ++k)
                file += "abc,\" "[rng() % 6];
            file += '\n';
        }
    for (int terminal = 0; terminal < 2; ++terminal) {
        if (terminal)
            file.pop_back();
        std::vector<std::string_view> expected;
        size_t start = file.find('\n') + 1;
        while (start < file.size()) {
            size_t end = file.find('\n', start);
            if (end == file.npos)
                end = file.size();
            expected.emplace_back(file.data() + start, end - start);
            start = end + 1;
        }
        for (size_t span : {31, 127, 1567, 65536, 2097152, 16000000}) {
            size_t count = 0;
            for (size_t first = 0; first < file.size(); first += span) {
                PreparedMasks collected{};
                bool have = false;
                auto verify = [&] {
                    if (!have)
                        return;
                    if (count >= expected.size() || expected[count].data() != collected.line.data() ||
                        expected[count].size() != collected.line.size())
                        throw std::logic_error("line mismatch");
                    PreparedMasks ref;
                    prepMasks(expected[count], ref);
                    if (bool(ref.head) != bool(collected.head))
                        throw std::logic_error("eligibility mismatch");
                    if (ref.head && (ref.ht != collected.ht || ref.tt != collected.tt ||
                                     _bzhi_u32(ref.commas, ref.line.size() - 60) !=
                                         _bzhi_u32(collected.commas, ref.line.size() - 60) ||
                                     _bzhi_u32(ref.quotes, ref.line.size() - 60) !=
                                         _bzhi_u32(collected.quotes, ref.line.size() - 60)))
                        throw std::logic_error("mask mismatch");
                    ++count;
                };
                eventLines({file, first, std::min(file.size(), first + span)}, [&]() -> PreparedMasks & {
                    verify();
                    have = true;
                    return collected;
                });
                verify();
            }
            if (count != expected.size())
                throw std::runtime_error("short scan");
            std::cout << terminal << " " << span << " " << count << " OK\n";
        }
    }
}
