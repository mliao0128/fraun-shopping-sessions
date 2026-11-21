#include "../event_prefixes.hpp"
#include <cassert>
#include <string>

int main() {
    // Train on wrappers and a UTF-8 prefix. Input storage outlives the dictionary.
    std::string csv = "event_id,other\n";
    std::vector<std::string> prefixes{"long-", "pfx-", "préfixe-", "different-"};
    for (unsigned i = 0; i < 100; ++i)
        for (auto& prefix : prefixes) csv += '"' + prefix + "00000000\",z\n";
    EventPrefixes codec({csv});
    std::vector<std::string> ids{"", "1", "12345678", "e00000000", "x00000000",
                                 "unsampled00000000", std::string(90, 'x')};
    for (const auto& prefix : prefixes) {
        for (auto suffix : {"00000000", "a0000000", "0000000a", "!+_ ,x.!",
                            "ABCDEFGH", "abcdefgh", "12345678"}) {
            ids.push_back(prefix + suffix);
            Key key = codec.encode(ids.back());
            assert((key.bits >> 60) == 9);
        }
        ids.push_back(prefix + std::string(8, static_cast<char>(0xe1)));
        assert(codec.encode(ids.back()).external());
        ids.push_back(prefix + std::string("x\0y\0zabc",8));
        assert((codec.encode(ids.back()).bits >> 60) == 9);
    }
    for (const auto& a : ids) for (const auto& b : ids) {
        const auto copy = b;
        assert((codec.encode(a) == codec.encode(copy)) == (a == copy));
    }
    EventPrefixes empty({});
    for (const auto& s : ids) assert(empty.encode(s) == eventID(s));
}
