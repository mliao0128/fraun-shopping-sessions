#include "shared.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <bit>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t absent = std::numeric_limits<uint32_t>::max();
constexpr int64_t dayMs = 86400000;
constexpr int64_t maxGap = 1800000;

struct Group {
    Key person;
    uint32_t eventEnd = 0;
    uint32_t clickEnd = 0;
};

// Sessions need only the times. The few purchases travel separately.
struct Purchase {
    int64_t ts;
    uint32_t price, category, group;
};

struct TimedClick {
    int64_t ts;
    uint32_t campaign;
    Key id;
};

// Most people have only a handful of rows. Avoid invoking introsort hundreds
// of thousands of times for those tiny spans; whales retain std::sort.
template<class Iterator, class Less>
void sortSpan(Iterator begin, Iterator end, Less less) {
    if (end - begin > 32) {
        std::sort(begin, end, less);
        return;
    }
    for (auto at = begin + (begin != end); at != end; ++at) {
        auto value = std::move(*at);
        auto hole = at;
        while (hole != begin && less(value, *(hole - 1))) {
            *hole = std::move(*(hole - 1));
            --hole;
        }
        *hole = std::move(value);
    }
}

// The caller already partitions by the low hash bits. Use the high bits here
// so a whole bucket does not fall into the same few slots again.
class PersonIndex {
    std::vector<uint32_t> slots;
    std::vector<Group>& groups;

    size_t slot(Key person) const {
        size_t pos = std::rotr(uint64_t(person.hash()), 32) & (slots.size() - 1);
        while (slots[pos] != absent && !(groups[slots[pos]].person == person))
            pos = (pos + 1) & (slots.size() - 1);
        return pos;
    }

public:
    PersonIndex(std::vector<Group>& groups, size_t rows) : slots(std::max<size_t>(16, std::bit_ceil(rows / 6 + 1)), absent), groups(groups) {}

    uint32_t lookup(Key person) const { return slots[slot(person)]; }

    uint32_t add(Key person) {
        size_t pos = slot(person);
        if (slots[pos] != absent) return slots[pos];
        if (groups.size() * 10 >= slots.size() * 7) {
            slots.assign(slots.size() * 2, absent);
            for (uint32_t i = 0; i < groups.size(); ++i)
                slots[slot(groups[i].person)] = i;
            pos = slot(person);
        }
        uint32_t index = static_cast<uint32_t>(groups.size());
        groups.push_back({person});
        slots[pos] = index;
        return index;
    }
};

// Least-significant-digit radix sort of the low `bits` bits; higher bits are zero.
void sortKeys(std::vector<uint64_t>& keys, unsigned bits) {
    const size_t count = keys.size();
    if (count < 64) {
        std::sort(keys.begin(), keys.end());
        return;
    }
    constexpr unsigned digitBits = 11, digits = 1u << digitBits, maxPasses = 6;
    const unsigned passes = (bits + digitBits - 1) / digitBits;
    static thread_local std::vector<uint64_t> spare;
    static thread_local std::vector<uint32_t> starts;
    spare.resize(count);
    starts.assign(size_t{passes} * digits, 0);
    for (uint64_t key : keys)
        for (unsigned pass = 0; pass < passes; ++pass)
            ++starts[pass * digits + ((key >> (pass * digitBits)) & (digits - 1))];
    uint64_t* from = keys.data();
    uint64_t* to = spare.data();
    static_assert(maxPasses * digitBits >= 64);
    for (unsigned pass = 0; pass < passes; ++pass) {
        uint32_t* start = starts.data() + pass * digits;
        const unsigned shift = pass * digitBits;
        // A digit shared by every key needs no pass.
        if (start[(from[0] >> shift) & (digits - 1)] == count) continue;
        uint32_t total = 0;
        for (unsigned d = 0; d < digits; ++d) {
            const uint32_t n = start[d];
            start[d] = total;
            total += n;
        }
        for (size_t i = 0; i < count; ++i)
            to[start[(from[i] >> shift) & (digits - 1)]++] = from[i];
        std::swap(from, to);
    }
    if (from != keys.data()) keys.swap(spare);
}

