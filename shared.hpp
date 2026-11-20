#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <immintrin.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Short IDs live in the key itself. Long IDs refer to the immutable input mapping.
struct alignas(8) Key {
    std::array<unsigned char, 16> data{};
    static Key from(std::string_view value) {
        Key key;
        if (value.size() <= 15) {
            size_t n = value.size();
            const char* p = value.data();
            uint64_t head = 0, tail = uint64_t(n) << 56;
            if (n >= 8) {
                uint64_t end;
                std::memcpy(&head, p, 8);
                std::memcpy(&end, p + n - 8, 8);
                // The split shift also handles the eight-byte case without shifting by 64.
                tail |= (end >> (127 - 8*n)) >> 1;
            } else if (n >= 4) {
                uint32_t first, last;
                std::memcpy(&first, p, 4);
                std::memcpy(&last, p + n - 4, 4);
                head = uint64_t(first) | (uint64_t(last) << (8*(n-4)));
            } else if (n) {
                head = static_cast<unsigned char>(p[0]);
                if (n >= 2) {
                    uint16_t last;
                    std::memcpy(&last, p+n-2, 2);
                    head |= uint64_t(last) << (8*(n-2));
                }
            }
            std::memcpy(key.data.data(), &head, 8);
            std::memcpy(key.data.data()+8, &tail, 8);
        } else {
            const char* ptr = value.data();
            const uint64_t taggedSize = value.size() | (uint64_t{1} << 63);
            std::memcpy(key.data.data(), &ptr, 8);
            std::memcpy(key.data.data() + 8, &taggedSize, 8);
        }
        return key;
    }
    bool empty() const { return data[15] == 0; }
    bool isLong() const { return (data[15] & 128) != 0; }
    std::string_view view() const {
        if (!isLong()) return {reinterpret_cast<const char*>(data.data()), data[15]};
        const char* ptr;
        uint64_t size;
        std::memcpy(&ptr, data.data(), 8);
        std::memcpy(&size, data.data() + 8, 8);
        return {ptr, size & ~(uint64_t{1} << 63)};
    }
    uint64_t hash() const {
        if (isLong()) return std::hash<std::string_view>{}(view());
        uint64_t a, b;
        std::memcpy(&a, data.data(), 8);
        std::memcpy(&b, data.data() + 8, 8);
        const __uint128_t product = static_cast<__uint128_t>(a ^ 0xa0761d6478bd642fULL)
                                     * (b ^ 0xe7037ed1a0b428dbULL);
        return static_cast<uint64_t>(product) ^ static_cast<uint64_t>(product >> 64);
    }
    bool operator==(const Key& other) const {
        if (!isLong()) return data == other.data;
        return other.isLong() && view() == other.view();
    }
    bool operator<(const Key& other) const {
        if (!isLong() && !other.isLong()) return std::memcmp(data.data(), other.data.data(), 16) < 0;
        return view() < other.view();
    }
};
static_assert(sizeof(Key) == 16 && std::endian::native == std::endian::little);

// Count all quotes, then account for the only two legal positions around each field.
// SIMD searches for delimiters without treating CR, spaces or non-ASCII bytes specially.
template<size_t N> bool csvGeneric(std::string_view line, std::array<std::string_view, N>& fields) {
    std::array<size_t, N> ends;
    size_t count = 0, quotes = 0;
    const char* bytes = line.data();
    size_t at = 0;
    auto scan = [&](uint32_t commas, uint32_t quoted, size_t base) {
        quotes += std::popcount(quoted);
        while (commas) {
            if (count == N - 1) return false;
            ends[count++] = base + std::countr_zero(commas);
            commas &= commas - 1;
        }
        return true;
    };
    if (line.size() >= 32) {
        while (at < line.size()) {
            size_t base = std::min(at, line.size() - 32);
            __m256i block = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(bytes + base));
            uint32_t keep = UINT32_MAX << (at - base);
            uint32_t commas = _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, _mm256_set1_epi8(',')));
            uint32_t quoted = _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, _mm256_set1_epi8('"')));
            if (!scan(commas & keep, quoted & keep, base)) return false;
            at = base + 32;
        }
    } else {
        for (; at < line.size(); ++at) {
            if (bytes[at] == ',') {
                if (count == N - 1) return false;
                ends[count++] = at;
            } else if (bytes[at] == '"') ++quotes;
        }
    }
    if (count != N - 1) return false;
    ends[count] = line.size();
    size_t begin = 0, legalQuotes = 0;
    for (size_t i = 0; i < N; ++i) {
        size_t end = ends[i];
        bool wrapped = end - begin >= 2 && bytes[begin] == '"' && bytes[end - 1] == '"';
        size_t trim = static_cast<size_t>(wrapped);
        fields[i] = {bytes + begin + trim, end - begin - 2 * trim};
        legalQuotes += trim * 2;
        begin = end + 1;
    }
    return quotes == legalQuotes;
}

