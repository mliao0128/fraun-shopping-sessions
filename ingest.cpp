#include "shared.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
constexpr int64_t invalidTime = INT64_MIN;
constexpr int64_t dayMs = 86'400'000;
constexpr unsigned partitionBits = 8;
constexpr size_t partitions = 1u << partitionBits;

int pairValue(const char* p) {
    return 10 * (p[0] - '0') + p[1] - '0';
}

bool digit(char c) { return static_cast<unsigned>(c - '0') < 10; }

// A dataset usually spans only a few weeks. Cache validated dates, comparing
// all ten input bytes so even malformed dates cannot alias a cached date.
int64_t midnight(const char* p) {
    struct Entry { uint64_t yearMonth; uint16_t day; int64_t ms; };
    static thread_local std::array<Entry, 64> cache{};
    uint64_t yearMonth;
    uint16_t digits;
    std::memcpy(&yearMonth, p, 8);
    std::memcpy(&digits, p + 8, 2);
    auto& entry = cache[((static_cast<unsigned>(p[8]) & 3) << 4)
                        | (static_cast<unsigned>(p[9]) & 15)];
    if (entry.yearMonth == yearMonth && entry.day == digits && yearMonth != 0)
        return entry.ms;
    if (p[4] != '-' || p[7] != '-' || !digit(p[0]) || !digit(p[1]) ||
        !digit(p[2]) || !digit(p[3]) || !digit(p[5]) || !digit(p[6]) ||
        !digit(p[8]) || !digit(p[9]))
        return invalidTime;
    int year = 100 * pairValue(p) + pairValue(p + 2);
    int month = pairValue(p + 5);
    int day = pairValue(p + 8);
    if (year == 0 || month < 1 || month > 12 || day < 1) return invalidTime;
    constexpr int lengths[]{0,31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    if (day > lengths[month] + (month == 2 && leap)) return invalidTime;

    year -= month <= 2;
    const int era = year / 400;
    const int yoe = year % 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    entry = {yearMonth, digits, days * dayMs};
    return entry.ms;
}

int64_t validIngest(const char* p) {
    // The input contract guarantees a valid, whole-second UTC ingest_ts.
    return midnight(p) + 1000LL * (3600 * pairValue(p + 11) +
                                   60 * pairValue(p + 14) + pairValue(p + 17));
}

std::vector<std::string_view> chunks(const std::vector<std::string_view>& files) {
    std::vector<std::string_view> result;
    constexpr size_t bytes = 2u << 20;
    for (auto file : files) {
        auto headerEnd = file.find('\n');
        if (headerEnd == file.npos) continue;
        file.remove_prefix(headerEnd + 1);
        while (file.size() > bytes) {
            auto end = file.find('\n', bytes);
            if (end == file.npos) break;
            result.push_back(file.substr(0, end + 1));
            file.remove_prefix(end + 1);
        }
        if (!file.empty()) result.push_back(file);
    }
    return result;
}

// No reallocation/copy of existing rows. Equal IDs route to the same
// partition, irrespective of which file or raw user supplied the copy.
constexpr size_t blockSize = 512;
template<class T> struct Blocks {
    std::vector<std::unique_ptr<T[]>> blocks;
    size_t size = 0;
    void add(const T& row) {
        const size_t offset = size % blockSize;
        if (!offset) blocks.push_back(std::make_unique_for_overwrite<T[]>(blockSize));
        blocks.back()[offset] = row;
        ++size;
    }
    template<class F> void each(F&& visit) {
        size_t remaining = size;
        for (auto& block : blocks) {
            const size_t count = std::min(remaining, blockSize);
            for (size_t i = 0; i < count; ++i) visit(block[i]);
            remaining -= count;
        }
    }
};

struct EventRow {
    Key id;
    int64_t ingest;
    Event data;
};

struct PersonCache {
    struct Entry { Key raw, canonical; };
    std::vector<Entry> entries = std::vector<Entry>(1u << 14);
    Key lookup(const Context& ctx, std::string_view raw) {
        const auto key = Key::from(raw);
        auto& entry = entries[key.hash() & (entries.size() - 1)];
        if (!(entry.raw == key)) entry = {key, ctx.person(key)};
        return entry.canonical;
    }
};

bool parseRow(std::string_view line, const Context& ctx, PersonCache& people, EventRow& row) {
    std::array<std::string_view,7> f;
    if (!csv(line, f) || f[0].empty()) return false;
    row.id = Key::from(f[0]);
    row.ingest = f[3].size() == 20 ? validIngest(f[3].data()) : timestamp(f[3]);
    row.data.ts = invalidTime;
    // An invalid earliest copy must survive dedup and suppress later copies.
    if (f[1].empty() || f[5].empty()) return true;
    auto ts = timestamp(f[2]);
    if (ts < ctx.start || ts >= ctx.end || row.ingest - dayMs > ts) return true;
    uint32_t price = 0, category = 0;
    if (f[4] == "purchase") {
        auto priceEnd = f[6].data() + f[6].size();
        auto [end, error] = std::from_chars(f[6].data(), priceEnd, price);
        if (error != std::errc{} || end != priceEnd || price == 0 || price > 10'000'000)
            return true;
        category = ctx.category(Key::from(f[5]));
    } else if (f[4] != "view" && f[4] != "cart") {
        return true;
    }
    row.data = {people.lookup(ctx, f[1]), ts, price, category};
    return true;
}

bool parseRow(std::string_view line, const Context& ctx, PersonCache& people, Click& row) {
    std::array<std::string_view,4> f;
    if (!csv(line, f) || f[0].empty() || f[1].empty()) return false;
    const auto ts = timestamp(f[3]);
    if (ts < ctx.start - 7 * dayMs || ts >= ctx.end) return false;
    const auto campaign = ctx.campaign(Key::from(f[2]));
    if (campaign == UINT32_MAX) return false;
    row = {people.lookup(ctx, f[1]), Key::from(f[0]), ts, campaign};
    return true;
}

template<class F>
void readLines(const std::vector<std::string_view>& files, unsigned workers, F visit) {
    const auto spans = chunks(files);
    if (spans.empty()) return;
    const unsigned readers = std::min<size_t>(spans.size(), workers);
    std::atomic<size_t> next = 0;
    parallel(readers, readers, [&](size_t reader) {
        PersonCache people;
        for (;;) {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= spans.size()) break;
            auto span = spans[i];
            while (!span.empty()) {
                const auto end = span.find('\n');
                const auto line = span.substr(0, end);
                span.remove_prefix(end == span.npos ? span.size() : end + 1);
                visit(reader, people, line);
            }
        }
    });
}