// Events become one sorted array of (person, time) keys: a person's events are
// contiguous and ordered, and the gap between neighbours is a plain subtraction.
// Clicks are counted and scattered into per-person spans.
struct Grouped {
    std::vector<Group> groups;
    std::vector<uint64_t> keys;       // group << timeBits | (ts - origin)
    std::vector<Purchase> purchases;  // ordered by group, then time
    std::vector<TimedClick> clicks;
    std::vector<uint32_t> owners;
    int64_t origin = 0;
    unsigned timeBits = 0;
};

void groupRows(const Bucket& bucket, Grouped& out, int64_t origin) {
    if (bucket.events.size >= absent || bucket.clicks.size >= absent)
        throw std::runtime_error("Too many rows in one bucket");
    auto& groups = out.groups;
    groups.clear();
    out.purchases.clear();
    PersonIndex index(groups, bucket.events.size);
    out.keys.resize(bucket.events.size);
    out.owners.resize(std::max(bucket.events.size, bucket.clicks.size));
    int64_t low = INT64_MAX, high = INT64_MIN;
    size_t at = 0;
    bucket.events.each([&](const Event& e) {
        const uint32_t g = index.add(e.person);
        const EventData row = e.unpack(origin);
        out.owners[at] = g;
        out.keys[at++] = static_cast<uint64_t>(row.ts);
        ++groups[g].eventEnd;
        low = std::min(low, row.ts);
        high = std::max(high, row.ts);
        if (row.price) out.purchases.push_back({row.ts, row.price, row.category, g});
    });
    out.origin = low;
    out.timeBits = std::bit_width(static_cast<uint64_t>(high) - static_cast<uint64_t>(low));
    const unsigned bits = out.timeBits + std::bit_width(groups.size() - 1);
    if (bits > 64) throw std::runtime_error("Timestamps span too wide a range");
    for (size_t i = 0; i < at; ++i)
        out.keys[i] = (out.timeBits == 64 ? 0 : uint64_t{out.owners[i]} << out.timeBits)
                      | (out.keys[i] - static_cast<uint64_t>(low));
    sortKeys(out.keys, bits);
    std::sort(out.purchases.begin(), out.purchases.end(), [](const Purchase& a, const Purchase& b) {
        return a.group != b.group ? a.group < b.group : a.ts < b.ts;
    });

    at = 0;
    bucket.clicks.each([&](const Click& c) {
        const uint32_t g = index.lookup(c.person);
        out.owners[at++] = g;
        if (g != absent) ++groups[g].clickEnd;
    });
    uint32_t nEvents = 0, nClicks = 0;
    for (auto& g : groups) {
        const uint32_t clickCount = g.clickEnd;
        nEvents += g.eventEnd;
        g.eventEnd = nEvents;
        g.clickEnd = nClicks;
        nClicks += clickCount;
    }
    out.clicks.resize(nClicks);
    at = 0;
    bucket.clicks.each([&](const Click& c) {
        const uint32_t g = out.owners[at++];
        if (g != absent) out.clicks[groups[g].clickEnd++] = {c.ts, c.campaign, c.id};
    });
}

class ClickWindow {
    std::vector<uint32_t> count, position, active;
    size_t first = 0, next = 0;

public:
    explicit ClickWindow(size_t campaigns) : count(campaigns), position(campaigns) {}

    void begin(size_t firstClick) {
        for (uint32_t c : active) count[c] = 0;
        active.clear();
        first = next = firstClick;
    }

    uint32_t attribute(const std::vector<TimedClick>& clicks, size_t end, int64_t start,
                       std::vector<CampaignStats>& daily) {
        while (next < end && clicks[next].ts <= start) {
            uint32_t c = clicks[next++].campaign;
            if (count[c]++ == 0) {
                position[c] = static_cast<uint32_t>(active.size());
                active.push_back(c);
            }
        }
        while (first < next && clicks[first].ts < start - 7 * dayMs) {
            uint32_t c = clicks[first++].campaign;
            if (--count[c] == 0) {
                const uint32_t last = active.back();
                active[position[c]] = last;
                position[last] = position[c];
                active.pop_back();
            }
        }
        for (uint32_t c : active) ++daily[c].assists;
        return first == next ? 0 : clicks[next - 1].campaign;
    }
};

