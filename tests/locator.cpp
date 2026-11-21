// Compile on x86-64 with -std=c++20 -O2 -march=haswell -ffunction-sections
// and -Wl,--gc-sections to exercise this translation unit in isolation.
#include "../ingest.cpp"
#include <cassert>

int main() {
    Reader reader(256, 1);
    Source source;
    constexpr int64_t half = int64_t{1} << 31;
    const std::array<int64_t, 7> origins{
        INT64_MIN + half, INT64_MIN + half + 1, -half, 0, half,
        INT64_MAX - INT32_MAX - 1, INT64_MAX - INT32_MAX
    };
    const std::array<int64_t, 10> arrivals{
        INT64_MIN, INT64_MIN + 1, INT64_MAX, INT64_MAX - 1,
        -half - 1, -half, half - 1, half, 0, 1
    };

    for (uint32_t bucket = 0; bucket < 1024; ++bucket) {
        for (int64_t origin : origins) {
            for (int64_t value : arrivals) {
                for (uint32_t index : {0u, indexLimit - 1, indexLimit, UINT32_MAX}) {
                    uint64_t info = location(reader, value, origin, bucket, index);
                    assert(arrival(info, reader, source, origin) == value);
                    const __int128 delta = __int128(value) - origin;
                    bool fits = delta >= -half && delta < half && index < indexLimit;
                    assert((uint32_t(info) < sourceTag) == fits);
                    if (fits) {
                        assert((uint32_t(info) >> indexBits) == bucket);
                        assert((info & (indexLimit - 1)) == index);
                    } else {
                        const Extended& extended = reader.slow[specialValue(info)];
                        assert(extended.bucket == bucket && extended.index == index);
                    }
                }
            }
        }
    }
    for (uint64_t offset : {0ULL, 0xffffffffULL, 0x100000000ULL, 0xffffffffffULL}) {
        for (uint32_t tag : {sourceTag, slowTag})
            assert(specialValue(special(offset, tag)) == offset);
    }
}