void ingestEvents(const Context& ctx, const std::vector<std::string_view>& files,
                  std::vector<Bucket>& out, unsigned workers) {
    std::vector<std::array<Blocks<EventRow>, partitions>> records(workers);
    readLines(files, workers, [&](size_t reader, PersonCache& people, std::string_view line) {
        EventRow row{};
        if (!parseRow(line, ctx, people, row)) return;
        records[reader][row.id.hash() & (partitions - 1)].add(row);
    });

    std::vector<std::vector<size_t>> offsets(partitions, std::vector<size_t>(out.size()));
    parallel(partitions, workers, [&](size_t shard) {
        size_t count = 0;
        for (auto& reader : records) count += reader[shard].size;
        if (!count) return;
        // Tags keep almost all probes in the small slot array. Every potential
        // match compares the complete ID, even if the 64-bit hashes collide.
        struct Slot { uint32_t tag, index; };
        std::vector<Slot> slots(std::bit_ceil(count + count / 3 + 1));
        const size_t mask = slots.size() - 1;
        std::vector<EventRow*> blocks;
        auto& sizes = offsets[shard];
        for (auto& reader : records) {
            auto& input = reader[shard];
            size_t rem = input.size;
            for (auto& block : input.blocks) {
                const size_t base = blocks.size() * blockSize;
                if (base + blockSize >= UINT32_MAX)
                    throw std::runtime_error("Too many rows in one ID partition");
                blocks.push_back(block.get());
                const size_t n = std::min(rem, blockSize);
                for (size_t i = 0; i < n; ++i) {
                    EventRow& row = block[i];
                    const Key id = row.id;
                    const uint64_t hash = id.hash();
                    const uint32_t tag = static_cast<uint32_t>(hash >> 32);
                    size_t pos = (hash >> partitionBits) & mask;
                    while (slots[pos].index) {
                        auto slot = slots[pos];
                        if (slot.tag == tag) {
                            const size_t index = slot.index - 1;
                            EventRow& kept = blocks[index / blockSize][index % blockSize];
                            if (kept.id == id) {
                                if (row.ingest < kept.ingest) {
                                    kept.data.ts = invalidTime;
                                    slots[pos].index = static_cast<uint32_t>(base + i + 1);
                                } else {
                                    row.data.ts = invalidTime;
                                }
                                break;
                            }
                        }
                        pos = (pos + 1) & mask;
                    }
                    if (!slots[pos].index)
                        slots[pos] = {tag, static_cast<uint32_t>(base + i + 1)};
                }
                rem -= n;
            }
        }
        for (auto& reader : records)
            reader[shard].each([&](const EventRow& row) {
                if (row.data.ts != invalidTime)
                    ++sizes[row.data.person.hash() & (out.size() - 1)];
            });
    });

    parallel(out.size(), workers, [&](size_t b) {
        auto& dst = out[b].events;
        size_t total = dst.size();
        for (auto& shard : offsets) {
            const auto start = total;
            total += shard[b];
            shard[b] = start;
        }
        dst.resize(total);
    });
    parallel(partitions, workers, [&](size_t shard) {
        auto& positions = offsets[shard];
        for (auto& reader : records) {
            auto& input = reader[shard];
            input.each([&](const EventRow& row) {
                if (row.data.ts == invalidTime) return;
                const auto b = row.data.person.hash() & (out.size() - 1);
                out[b].events[positions[b]++] = row.data;
            });
            input.blocks.clear();
        }
    });
}

