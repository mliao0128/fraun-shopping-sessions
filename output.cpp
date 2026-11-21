#include "shared.hpp"
#include "flat_parquet.hpp"

namespace fs = std::filesystem;
namespace {
using namespace flat;
constexpr Field userSchema[] = {
    {"user_id",Bytes}, {"day",Int32}, {"sessions",Int64}, {"events",Int64},
    {"revenue_cents",Int64}, {"top_category",Bytes,true}
};
constexpr Field campaignSchema[] = {
    {"campaign_id",Bytes}, {"channel",Bytes}, {"day",Int32}, {"sessions",Int64},
    {"purchasing_sessions",Int64}, {"revenue_cents",Int64}, {"assisted_sessions",Int64}
};
constexpr size_t rowsPerGroup = 65536;
struct CampaignRow { uint32_t id; int32_t day; CampaignStats stats; };
}

void writeOutputs(const fs::path& root, const Context& ctx, std::vector<Result>& results, unsigned workers, const std::function<bool(size_t)>& aggregateNext) {
    auto userDir = root / "user_daily", campaignDir = root / "campaign_daily";
    fs::create_directories(userDir);
    fs::create_directories(campaignDir);
    size_t files = std::min<size_t>(workers, results.size());
    parallel(files, workers, [&](size_t part) {
        Writer file(userDir / ("part-" + std::to_string(part) + ".parquet"),userSchema);
        auto& rows = results[part].users;
        // Flush while recent output is still in cache and reuse the same
        // bounded column buffers for the next group of input buckets.
        for (;;) {
            const bool more = aggregateNext(part);
            if (more && rows.days.size() < 32768)
                continue;
            size_t run = 0, consumed = 0;
            for (size_t start = 0; start < rows.days.size(); start += rowsPerGroup) {
                const size_t n = std::min(rowsPerGroup, rows.days.size() - start);
                file.group(n);
                file.userRuns(n, rows.people, rows.userCounts, run, consumed);
                file.days(rows.days.data() + start, n);
                file.counts64(rows.sessions.data() + start, n);
                file.counts64(rows.events.data() + start, n);
                file.numbers(rows.revenue.data() + start, n);
                file.categories(rows.top.data() + start, n, ctx.categories);
            }
            rows.userCounts.clear();
            rows.people.clear();
            rows.days.clear();
            rows.sessions.clear();
            rows.events.clear();
            rows.revenue.clear();
            rows.top.clear();
            if (!more) break;
        }
        file.close();
    });
    std::unordered_map<int32_t, std::vector<CampaignStats>> totals;
    for (const auto& result : results) for (const auto& [day, stats] : result.byDay) {
        auto& daily = totals.try_emplace(day, stats.size()).first->second;
        for (size_t i = 0; i < stats.size(); ++i) {
            daily[i].sessions += stats[i].sessions;
            daily[i].purchasing += stats[i].purchasing;
            daily[i].revenue += stats[i].revenue;
            daily[i].assists += stats[i].assists;
        }
    }
    std::vector<CampaignRow> rows;
    for (const auto& [day, daily] : totals)
        for (uint32_t i = 0; i < daily.size(); ++i)
            if (daily[i].sessions || daily[i].assists) rows.push_back({i, day, daily[i]});

    Writer file(campaignDir / "part-0.parquet",campaignSchema);
    std::vector<int32_t> days;
    std::vector<int64_t> values;
    for (size_t start = 0; start < rows.size(); start += rowsPerGroup) {
        size_t n = std::min(rowsPerGroup,rows.size()-start);
        file.group(n);
        file.strings<false>(n,[&](size_t j) { return &ctx.campaignNames[rows[start+j].id]; });
        file.strings<false>(n,[&](size_t j) { return &ctx.channels[rows[start+j].id]; });
        days.resize(n);
        for (size_t j=0; j<n; ++j) days[j] = rows[start+j].day;
        file.numbers(days.data(),n);
        values.resize(n);
        for (auto member : {&CampaignStats::sessions, &CampaignStats::purchasing,
                            &CampaignStats::revenue, &CampaignStats::assists}) {
            for (size_t j=0; j<n; ++j) values[j] = rows[start+j].stats.*member;
            file.numbers(values.data(),n);
        }
    }
    file.close();
}