int32_t utcDay(int64_t ts) {
    return static_cast<int32_t>(ts / dayMs - (ts % dayMs < 0));
}

}  // namespace

void aggregate(Bucket& bucket, Result& result, size_t categoryCount, size_t campaignCount, int64_t origin) {
    if (!bucket.events.size) return;
    // Scratch space is kept per thread: buckets are alike in size, and reusing
    // it spares the allocator and the kernel a round trip per bucket.
    static thread_local Grouped grouped;
    groupRows(bucket, grouped, origin);
    const auto& groups = grouped.groups;
    const auto& keys = grouped.keys;
    const auto& purchases = grouped.purchases;
    const auto& clicks = grouped.clicks;
    const uint64_t timeMask = grouped.timeBits == 64 ? ~uint64_t{0} : (uint64_t{1} << grouped.timeBits) - 1;

    ClickWindow window(campaignCount);
    std::vector<int64_t> categoryRevenue(categoryCount);
    std::vector<uint32_t> touched;
    size_t i = 0, p = 0, clickBegin = 0;
    struct DaySlot { int32_t day; std::vector<CampaignStats>* stats; };
    std::array<DaySlot, 64> dayCache{};
    auto dailyStats = [&](int32_t day) {
        auto& slot = dayCache[static_cast<uint32_t>(day) % dayCache.size()];
        if (!slot.stats || slot.day != day) {
            slot.day = day;
            slot.stats = &result.byDay.try_emplace(day, campaignCount).first->second;
        }
        return slot.stats;
    };

    for (uint32_t g = 0; g < groups.size(); ++g) {
        const Group& group = groups[g];
        sortSpan(grouped.clicks.begin() + clickBegin, grouped.clicks.begin() + group.clickEnd,
                 [](const auto& a, const auto& b) {
                      // The last click at a tied timestamp must have the smallest ID.
                      return a.ts != b.ts ? a.ts < b.ts : b.id < a.id;
                  });
        window.begin(clickBegin);
        std::vector<CampaignStats>* daily = nullptr;
        UserRow row{};
        while (i < group.eventEnd) {
            const int64_t start = static_cast<int64_t>(keys[i] & timeMask) + grouped.origin;
            const int32_t day = utcDay(start);
            if (daily == nullptr || row.day != day) {
                if (daily) result.users.append(row);
                for (uint32_t c : touched) categoryRevenue[c] = 0;
                touched.clear();
                row = {group.person, day, 0, 0, 0, absent};
                daily = dailyStats(day);
            }
            // Within one person the keys differ exactly as the timestamps do.
            const size_t sessionBegin = i++;
            while (i < group.eventEnd && keys[i] - keys[i - 1] <= uint64_t{maxGap}) ++i;
            // Earlier purchases went to earlier sessions, so these are this session's.
            const int64_t last = static_cast<int64_t>(keys[i - 1] & timeMask) + grouped.origin;
            int64_t sessionRevenue = 0;
            for (; p < purchases.size() && purchases[p].group == g && purchases[p].ts <= last; ++p) {
                const auto& e = purchases[p];
                sessionRevenue += e.price;
                const uint32_t c = e.category;
                if (categoryRevenue[c] == 0) touched.push_back(c);
                const int64_t amount = (categoryRevenue[c] += e.price);
                if (row.top == absent || amount > categoryRevenue[row.top] ||
                    (amount == categoryRevenue[row.top] && c < row.top))
                    row.top = c;
            }

            ++row.sessions;
            row.events += static_cast<uint32_t>(i - sessionBegin);
            row.revenue += sessionRevenue;
            uint32_t winner = window.attribute(clicks, group.clickEnd, start, *daily);
            auto& credit = (*daily)[winner];
            ++credit.sessions;
            credit.purchasing += sessionRevenue != 0;
            credit.revenue += sessionRevenue;
        }
        if (daily) result.users.append(row);
        clickBegin = group.clickEnd;
    }
}
