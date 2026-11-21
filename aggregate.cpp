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
    bool wide = false, circular = false;
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
    uint64_t clickBits = 0;
    out.clickOwner.resize(bucket.clicks.size);
    at = 0;
    bucket.clicks.each([&](const Click &c) {
        clickBits |= uint64_t(c.ts) - uint64_t(origin) + 7 * dayMs;
        const uint32_t g = index.lookup(c.person);
        out.clickOwner[at++] = g;
        if (g!=absent)
            ++groups[g].clickEnd;
    });
    out.circular = ((usedBits | clickBits) < (uint64_t{1} << 31));
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

struct TimeCell {
    uint32_t first = UINT32_MAX, last = 0, count = 0;
};
class CellTable {
    static constexpr size_t binCount = UINT32_MAX / maxGap + 1;
    static constexpr size_t wordCount = (binCount + 63) / 64;
    static_assert(wordCount <= 64);
    std::array<TimeCell, binCount> cells{};
    std::array<uint64_t, wordCount> occupied{};

  public:
    template<class F> void sessions(const uint32_t* times,size_t n,int64_t origin,F emit) {
        uint64_t words=0;
        for (size_t i=0; i<n; ++i) {
            const uint32_t ts = times[i], bin = ts / uint32_t(maxGap);
            auto& c=cells[bin];
            c.first = std::min(c.first, ts);
            c.last = std::max(c.last, ts);
            words |= uint64_t{1} << (bin / 64);
            occupied[bin / 64] |= uint64_t{1} << (bin % 64);
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
                c = TimeCell{};
            }
        }
        emit(origin+first,origin+last,count);
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

    struct Session {
        int64_t start, last;
        uint32_t count, dayIndex;
    };
    // Each click assists a contiguous range of starts. Keep per-day ends so
    // an entire run of sessions can be credited with one update.
    struct DayRange {
        std::vector<CampaignStats> *daily;
        unsigned end;
    };
    struct Coverage {
        unsigned owner = absent, end = 0;
    };
    std::vector<Coverage> covered(campaignCount);
    static thread_local std::vector<DayRange> spans;
    static thread_local std::vector<Session> sessions;
    constexpr unsigned maxSessions = 256;
    using WordSet = std::array<uint64_t, maxSessions / 64>;
    std::vector<WordSet> coverage(campaignCount);
    std::vector<uint64_t> narrowMask(campaignCount);
    alignas(32) std::array<uint32_t, 64> relStarts{};

    std::vector<uint32_t> touchedClicks;
    static thread_local std::array<uint16_t, 4096> firstInBin;
    std::array<const TimedClick *, maxSessions> latest;
    auto rangeWords = [](unsigned lo, unsigned hi, auto apply) {
        unsigned w = lo / 64, end = (hi - 1) / 64;
        if (w == end) {
            apply(w, (UINT64_MAX << (lo % 64)) & (UINT64_MAX >> (63 - (hi - 1) % 64)));
            return;
        }
        apply(w, UINT64_MAX << (lo % 64));
        while (++w < end)
            apply(w, UINT64_MAX);
        apply(end, UINT64_MAX >> (63 - (hi - 1) % 64));
    };