struct CsvMask {
    uint32_t comma, quote;
};
inline CsvMask classify32(const char* p) {
    auto bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    return {static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(',')))),
            static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('"'))))};
}

template<size_t N, bool Long>
bool csvPacked(std::string_view line, std::array<std::string_view,N>& fields) {
    const size_t size = line.size();
    const char* p = line.data();
    auto first = classify32(p);
    auto last = classify32(p + size - 32);
    uint64_t commas, quotes, upperCommas = 0, upperQuotes = 0;
    if constexpr (Long) {
        auto middle = classify32(p + 32);
        commas = first.comma | (uint64_t(middle.comma) << 32);
        quotes = first.quote | (uint64_t(middle.quote) << 32);
        upperCommas = last.comma >> (96-size);
        upperQuotes = last.quote >> (96-size);
    } else {
        commas = first.comma | (uint64_t(last.comma) << (size-32));
        quotes = first.quote | (uint64_t(last.quote) << (size-32));
    }
    if (std::popcount(commas) + std::popcount(upperCommas) != N - 1) return false;

    // Compress the quotes at possible field boundaries to one bit per field.
    // The first empty field can have no closing byte. An open quote with no
    // distinct matching close, or any quote in the interior, invalidates the row.
    uint64_t starts = (commas<<1) | 1, ends = (commas>>1);
    unsigned wrapped, closed;
    if constexpr (Long) {
        ends |= upperCommas << 63;
        uint64_t upperStarts = (upperCommas<<1) | (commas>>63);
        uint64_t upperEnds = (upperCommas>>1) | (uint64_t{1}<<(size-65));
        if (upperQuotes & ~(upperStarts ^ upperEnds)) return false;
        wrapped = _pext_u64(quotes, starts) | (_pext_u64(upperQuotes, upperStarts) << std::popcount(starts));
        closed = _pext_u64(quotes, ends) | (_pext_u64(upperQuotes, upperEnds) << std::popcount(ends));
    } else {
        ends |= uint64_t{1} << (size-1);
        wrapped = _pext_u64(quotes, starts);
        closed = _pext_u64(quotes, ends);
    }
    closed <<= (commas & 1);
    // Each quote must occupy exactly one boundary. A single-byte field
    // containing only a quote is simultaneously a start and an end.
    if (wrapped != closed || (quotes & ~(starts ^ ends))) return false;

    size_t begin = 0;
    for (size_t i = 0; i < N; ++i) {
        size_t end = size;
        if (i != N-1) {
            if (Long && !commas) {
                end = 64 + std::countr_zero(upperCommas);
                upperCommas &= upperCommas - 1;
            } else {
                end = std::countr_zero(commas);
                commas &= commas - 1;
            }
        }
        size_t trim = (wrapped >> i) & 1;
        fields[i] = {p + begin + trim, end - begin - 2*trim};
        begin = end + 1;
    }
    return true;
}

