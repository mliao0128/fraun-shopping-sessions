#include "../aggregate.cpp"
#include <iostream>
#include <random>
#include <tuple>

int main() {
    using Session = std::tuple<int64_t, int64_t, uint32_t>;
    constexpr uint32_t width = 1u << 20;
    std::mt19937 random(29013);
    CellTable cells;
    for (unsigned trial = 0; trial < 12000; ++trial) {
        std::vector<uint32_t> times;
        const unsigned size = trial % 100 == 0 ? 4000 : 1 + random() % 80;
        for (unsigned i = 0; i < size; ++i) {
            uint32_t time = random();
            switch (trial % 7) {
                case 0: time %= width; break;
                case 1: time = (random() % 12) * width + time % width; break;
                case 2: time = (random() % 64) * 64 * width + (time & 3); break;
                case 3: time = (random() % 400) * maxGap + random() % 3; break;
                case 4: time = UINT32_MAX - time % (2 * width); break;
            }
            times.push_back(time);
        }
        if (trial % 3 == 0) {
            const uint32_t start = random() % 1000000;
            times.insert(times.end(), {0, 0, width-1, width, width+1, UINT32_MAX,
                         UINT32_MAX-width, start, start+1799999, start+1800000, start+1800001});
        }
        const int64_t origin = trial % 2 ? -2208988800123LL : 1790812800000LL;
        std::shuffle(times.begin(), times.end(), random);
        std::vector<Session> actual, expected;
        cells.sessions(times.data(), times.size(), origin, [&](auto first, auto last, auto count) {
            actual.emplace_back(first, last, count);
        });
        std::sort(times.begin(), times.end());
        for (size_t first = 0; first < times.size();) {
            size_t end = first+1;
            while (end < times.size() && uint64_t(times[end]) - times[end-1] <= maxGap) ++end;
            expected.emplace_back(origin+times[first], origin+times[end-1], end-first);
            first = end;
        }
        if (actual != expected) {
            std::cerr << "Session difference on trial " << trial << '\n';
            return 1;
        }
    }
}
