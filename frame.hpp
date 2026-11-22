#pragma once
#include <cstdint>
#include <limits>

// Every admitted click/event lies between lookbackBegin and windowEnd.
// After UTC-midnight alignment the largest admitted offset is exclusiveEnd-1.
struct CalendarFrame {
    static constexpr int64_t day = 86400000;
    int64_t anchor = 0;
    int32_t calendarDay = 0;
    bool usable = false, circular = false;

    static CalendarFrame from(int64_t start, int64_t end) {
        if (end <= start || start < INT64_MIN + 8*day) return {};
        const int64_t earliest = start - 7*day;
        const int64_t d = earliest/day - (earliest%day < 0);
        const int64_t last = end-1;
        const int64_t lastDay = last/day - (last%day < 0);
        if (d < INT32_MIN || lastDay > INT32_MAX) return {};
        const int64_t anchor = d*day;
        const uint64_t extent = uint64_t(end)-uint64_t(anchor);
        return {anchor,static_cast<int32_t>(d),extent<=(uint64_t(1)<<32),extent<=(uint64_t(1)<<31)};
    }
};
