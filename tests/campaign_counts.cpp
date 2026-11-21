#include "../shared.hpp"
#include <cassert>
int main() {
    Result r;
    r.startBucket(UINT32_MAX - 1);
    auto &day = r.byDay.try_emplace(0, 2).first->second;
    day.addAssists(1, UINT32_MAX - 1);
    day.credit(1, 700);
    r.startBucket(1);
    day.addAssists(1, 1);
    r.startBucket(1);
    day.credit(1, 0);
    day.addAssists(1, 1);
    auto value = day.total(1);
    assert(value.sessions == 2 && value.purchasing == 1 && value.revenue == 700 &&
           value.assists == int64_t(UINT32_MAX) + 1);
    r.startBucket(UINT32_MAX - 1);
    day.addAssists(1, UINT32_MAX - 1);
    r.startBucket(1);
    day.addAssists(1, 1);
    day.credit(1, 33);
    value = day.total(1);
    assert(value.assists == 2LL * UINT32_MAX + 1 && value.revenue == 733 && value.sessions == 3 &&
           value.purchasing == 2);
    const auto unused = day.total(0);
    assert(unused.assists == 0 && unused.sessions == 0 && unused.revenue == 0 && unused.purchasing == 0);
}