void ingestClicks(const Context& ctx, const std::vector<std::string_view>& files,
                  std::vector<Bucket>& out, unsigned workers) {
    // All surviving copies of a click are identical. They enter and leave the
    // seven-day window together, so they change neither its winning (ts, id)
    // nor the set of campaigns in it. Routing straight to the person's bucket
    // therefore implements both output tables without a global click-ID pass.
    std::vector<std::vector<Blocks<Click>>> records(workers);
    for (auto& reader : records) reader.resize(out.size());
    readLines(files, workers, [&](size_t reader, PersonCache& people, std::string_view line) {
        Click click{};
        if (!parseRow(line, ctx, people, click)) return;
        records[reader][click.person.hash() & (out.size() - 1)].add(click);
    });
    parallel(out.size(), workers, [&](size_t b) {
        auto& clicks = out[b].clicks;
        size_t total = clicks.size();
        for (auto& reader : records) total += reader[b].size;
        clicks.reserve(total);
        for (auto& reader : records) {
            auto& input = reader[b];
            size_t remaining = input.size;
            for (auto& block : input.blocks) {
                const size_t n = std::min(remaining, blockSize);
                clicks.insert(clicks.end(), block.get(), block.get() + n);
                remaining -= n;
            }
            input.blocks.clear();
        }
    });
}
} // namespace

int64_t timestamp(std::string_view s) {
    const char* p = s.data();
    if ((s.size() != 20 && s.size() != 25) || p[4] != '-') {
        int64_t ms;
        if (s.empty() || !digit(*p)) return invalidTime;
        auto [end, ec] = std::from_chars(p, p + s.size(), ms);
        return ec == std::errc{} && end == p + s.size() ? ms : invalidTime;
    }
    const auto base = midnight(p);
    if (base == invalidTime || p[10] != 'T' || p[13] != ':' || p[16] != ':' ||
        !digit(p[11]) || !digit(p[12]) || !digit(p[14]) || !digit(p[15]) ||
        !digit(p[17]) || !digit(p[18])) return invalidTime;
    const int hour = pairValue(p+11), min = pairValue(p+14), sec = pairValue(p+17);
    if (hour > 23 || min > 59 || sec > 59) return invalidTime;
    int offset = 0;
    if (s.size() == 25) {
        if ((p[19] != '+' && p[19] != '-') || p[22] != ':' || !digit(p[20]) ||
            !digit(p[21]) || !digit(p[23]) || !digit(p[24])) return invalidTime;
        const int oh = pairValue(p+20), om = pairValue(p+23);
        if (oh > 23 || om > 59) return invalidTime;
        offset = (p[19] == '+' ? 1 : -1) * (60 * oh + om);
    } else if (p[19] != 'Z') {
        return invalidTime;
    }
    return base + 1000LL * (3600 * hour + 60 * (min - offset) + sec);
}

void ingest(const Context& ctx, const std::vector<std::string_view>& eventFiles,
            const std::vector<std::string_view>& clickFiles, std::vector<Bucket>& out,
            unsigned workers) {
    if (!std::has_single_bit(out.size())) throw std::logic_error("Bucket count must be a power of two");
    ingestEvents(ctx, eventFiles, out, workers);
    ingestClicks(ctx, clickFiles, out, workers);
}
