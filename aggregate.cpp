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
    uint32_t purchaseEnd = 0;
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

struct OwnedTime { uint32_t group, offset; };
struct Grouped {
    std::vector<Group> groups;
    std::vector<Purchase> purchases;
    std::vector<TimedClick> clicks;
    std::vector<OwnedTime> pending;
    std::vector<uint32_t> times, clickOwner;
    std::vector<int64_t> wideTimes;
    bool wide = false;
};

void groupRows(const Bucket& bucket, Grouped& out, int64_t origin) {
    if (bucket.events.size >= absent || bucket.clicks.size >= absent)
        throw std::runtime_error("Too many rows in one bucket");
    auto& groups = out.groups;
    groups.clear();
    out.purchases.clear();
    PersonIndex index(groups,bucket.events.size);
    out.pending.resize(bucket.events.size);
    uint64_t usedBits = 0;
    size_t at = 0;
    bucket.events.each([&](const Event& e) {
        uint32_t g = index.add(e.person);
        const EventData row = e.unpack(origin);
        const uint64_t delta = uint64_t(row.ts)-uint64_t(origin);
        usedBits |= delta;
        out.pending[at++] = {g,static_cast<uint32_t>(delta)};
        ++groups[g].eventEnd;
        if (row.price) { ++groups[g].purchaseEnd; out.purchases.push_back({row.ts,row.price,row.category,g}); }
    });
    out.wide = usedBits > UINT32_MAX;
    out.clickOwner.resize(bucket.clicks.size);
    at = 0;
    bucket.clicks.each([&](const Click& c) {
        const uint32_t g = index.lookup(c.person);
        out.clickOwner[at++] = g;
        if (g!=absent) ++groups[g].clickEnd;
    });
    uint32_t nEvents=0, nClicks=0;
    for(auto& g : groups) {
        uint32_t ne=g.eventEnd, nc=g.clickEnd;
        g.eventEnd=nEvents; g.clickEnd=nClicks;
        nEvents+=ne; nClicks+=nc;
    }
    if (!out.wide) {
        out.times.resize(nEvents);
        for (auto row : out.pending)
            out.times[groups[row.group].eventEnd++] = row.offset;
    } else {
        // Revisit immutable rows only if at least one offset needs more than
        // 32 bits. All full-width event times are preserved in this path.
        out.wideTimes.resize(nEvents);
        at=0;
        bucket.events.each([&](const Event& e) {
            uint32_t g=out.pending[at++].group;
            out.wideTimes[groups[g].eventEnd++] = e.unpack(origin).ts;
        });
    }
    out.clicks.resize(nClicks);
    at=0;
    bucket.clicks.each([&](const Click& c) {
        uint32_t g=out.clickOwner[at++];
        if (g!=absent) out.clicks[groups[g].clickEnd++] = {c.ts,c.campaign,c.id};
    });
    // Group purchases with a counting scatter. Only purchases by the
    // same person need time ordering, and most such spans are short.
    uint32_t cursor = 0;
    for (Group& group : groups) {
        const uint32_t count = group.purchaseEnd;
        group.purchaseEnd = cursor;
        cursor += count;
    }
    static thread_local std::vector<Purchase> ordered;
    ordered.resize(out.purchases.size());
    for (const Purchase& row : out.purchases) ordered[groups[row.group].purchaseEnd++] = row;
    out.purchases.swap(ordered);
    size_t first = 0;
    for (const Group& group : groups) {
        if (group.purchaseEnd - first > 1) {
            sortSpan(out.purchases.begin()+first,out.purchases.begin()+group.purchaseEnd,
                     [](const Purchase& a,const Purchase& b) { return a.ts < b.ts; });
        }
        first = group.purchaseEnd;
    }

}

// Sorting a few timestamps is cheaper than setting up an occupancy bitmap.
// This is also the general fallback for windows wider than 32 offset bits.
template<class T,class F>
void exactSessions(T* times,size_t n,int64_t origin,F emit) {
    sortSpan(times,times+n,std::less<T>{});
    size_t start=0;
    for (size_t i=1; i<n; ++i) {
        if (uint64_t(times[i])-uint64_t(times[i-1])<=uint64_t(maxGap)) continue;
        emit(origin+int64_t(times[start]),origin+int64_t(times[i-1]),uint32_t(i-start));
        start=i;
    }
    emit(origin+int64_t(times[start]),origin+int64_t(times[n-1]),uint32_t(n-start));
}

struct TimeCell { uint32_t first=0,last=0,count=0; };
class CellTable {
    static constexpr unsigned shift=20;
    static_assert((1u<<shift)<=maxGap);
    // Each worker handles one person at a time. Reuse the same direct table
    // across people and clear exactly the occupied cells after consuming them.
    std::array<TimeCell,1u<<(32-shift)> cells{};
    std::array<uint64_t,64> occupied{};
public:
    template<class F> void sessions(const uint32_t* times,size_t n,int64_t origin,F emit) {
        uint64_t words=0;
        for (size_t i=0; i<n; ++i) {
            const uint32_t ts=times[i], bin=ts>>shift;
            auto& c=cells[bin];
            if (!c.count) {
                words|=uint64_t{1}<<(bin/64);
                occupied[bin/64]|=uint64_t{1}<<(bin%64);
                c.first=c.last=ts;
            } else {
                c.first=std::min(c.first,ts);
                c.last=std::max(c.last,ts);
            }
            ++c.count;
        }
        // All times in one cell are connected. The earliest time in the next
        // occupied cell and the latest in the previous cell give the exact gap.
        uint32_t first=0,last=0,count=0;
        while (words) {
            unsigned w=std::countr_zero(words);
            uint64_t bins=occupied[w];
            words&=words-1;
            occupied[w]=0;
            while (bins) {
                auto& c=cells[w*64+std::countr_zero(bins)];
                bins&=bins-1;
                if (!count || c.first-last>maxGap) {
                    if (count) emit(origin+first,origin+last,count);
                    first=c.first;
                    count=0;
                }
                count+=c.count;
                last=c.last;
                c.count=0;
            }
        }
        emit(origin+first,origin+last,count);
    }
};

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
    const auto& purchases = grouped.purchases;
    const auto& clicks = grouped.clicks;
    static thread_local CellTable cells;

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
        auto emit = [&](int64_t start,int64_t last,uint32_t eventCount) {
            const int32_t day = utcDay(start);
            if (daily == nullptr || row.day != day) {
                if (daily) result.users.append(row);
                for (uint32_t c : touched) categoryRevenue[c] = 0;
                touched.clear();
                row = {group.person, day, 0, 0, 0, absent};
                daily = dailyStats(day);
            }
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
            row.events += eventCount;
            row.revenue += sessionRevenue;
            uint32_t winner = window.attribute(clicks, group.clickEnd, start, *daily);
            auto& credit = (*daily)[winner];
            ++credit.sessions;
            credit.purchasing += sessionRevenue != 0;
            credit.revenue += sessionRevenue;
        };
        const size_t count=group.eventEnd-i;
        if (grouped.wide) exactSessions(grouped.wideTimes.data()+i,count,0,emit);
        else if (count<=8) exactSessions(grouped.times.data()+i,count,origin,emit);
        else cells.sessions(grouped.times.data()+i,count,origin,emit);
        i=group.eventEnd;
        if (daily) result.users.append(row);
        clickBegin = group.clickEnd;
    }
}
