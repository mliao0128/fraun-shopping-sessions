#include "../ingest.cpp"
#include <cassert>
#include <random>
#include <unordered_map>

// CRC and bit extraction are linear over GF(2). A rank of 63 proves that
// no two of the 2^63 possible nine-byte ASCII strings have the same token.
int main() {
    std::array<uint64_t,63> basis{};
    unsigned rank = 0;
    std::string key(9, '\0');
    assert(tokenFromText(key).bits == 0);
    for (unsigned byte=0; byte<9; ++byte) {
        for (unsigned bit=0; bit<7; ++bit) {
            key[byte] = char(1u << bit);
            uint64_t v = tokenFromText(key).bits;
            assert((v >> 63) == 0);
            while (v) {
                const unsigned high = std::bit_width(v) - 1;
                if (basis[high]) v ^= basis[high];
                else { basis[high] = v; ++rank; break; }
            }
            key[byte] = '\0';
        }
    }
    assert(rank == 63);
    for (unsigned i=0;i<9;++i) {
        key[i]=char(0xff);
        std::string other = key;
        auto a=tokenFromText(key), b=tokenFromText(other);
        assert(a.external() && b.external() && a==b);
        key[i]='\0';
    }
    std::mt19937_64 rng(451);
    std::unordered_map<uint32_t,std::string> seen;
    for (unsigned i=0; i<1000000; ++i) {
        for (char& c:key) c=char('a'+rng()%26);
        const auto tok = tokenFromText(key);
        auto [it,inserted] = seen.emplace(uint32_t(tok.bits), key);
        if (!inserted && it->second!=key) {
            assert(!(tok==tokenFromText(it->second)));
            std::printf("rank=%u CRC32C collision %s %s %08x\n",rank,it->second.c_str(),key.c_str(),uint32_t(tok.bits));
            return 0;
        }
    }
    return 1;
}