template<size_t N> bool csv(std::string_view line, std::array<std::string_view,N>& fields) {
    if constexpr (N == 4 || N == 7) {
        if (line.size() >= 32 && line.size() <= 64) return csvPacked<N,false>(line,fields);
        if (line.size() > 64 && line.size() <= 96) return csvPacked<N,true>(line,fields);
    }
    return csvGeneric(line,fields);
}

template<class F> void parallel(size_t count, unsigned workers, F&& fn) {
    if (count == 0) return;
    workers = static_cast<unsigned>(std::min<size_t>(std::max(1u, workers), count));
    if (workers == 1) {
        for (size_t i = 0; i < count; ++i) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::mutex errorLock;
    std::exception_ptr error;
    auto work = [&] {
        try {
            for (;;) {
                auto i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= count) break;
                fn(i);
            }
        } catch (...) {
            next.store(count, std::memory_order_relaxed);
            std::lock_guard guard(errorLock);
            if (!error) error = std::current_exception();
        }
    };
    std::vector<std::jthread> threads;
    for (unsigned i = 1; i < workers; ++i) threads.emplace_back(work);
    work();
    threads.clear();
    if (error) std::rethrow_exception(error);
}

class Index {
    struct Slot { Key key; uint32_t value = UINT32_MAX; };
    std::vector<Slot> slots;
    size_t occupied = 0;
    size_t locate(const Key& key) const {
        size_t mask = slots.size() - 1, pos = key.hash() & mask;
        while (slots[pos].value != UINT32_MAX && !(slots[pos].key == key)) pos = (pos + 1) & mask;
        return pos;
    }
public:
    void reserve(size_t n);
    uint32_t insert(Key key, uint32_t value);
    uint32_t find(const Key& key) const {
        return slots.empty() ? UINT32_MAX : slots[locate(key)].value;
    }
};

// Only aliases occupy the immutable lookup table. Canonical and unlinked IDs
// both fall through to the input key.
struct Identities {
    static constexpr unsigned shardBits = 7;
    static constexpr size_t shards = 1u << shardBits;
    struct Alias { Key raw, name; };
    std::vector<Alias> slots;
    size_t shardSize = 0;

    static size_t shard(uint64_t hash) { return hash >> (64 - shardBits); }
    Key person(const Key& raw) const {
        if (slots.empty()) return raw;
        uint64_t hash = raw.hash();
        const Alias* table = slots.data() + shard(hash) * shardSize;
        size_t mask = shardSize - 1, pos = hash & mask;
        while (!table[pos].raw.empty()) {
            if (table[pos].raw == raw) return table[pos].name;
            pos = (pos + 1) & mask;
        }
        return raw;
    }
};

struct Context {
    int64_t start, end;
    Identities identities;
    Index products, campaigns;
    std::vector<Key> categories, campaignNames, channels;
    uint32_t unknown;
    Key person(Key raw) const { return identities.person(raw); }
    uint32_t category(Key product) const {
        uint32_t value = products.find(product);
        return value == UINT32_MAX ? unknown : value;
    }
    uint32_t campaign(Key id) const { return campaigns.find(id); }
};

struct Event { Key person; int64_t ts; uint32_t price, category; };
struct Click { Key person, id; int64_t ts; uint32_t campaign; };
struct Bucket { std::vector<Event> events; std::vector<Click> clicks; };
struct UserRow { Key person; int32_t day; uint32_t sessions, events; int64_t revenue; uint32_t top; };
struct CampaignStats { int64_t sessions=0, purchasing=0, revenue=0, assists=0; };
struct Result {
    std::vector<UserRow> users;
    std::unordered_map<int32_t, std::vector<CampaignStats>> byDay;
};

void linkPeople(const std::vector<std::string_view>&, Context&, unsigned);
int64_t timestamp(std::string_view value);
void ingest(const Context&, const std::vector<std::string_view>&, const std::vector<std::string_view>&,
            std::vector<Bucket>&, unsigned);
void aggregate(Bucket&, Result&, size_t categoryCount, size_t campaignCount);
void writeOutputs(const std::filesystem::path&, const Context&, const std::vector<Result>&, unsigned);