    for (uint32_t g = 0; g < groups.size(); ++g) {
        const Group& group = groups[g];
        std::vector<CampaignStats> *daily = nullptr;
        UserRow row{};
        const size_t firstOutput = result.users.days.size();
        sessions.clear();
        auto emit = [&](int64_t first, int64_t last, uint32_t count) {
            sessions.push_back({first, last, count});
        };
        const size_t count = group.eventEnd - i;
        if (grouped.wide)
            exactSessions(grouped.wideTimes.data() + i, count, 0, emit);
        else if (count <= 8)
            exactSessions(grouped.times.data() + i, count, origin, emit);
        else
            cells.sessions(grouped.times.data() + i, count, origin, emit);
        const unsigned n = sessions.size();
        const uint64_t span = uint64_t(sessions.back().start) - uint64_t(sessions.front().start);
        constexpr unsigned shortLimit = 64;
        bool narrow = grouped.circular && n <= shortLimit && group.clickEnd - clickBegin >= 1;
        const bool bitset =
            (group.clickEnd - clickBegin >= 32 && n <= maxSessions && span <= UINT32_MAX);
        if (narrow) {
            const int64_t base = sessions.front().start;
            std::fill_n(latest.begin(), n, nullptr);
            for (unsigned j = 0; j < n; ++j)
                relStarts[j] = uint32_t(sessions[j].start);
            const uint64_t all = UINT64_MAX >> (64 - n);
            for (size_t j = clickBegin; j < group.clickEnd; ++j) {
                const auto &click = clicks[j];
                uint64_t mask = 0;
                auto t = _mm256_set1_epi32(uint32_t(click.ts));
                const auto length = _mm256_set1_epi32(7 * dayMs);
                for (unsigned k = 0; k < n; k += 8) {
                    const auto v = _mm256_load_si256((const __m256i *)(relStarts.data() + k));
                    const auto delta = _mm256_sub_epi32(v, t);
                    const auto valid = _mm256_cmpeq_epi32(_mm256_max_epu32(delta, length), length);
                    mask |= uint64_t(unsigned(_mm256_movemask_ps(_mm256_castsi256_ps(valid)))) << k;
                }
                mask &= all;
                if (!mask)
                    continue;
                auto &covered = narrowMask[click.campaign];
                if (!covered)
                    touchedClicks.push_back(click.campaign);
                covered |= mask;
                const unsigned at = std::countr_zero(mask);
                const TimedClick *old = latest[at];
                if (!old || old->ts < click.ts || (old->ts == click.ts && click.id < old->id))
                    latest[at] = &click;
            }
        } else if (bitset) {
            std::fill_n(latest.begin(), n, nullptr);
            const auto first = sessions.front().start, last = sessions.back().start;
            size_t begin = 0;
            for (unsigned k = 0; k < n; ++k) {
                const unsigned end = ((uint64_t(sessions[k].start) - uint64_t(first)) >> 20) + 1;
                std::fill(firstInBin.begin() + begin, firstInBin.begin() + end, k);
                begin = end;
            }
            auto bound = [&](int64_t t, bool inclusive) -> unsigned {
                if (t < first)
                    return 0;
                if (t > last)
                    return n;
                const unsigned k = firstInBin[(uint64_t(t) - uint64_t(first)) >> 20];
                return k + (inclusive ? sessions[k].start <= t : sessions[k].start < t);
            };
            for (size_t j = clickBegin; j < group.clickEnd; ++j) {
                const auto &click = clicks[j];
                unsigned lo = bound(click.ts, false);
                unsigned hi = click.ts > INT64_MAX - 7 * dayMs ? n : bound(click.ts + 7 * dayMs, true);
                if (lo == hi)
                    continue;
                auto &mask = coverage[click.campaign];
                bool fresh = !(mask[0] | mask[1] | mask[2] | mask[3]);
                if (fresh)
                    touchedClicks.push_back(click.campaign);
                rangeWords(lo, hi, [&](unsigned w, uint64_t newBits) { mask[w] |= newBits; });
                const auto *old = latest[lo];
                if (!old || old->ts < click.ts || (old->ts == click.ts && click.id < old->id))
                    latest[lo] = &click;
            }
        } else {
            sortSpan(grouped.clicks.begin() + clickBegin, grouped.clicks.begin() + group.clickEnd,
                     [](const auto &a, const auto &b) {
                         // The last click at a tied timestamp must have the smallest ID.
                         return a.ts != b.ts ? a.ts < b.ts : b.id < a.id;
                     });
        }
        const TimedClick *winnerClick = nullptr;
        const bool hasClicks = clickBegin < group.clickEnd;
        size_t nextClick = clickBegin;
        spans.clear();
        uint32_t nStarts = 0;
        for (auto &session : sessions) {
            const auto start = session.start, last = session.last;
            const uint32_t eventCount = session.count;
            const int32_t day = utcDay(start);
            if (daily == nullptr || row.day != day) {
                if (daily) result.users.append(row);
                for (uint32_t c : touched) categoryRevenue[c] = 0;
                touched.clear();
                row = {group.person, day, 0, 0, 0, absent};
                daily = dailyStats(day);
                if (hasClicks)
                    spans.push_back({daily, nStarts});
            }
            int64_t sessionRevenue = 0;
            for (; p < group.purchaseEnd && purchases[p].ts <= last; ++p) {
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
            uint32_t winner = 0;
            if (hasClicks) {
                session.dayIndex = spans.size() - 1;
                spans.back().end = ++nStarts;
                if (bitset || narrow) {
                    if (latest[nStarts - 1])
                        winnerClick = latest[nStarts - 1];
                    if (winnerClick && winnerClick->ts >= start - 7 * dayMs)
                        winner = winnerClick->campaign;
                } else {
                    while (nextClick < group.clickEnd && clicks[nextClick].ts <= start)
                        ++nextClick;
                    if (nextClick != clickBegin && clicks[nextClick - 1].ts >= start - 7 * dayMs)
                        winner = clicks[nextClick - 1].campaign;
                }
            }
            auto& credit = (*daily)[winner];
            ++credit.sessions;
            credit.purchasing += sessionRevenue != 0;
            credit.revenue += sessionRevenue;
        }
        // A click assists a contiguous range of session starts. Once a prior
        // click in this campaign covers part of that range, credit only its suffix.
        if (narrow) {
            unsigned lo = 0;
            for (const auto &span : spans) {
                const uint64_t dayMask = (UINT64_MAX << lo) & (UINT64_MAX >> (64 - span.end));
                for (auto c : touchedClicks)
                    (*span.daily)[c].assists += std::popcount(dayMask & narrowMask[c]);
                lo = span.end;
            }
            for (auto c : touchedClicks)
                narrowMask[c] = 0;
            touchedClicks.clear();
        } else if (bitset) {
            unsigned lo = 0;
            for (const auto &span : spans) {
                for (auto c : touchedClicks) {
                    unsigned count = 0;
                    rangeWords(lo, span.end, [&](unsigned w, uint64_t bits) {
                        count += std::popcount(coverage[c][w] & bits);
                    });
                    (*span.daily)[c].assists += count;
                }
                lo = span.end;
            }
            for (auto c : touchedClicks)
                coverage[c].fill(0);
            touchedClicks.clear();
        } else if (hasClicks) {
            uint32_t lower = 0, upper = 0;
            for (size_t j = clickBegin; j < group.clickEnd; ++j) {
                const auto &click = clicks[j];
                while (lower < nStarts && sessions[lower].start < click.ts)
                    ++lower;
                if (upper < lower)
                    upper = lower;
                while (upper < nStarts && sessions[upper].start - 7 * dayMs <= click.ts)
                    ++upper;
                auto &state = covered[click.campaign];
                uint32_t left = state.owner == g ? std::max(state.end, lower) : lower;
                while (left < upper) {
                    const auto &span = spans[sessions[left].dayIndex];
                    const uint32_t end = std::min(upper, span.end);
                    (*span.daily)[click.campaign].assists += end - left;
                    left = end;
                }
                state = {g, upper};
            }
        }
        i=group.eventEnd;
        if (daily) {
            result.users.append(row);
            result.users.people.push_back(group.person);
            result.users.userCounts.push_back(
                static_cast<uint32_t>(result.users.days.size() - firstOutput));
        }
        clickBegin = group.clickEnd;
    }
}
