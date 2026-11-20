#include "shared.hpp"

#include <algorithm>
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
    size_t eventEnd = 0;
    size_t clickEnd = 0;
};

struct TimedEvent {
    int64_t ts;
    uint32_t price, category;
};

struct TimedClick {
    int64_t ts;
    uint32_t campaign;
    Key id;
};

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
    explicit PersonIndex(std::vector<Group>& groups) : slots(16, absent), groups(groups) {}

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

// Count and scatter into contiguous spans. Subsequent sorts compare only
// timestamps within one person and don't move a person ID with every event.
void groupRows(Bucket& bucket, std::vector<Group>& groups, std::vector<TimedEvent>& events,
               std::vector<TimedClick>& clicks) {
    PersonIndex index(groups);
    std::vector<uint32_t> eventOwners(bucket.events.size());
    for (size_t i = 0; i < bucket.events.size(); ++i) {
        uint32_t g = index.add(bucket.events[i].person);
        eventOwners[i] = g;
        ++groups[g].eventEnd;
    }
    std::vector<uint32_t> clickOwners(bucket.clicks.size());
    for (size_t i = 0; i < bucket.clicks.size(); ++i) {
        uint32_t g = index.lookup(bucket.clicks[i].person);
        clickOwners[i] = g;
        if (g != absent) ++groups[g].clickEnd;
    }
    size_t nEvents = 0, nClicks = 0;
    for (auto& g : groups) {
        const size_t eventCount = g.eventEnd, clickCount = g.clickEnd;
        g.eventEnd = nEvents;
        g.clickEnd = nClicks;
        nEvents += eventCount;
        nClicks += clickCount;
    }
    events.resize(nEvents);
    for (size_t i = 0; i < bucket.events.size(); ++i) {
        const auto& e = bucket.events[i];
        events[groups[eventOwners[i]].eventEnd++] = {e.ts, e.price, e.category};
    }
    std::vector<Event>().swap(bucket.events);
    std::vector<uint32_t>().swap(eventOwners);
    clicks.resize(nClicks);
    for (size_t i = 0; i < bucket.clicks.size(); ++i) {
        if (clickOwners[i] == absent) continue;
        const auto& c = bucket.clicks[i];
        clicks[groups[clickOwners[i]].clickEnd++] = {c.ts, c.campaign, c.id};
    }
    std::vector<Click>().swap(bucket.clicks);
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

void aggregate(Bucket& bucket, Result& result, size_t categoryCount, size_t campaignCount) {
    if (bucket.events.empty()) {
        std::vector<Click>().swap(bucket.clicks);
        return;
    }
    std::vector<Group> groups;
    std::vector<TimedEvent> events;
    std::vector<TimedClick> clicks;
    groupRows(bucket, groups, events, clicks);

    ClickWindow window(campaignCount);
    std::vector<int64_t> categoryRevenue(categoryCount);
    std::vector<uint32_t> touched;
    size_t eventBegin = 0, clickBegin = 0;

    for (const Group& group : groups) {
        std::sort(events.begin() + eventBegin, events.begin() + group.eventEnd,
                  [](const auto& a, const auto& b) { return a.ts < b.ts; });
        std::sort(clicks.begin() + clickBegin, clicks.begin() + group.clickEnd,
                  [](const auto& a, const auto& b) {
                      // The last click at a tied timestamp must have the smallest ID.
                      return a.ts != b.ts ? a.ts < b.ts : b.id < a.id;
                  });
        window.begin(clickBegin);
        std::vector<CampaignStats>* daily = nullptr;
        size_t i = eventBegin;
        while (i < group.eventEnd) {
            const int64_t start = events[i].ts;
            const int32_t day = utcDay(start);
            if (daily == nullptr || result.users.back().day != day) {
                for (uint32_t c : touched) categoryRevenue[c] = 0;
                touched.clear();
                result.users.push_back({group.person, day, 0, 0, 0, absent});
                daily = &result.byDay.try_emplace(day, campaignCount).first->second;
            }
            UserRow& row = result.users.back();
            const size_t sessionBegin = i;
            int64_t sessionRevenue = 0;
            do {
                const auto& e = events[i++];
                if (e.price == 0) continue;
                sessionRevenue += e.price;
                const uint32_t c = e.category;
                if (categoryRevenue[c] == 0) touched.push_back(c);
                const int64_t amount = (categoryRevenue[c] += e.price);
                if (row.top == absent || amount > categoryRevenue[row.top] ||
                    (amount == categoryRevenue[row.top] && c < row.top))
                    row.top = c;
            } while (i < group.eventEnd && events[i].ts - events[i - 1].ts <= maxGap);

            ++row.sessions;
            row.events += static_cast<uint32_t>(i - sessionBegin);
            row.revenue += sessionRevenue;
            uint32_t winner = window.attribute(clicks, group.clickEnd, start, *daily);
            auto& credit = (*daily)[winner];
            ++credit.sessions;
            credit.purchasing += sessionRevenue != 0;
            credit.revenue += sessionRevenue;
        }
        eventBegin = group.eventEnd;
        clickBegin = group.clickEnd;
    }
}
