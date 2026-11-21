#include <sys/mman.h>
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
#include <tuple>
#include <vector>
#include <chrono>
#include <iostream>
// Stage timings for SHOP_PROFILE=1, as in main.cpp.
static void lap(const char* name) {
    static const bool profiling = std::getenv("SHOP_PROFILE") != nullptr;
    static auto previous = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (profiling) std::cerr << "  " << name << " " << std::chrono::duration<double>(now - previous).count() << " s\n";
    previous = now;
}

namespace {
constexpr int64_t invalidTime = INT64_MIN;
constexpr int64_t dayMs = 86'400'000;
constexpr unsigned partitionBits = 8;
constexpr size_t partitions = 1u << partitionBits;

int pairValue(const char* p) {
    return 10 * (p[0] - '0') + p[1] - '0';
}

bool digit(char c) { return static_cast<unsigned>(c - '0') < 10; }

// The cache key contains every date digit. Its two omitted separators are
// guaranteed on ingest timestamps and checked by the public timestamp parser.
[[gnu::noinline]] int64_t calendarMidnight(const char* p) {
    if (!digit(p[0]) || !digit(p[1]) ||
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
    return days * dayMs;
}

inline int64_t midnight(const char* p) {
    struct Entry { uint64_t digits; int64_t ms = invalidTime; };
    static thread_local std::array<Entry,64> cache{};
    const auto data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const auto packed = _mm_shuffle_epi8(data,
        _mm_setr_epi8(0,1,2,3,5,6,8,9,-1,-1,-1,-1,-1,-1,-1,-1));
    const uint64_t digits = _mm_cvtsi128_si64(packed);
    const unsigned slot = ((uint8_t(p[8]) & 3) << 4) | (uint8_t(p[9]) & 15);
    auto& entry = cache[slot];
    if (entry.digits != digits) entry = {digits, calendarMidnight(p)};
    return entry.ms;
}

// Convert all three clock pairs in one vector. For untrusted event times
// validate the digit ranges and literal colons before returning the value.
template<bool Check> inline int clockSeconds(const char* p) {
    const auto raw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p));
    const auto digits = _mm_sub_epi8(raw, _mm_cvtsi64_si128(0x30303a30303a3030LL));
    if constexpr (Check) {
        const auto excess = _mm_subs_epu8(digits, _mm_cvtsi64_si128(0x0905000905000902LL));
        if (!_mm_testz_si128(excess,excess) || (p[0]=='2' && p[1]>'3')) return -1;
    }
    const auto fields = _mm_shuffle_epi8(digits,
        _mm_setr_epi8(0,1,3,4,6,7,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1));
    const auto pairs = _mm_maddubs_epi16(fields, _mm_set1_epi16(0x010a));
    const auto weighted = _mm_madd_epi16(pairs, _mm_setr_epi16(3600,60,1,0,0,0,0,0));
    return _mm_cvtsi128_si32(_mm_add_epi32(weighted, _mm_srli_si128(weighted,4)));
}

inline int64_t validIngest(const char* p) {
    return midnight(p) + 1000LL * clockSeconds<false>(p+11);
}

// A valid UTC arrival can only be late if its calendar date follows the
// event. Keep the six clock digits rather than converting every arrival into
// milliseconds immediately. Only next-day arrivals and actual duplicate
// candidates need their time of day decoded.
inline uint32_t packedClock(const char* clock) {
    uint64_t bytes;
    std::memcpy(&bytes, clock, sizeof(bytes));
    return _pext_u64(__builtin_bswap64(bytes), 0x0f0f000f0f000f0fULL);
}

inline int decodeClock(uint32_t bcd) {
    const unsigned hours = 10 * (bcd >> 20) + ((bcd >> 16) & 15);
    const unsigned minutes = 10 * ((bcd >> 12) & 15) + ((bcd >> 8) & 15);
    const unsigned seconds = 10 * ((bcd >> 4) & 15) + (bcd & 15);
    return 3600 * hours + 60 * minutes + seconds;
}

inline int64_t arrivalMillis(int64_t packed) {
    // The low clock contribution (0x000000..0x235959) is less than a day.
    int64_t clock = packed % dayMs;
    if (clock < 0) clock += dayMs;
    return packed - clock + 1000LL * decodeClock(static_cast<uint32_t>(clock));
}

// Parse 13 decimal digits with two bounded overlapping loads. Combining digit
// pairs, then four-digit groups, leaves two eight-digit chunks for the scalar sum.
int64_t decimal13(const char* p) {
#if defined(__AVX512BW__) && defined(__AVX512VL__)
    // A masked load reads exactly the input field, including at page end.
    const auto asciiZero = _mm_set1_epi8('0');
    const auto raw = _mm_mask_loadu_epi8(asciiZero, 0x1fff, p);
    const auto digits = _mm_sub_epi8(raw, asciiZero);
    if (_mm_cmpgt_epu8_mask(digits, _mm_set1_epi8(9))) return invalidTime;
    const auto padded = _mm_slli_si128(digits, 3);
#else
    const auto first = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p));
    const auto last = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p+5));
    const auto digits = _mm_sub_epi8(_mm_unpacklo_epi64(first,last), _mm_set1_epi8('0'));
    const auto invalid = _mm_subs_epu8(digits, _mm_set1_epi8(9));
    if (!_mm_testz_si128(invalid,invalid)) return invalidTime;
    const auto padded = _mm_shuffle_epi8(digits,
        _mm_setr_epi8(-1,-1,-1,0,1,2,3,4,5,6,7,11,12,13,14,15));
#endif
    const auto pairs = _mm_maddubs_epi16(padded, _mm_set1_epi16(0x010a));
    const auto quads = _mm_madd_epi16(pairs, _mm_set1_epi32(0x00010064));
    const auto chunks = _mm_madd_epi16(_mm_packs_epi32(quads,quads), _mm_set1_epi32(0x00012710));
    return int64_t(_mm_cvtsi128_si32(chunks)) * 100'000'000 + _mm_extract_epi32(chunks,1);
}

inline int utcOffsetMinutes(const char* p) {
    struct Offset { uint64_t key; int minutes = 2000; };
    static thread_local std::array<Offset,64> cache{};
    uint64_t key;
    std::memcpy(&key,p-2,8); // p is the sign at byte 19 of a 25-byte timestamp
    key >>= 16;
    const unsigned slot = (((uint8_t(p[0]) & 2) << 4) ^ (uint8_t(p[1]) << 2)
                         ^ uint8_t(p[2]) ^ uint8_t(p[4])) & 63;
    auto& entry = cache[slot];
    if (entry.key != key) {
        int minutes = 2000;
        if ((p[0]=='+' || p[0]=='-') && p[3]==':' && digit(p[1]) && digit(p[2]) &&
            digit(p[4]) && digit(p[5])) {
            const int h=pairValue(p+1), m=pairValue(p+4);
            if (h<24 && m<60) minutes=(p[0]=='+' ? 1 : -1)*(60*h+m);
        }
        entry = {key,minutes};
    }
    return entry.minutes;
}

struct InputChunk {
    std::string_view file;
    size_t begin, end;
};

// Boundaries depend only on file sizes. Readers fault in their own chunks
// and consume every line whose first byte belongs to their range.
std::vector<InputChunk> chunks(const std::vector<std::string_view>& files) {
    std::vector<InputChunk> result;
    constexpr size_t bytes = 2u << 20;
    for (auto file : files)
        for (size_t begin = 0; begin < file.size(); begin += bytes)
            result.push_back({file, begin, std::min(file.size(), begin + bytes)});
    return result;
}

struct StructuralBlock {
    uint64_t commas, quotes, newlines;
};

inline StructuralBlock classifyBlock(const char* p) {
#ifdef __AVX512BW__
    const __m512i bytes = _mm512_loadu_si512(p);
    return {_mm512_cmpeq_epi8_mask(bytes, _mm512_set1_epi8(',')),
            _mm512_cmpeq_epi8_mask(bytes, _mm512_set1_epi8('"')),
            _mm512_cmpeq_epi8_mask(bytes, _mm512_set1_epi8('\n'))};
#else
    const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 32));
    auto mask = [&](char ch) -> uint64_t {
        __m256i match = _mm256_set1_epi8(ch);
        uint32_t lo = _mm256_movemask_epi8(_mm256_cmpeq_epi8(a, match));
        uint32_t hi = _mm256_movemask_epi8(_mm256_cmpeq_epi8(b, match));
        return lo | (uint64_t{hi} << 32);
    };
    return {mask(','), mask('"'), mask('\n')};
#endif
}

template<size_t N, class F> void chunkFields(const InputChunk& chunk, F&& visit) {
    const char* bytes = chunk.file.data();
    const size_t fileEnd = chunk.file.size();
    size_t pos = chunk.begin;
    if (pos == 0 || bytes[pos - 1] != '\n') {
        // At offset zero skip the header. Other readers skip the previous
        // chunk's last line, searching at most as far as their own boundary.
        auto prefix = chunk.file.substr(pos, chunk.end - pos);
        size_t newline = prefix.find('\n');
        if (newline == prefix.npos) return;
        pos += newline + 1;
    }
    // Start each vector at the current row: its comma and quote positions
    // are immediately usable by the field parser. No alignment or carrying
    // masks across blocks is needed.
    while (pos < chunk.end) {
        const char* row = bytes + pos;
        const size_t remaining = fileEnd - pos;
        std::array<std::string_view,N> fields;
        if (remaining >= 128) {
            auto first = classifyBlock(row);
            if (first.newlines) {
                const unsigned n = std::countr_zero(first.newlines);
                const std::string_view line(row, n);
                pos += n + 1;
                if (n && csvFromMasks<N,false>(line, fields, _bzhi_u64(first.commas, n),
                                               _bzhi_u64(first.quotes, n), 0, 0))
                    visit(line, fields);
                continue;
            }
            auto second = classifyBlock(row + 64);
            if (second.newlines) {
                const unsigned n = std::countr_zero(second.newlines);
                const std::string_view line(row, 64 + n);
                pos += line.size() + 1;
                if (n ? csvFromMasks<N,true>(line, fields, first.commas, first.quotes,
                                             _bzhi_u64(second.commas,n), _bzhi_u64(second.quotes,n))
                      : csvFromMasks<N,false>(line, fields, first.commas, first.quotes, 0, 0))
                    visit(line, fields);
                continue;
            }
        }
        // No bounded vector load past the mapping. A long record still belongs
        // to the chunk containing its first byte.
        const size_t next = chunk.file.find('\n', pos + (remaining >= 128 ? 128 : 0));
        const size_t end = next == chunk.file.npos ? fileEnd : next;
        auto line = std::string_view(row, end - pos);
        if (csv(line, fields)) visit(line, fields);
        pos = end + 1;
    }
}

// ---- Block storage -------------------------------------------------------
// Every row stream is a chain of equally sized, cache-line-aligned blocks cut
// from large per-thread extents. Nothing is freed: the process exits first.
constexpr size_t blockBytes = 61440;                 // 960 cache lines
constexpr size_t extentBytes = 512 * blockBytes;     // 30 MB, a multiple of 2 MB
// Blocks whose rows have been consumed are handed to later writers. They
// are written with non-temporal stores, so a cold block costs nothing extra
// and the kernel has no fresh page to supply.
class Recycled {
    std::mutex lock;
    std::vector<void*> blocks;
    std::atomic<size_t> available{0};
public:
    void give(const std::vector<void*>& more) {
        if (more.empty()) return;
        std::lock_guard guard(lock);
        blocks.insert(blocks.end(), more.begin(), more.end());
        available.store(blocks.size(), std::memory_order_relaxed);
    }
    void* take() {
        if (!available.load(std::memory_order_relaxed)) return nullptr;
        std::lock_guard guard(lock);
        if (blocks.empty()) return nullptr;
        void* block = blocks.back();
        blocks.pop_back();
        available.store(blocks.size(), std::memory_order_relaxed);
        return block;
    }
} recycled;

class Slab {
    char* next = nullptr;
    char* end = nullptr;
public:
    void* take() {
        if (void* block = recycled.take()) return block;
        if (next == end) {
            constexpr size_t huge = 2u << 20;
            void* raw = mmap(nullptr, extentBytes + huge, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (raw == MAP_FAILED) throw std::bad_alloc();
            next = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(raw) + huge - 1) & ~(huge - 1));
            end = next + extentBytes;
            madvise(next, extentBytes, MADV_HUGEPAGE);
        }
        void* block = next;
        next += blockBytes;
        return block;
    }
};

template<class T> constexpr size_t rowsPerBlock = blockBytes / sizeof(T);

// Radix scatter with software write combining: rows collect in a small
// per-partition buffer and leave as whole cache lines with non-temporal
// stores, so the destination is never read and the open set stays in L2.
template<class T, size_t Group> class Scatter {
    static_assert(std::is_trivially_copyable_v<T> && blockBytes % sizeof(T) == 0);
    static_assert((Group * sizeof(T)) % 64 == 0 && rowsPerBlock<T> % Group == 0);
    struct alignas(64) Buffer { T rows[Group]; };
    Slab& slab;
    std::vector<Buffer> buffers;
    T* destination(size_t part, size_t start) {
        const size_t offset = start % rowsPerBlock<T>;
        if (!offset) blocks[part].push_back(static_cast<T*>(slab.take()));
        return blocks[part].back() + offset;
    }
public:
    std::vector<uint32_t> counts;
    std::vector<std::vector<T*>> blocks;
    Scatter(Slab& slab, size_t parts) : slab(slab), buffers(parts), counts(parts), blocks(parts) {}

    // Returns the row's position in its partition's chain.
    uint32_t add(size_t part, const T& row) {
        const uint32_t index = counts[part]++;
        const size_t slot = index % Group;
        Buffer& buffer = buffers[part];
        buffer.rows[slot] = row;
        if (slot == Group - 1) {
#ifdef __AVX512F__
            auto* from = reinterpret_cast<const __m512i*>(&buffer);
            auto* to = reinterpret_cast<__m512i*>(destination(part, index - slot));
            for (size_t i = 0; i < Group * sizeof(T) / 64; ++i)
                _mm512_stream_si512(to + i, _mm512_load_si512(from + i));
#else
            auto* from = reinterpret_cast<const __m256i*>(&buffer);
            auto* to = reinterpret_cast<__m256i*>(destination(part, index - slot));
            for (size_t i = 0; i < Group * sizeof(T) / 32; ++i)
                _mm256_stream_si256(to + i, _mm256_load_si256(from + i));
#endif
        }
        return index;
    }
    void finish() {
        for (size_t part = 0; part < counts.size(); ++part) {
            const size_t rest = counts[part] % Group;
            if (rest) std::memcpy(destination(part, counts[part] - rest), &buffers[part], rest * sizeof(T));
        }
        _mm_sfence();
        std::vector<Buffer>().swap(buffers);
    }
    void release(size_t part, std::vector<void*>& to) {
        to.insert(to.end(), blocks[part].begin(), blocks[part].end());
    }
    T& at(size_t part, size_t index) { return blocks[part][index / rowsPerBlock<T>][index % rowsPerBlock<T>]; }
    template<class F> void pieces(size_t part, F&& visit) {
        size_t remaining = counts[part];
        for (T* block : blocks[part]) {
            const size_t count = std::min(remaining, rowsPerBlock<T>);
            visit(block, count);
            remaining -= count;
        }
    }
};

// ---- Rows ----------------------------------------------------------------
// A parsed event waits under its raw user ID until identities are known.
// Non-purchases can reuse their zero price and category payload for exact dedup.
struct StagedEvent {
    Key user;
    int64_t ts;
    uint64_t payload, origin;
    static constexpr uint64_t inlineId = uint64_t{1} << 63;
    static constexpr uint64_t timeMask = (uint64_t{1} << 49) - 1;

    void remember(const Key& id, int64_t ingest) {
        if (payload || ingest < -(int64_t{1} << 48) || ingest >= (int64_t{1} << 48)) return;
        origin = id.bits;
        payload = inlineId | (static_cast<uint64_t>(ingest) & timeMask);
    }
    uint32_t price() const { return payload & inlineId ? 0 : static_cast<uint32_t>(payload >> 32); }
    uint32_t category() const { return payload & inlineId ? 0 : static_cast<uint32_t>(payload); }
};
static_assert(sizeof(StagedEvent) == 32 && sizeof(Event) == 16 && sizeof(Click) == 32);

// Dedup record: 23 hash bits, then either the line's offset or the staged row.
constexpr unsigned refBits = 40, indexBits = 26, partBitsMax = 10, readerBitsMax = 4;
constexpr uint64_t stagedFlag = uint64_t{1} << refBits;
constexpr unsigned tagShift = refBits + 1;
static_assert(indexBits + partBitsMax + readerBitsMax == refBits);

struct Reader {
    Slab slab;
    Scatter<StagedEvent, 8> events;
    Scatter<Click, 4> clicks;
    Scatter<uint64_t, 8> ids;
    Reader(size_t parts, size_t idShards) : events(slab, parts), clicks(slab, parts), ids(slab, idShards) {}
};
struct Router {
    Slab slab;
    Scatter<Event, 8> events;
    Scatter<Click, 4> clicks;
    explicit Router(size_t buckets) : events(slab, buckets), clicks(slab, buckets) {}
};

struct ParsedEvent {
    Key id;
    bool valid = false;
    StagedEvent row;
    std::string_view ingest;
};

// Malformed lines and empty IDs never take part in deduplication. Every other
// copy does, valid or not: an invalid earliest copy suppresses later ones.
inline bool parseEvent(const std::array<std::string_view,7>& f, const Context& ctx, ParsedEvent& out) {
    if (f[0].empty()) return false;
    out.id = Key::from(f[0]);
    out.ingest = f[3];
    out.valid = false;
    if (f[1].empty() || f[5].empty()) return true;
    const auto ts = timestamp(f[2]);
    if (ts < ctx.start || ts >= ctx.end) return true;
    int64_t ingest = 0;
    if (f[3].size() == 20) {
        const auto base = midnight(f[3].data());
        const auto clock = packedClock(f[3].data() + 11);
        if (base > ts && base - dayMs + 1000LL * decodeClock(clock) > ts) return true;
        ingest = base + clock;
    } else {
        // Outside the canonical UTC fast path, dedup reads this row's source.
        const auto arrival = timestamp(f[3]);
        if (arrival > ts && static_cast<uint64_t>(arrival) - static_cast<uint64_t>(ts) > dayMs) return true;
    }
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
    out.row = {Key::from(f[1]), ts, (uint64_t{price} << 32) | category, 0};
    if (f[3].size() == 20) out.row.remember(out.id, ingest);
    out.valid = true;
    return true;
}
inline bool parseClick(const std::array<std::string_view,4>& f, const Context& ctx, Click& row) {
    if (f[0].empty() || f[1].empty()) return false;
    const auto ts = timestamp(f[3]);
    if (ts < ctx.start - 7 * dayMs || ts >= ctx.end) return false;
    const auto campaign = ctx.campaign(Key::from(f[2]));
    if (campaign == UINT32_MAX) return false;
    row = {Key::from(f[1]), Key::from(f[0]), ts, campaign};
    return true;
}

struct Source {
    std::vector<InputChunk> spans;
    std::vector<uint64_t> starts;            // global offset of each event file
    std::vector<std::string_view> files;
    size_t eventSpans = 0;
    // The line's first byte and the end of its file.
    std::pair<const char*, const char*> locate(uint64_t offset) const {
        const size_t f = std::upper_bound(starts.begin(), starts.end(), offset) - starts.begin() - 1;
        return {files[f].data() + (offset - starts[f]), files[f].data() + files[f].size()};
    }
};

// One ID shard: find the hash tags that occur more than once, then settle
// those few rows exactly from their source lines.
void deduplicate(size_t shard, const Context& ctx, const Source& source,
                 std::vector<std::unique_ptr<Reader>>& readers) {
    size_t count = 0;
    for (auto& r : readers) count += r->ids.counts[shard];
    if (count < 2) return;
    // The compact filters conservatively nominate collisions. Sorting by the
    // full tag below separates unrelated IDs before any source-line reread.
    const size_t bits = std::min<size_t>(1u << (64 - tagShift),
        std::max<size_t>(64, std::bit_ceil(count) * 16));
    const size_t mask = bits - 1;
    struct Word { uint64_t seen, repeated; };
    static thread_local std::vector<Word> filter;
    filter.assign(bits / 64, Word{});
    for (auto& r : readers)
        r->ids.pieces(shard, [&](const uint64_t* records, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                const size_t bit = (records[i] >> tagShift) & mask;
                const uint64_t flag = uint64_t{1} << (bit % 64);
                Word& word = filter[bit / 64];
                word.repeated |= word.seen & flag;
                word.seen |= flag;
            }
        });
    static thread_local std::vector<uint64_t> suspects, scratch;
    suspects.clear();
    for (auto& r : readers)
        r->ids.pieces(shard, [&](const uint64_t* records, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                const size_t bit = (records[i] >> tagShift) & mask;
                if ((filter[bit / 64].repeated >> (bit % 64)) & 1)
                    suspects.push_back(records[i]);
            }
        });
    if (suspects.size() < 512) {
        std::sort(suspects.begin(), suspects.end());
    } else {
        // Only the high 23 tag bits group potential copies. Two stable radix
        // passes discard the unnecessary ordering by source or staged offset.
        std::array<size_t,2048> low{};
        std::array<size_t,4096> high{};
        for (uint64_t x : suspects) {
            ++low[(x >> tagShift) & 2047];
            ++high[x >> (tagShift + 11)];
        }
        auto offsets = [](auto& counts) {
            size_t offset = 0;
            for (size_t& n : counts) {
                size_t previous = offset;
                offset += n;
                n = previous;
            }
        };
        offsets(low);
        offsets(high);
        scratch.resize(suspects.size());
        for (uint64_t x : suspects)
            scratch[low[(x >> tagShift) & 2047]++] = x;
        for (uint64_t x : scratch)
            suspects[high[x >> (tagShift + 11)]++] = x;
    }
    size_t kept = 0;
    for (size_t begin = 0; begin < suspects.size();) {
        size_t end = begin + 1;
        while (end < suspects.size() && (suspects[end] >> tagShift) == (suspects[begin] >> tagShift)) ++end;
        if (end - begin > 1) {
            for (size_t i = begin; i < end; ++i) suspects[kept++] = suspects[i];
        }
        begin = end;
    }
    suspects.resize(kept);
    if (suspects.empty()) return;

    // The suspects' staged rows and source lines are scattered through memory.
    // Resolve them in waves so those cache misses overlap.
    struct Copy { Key id; int64_t ingest; StagedEvent* staged; };
    static thread_local std::vector<Copy> copies;
    copies.resize(suspects.size());
    // Canonical UTC arrival keys are exactly ordered already. If a caller
    // supplies another timestamp form, translate this shard into milliseconds.
    std::vector<std::pair<size_t,int64_t>> nonCanonical;
    constexpr size_t wave = 64;
    for (size_t base = 0; base < suspects.size(); base += wave) {
        std::array<const char*, wave> lines{}, limits;
        const size_t stop = std::min(suspects.size(), base + wave);
        for (size_t i = base; i < stop; ++i) {
            const uint64_t record = suspects[i];
            const uint64_t ref = record & (stagedFlag - 1);
            copies[i].staged = nullptr;
            if (record & stagedFlag) {
                const size_t reader = ref >> (indexBits + partBitsMax);
                const size_t part = (ref >> indexBits) & ((1u << partBitsMax) - 1);
                copies[i].staged = &readers[reader]->events.at(part, ref & ((1u << indexBits) - 1));
                __builtin_prefetch(copies[i].staged);
                __builtin_prefetch(reinterpret_cast<const char*>(copies[i].staged) + sizeof(StagedEvent)-1);
            } else {
                std::tie(lines[i-base], limits[i-base]) = source.locate(ref);
                __builtin_prefetch(lines[i-base]);
                __builtin_prefetch(lines[i-base] + 64);
            }
        }
        for (size_t i = base; i < stop; ++i)
            if (copies[i].staged) {
                const StagedEvent& row = *copies[i].staged;
                if (row.payload & StagedEvent::inlineId) {
                    copies[i].id = Key{row.origin};
                    copies[i].ingest = static_cast<int64_t>(row.payload << 15) >> 15;
                    continue;
                }
                std::tie(lines[i-base], limits[i-base]) = source.locate(row.origin);
                __builtin_prefetch(lines[i-base]);
                __builtin_prefetch(lines[i-base] + 64);
            }
        for (size_t i = base; i < stop; ++i) {
            std::array<std::string_view,7> f;
            const char* start = lines[i-base];
            if (!start) continue;
            const void* newline = std::memchr(start, '\n', limits[i-base] - start);
            const char* stop = newline ? static_cast<const char*>(newline) : limits[i-base];
            if (!csv(std::string_view(start, stop - start), f)) throw std::logic_error("Source line changed");
            copies[i].id = Key::from(f[0]);
            if (f[3].size() == 20) {
                copies[i].ingest = midnight(f[3].data()) + packedClock(f[3].data()+11);
            } else {
                copies[i].ingest = 0;
                nonCanonical.emplace_back(i,timestamp(f[3]));
            }
        }
    }
    if (!nonCanonical.empty()) {
        for (auto& copy : copies) copy.ingest = arrivalMillis(copy.ingest);
        for (auto [index, millis] : nonCanonical) copies[index].ingest = millis;
    }
    // Within each exact ID the earliest ingest survives. Copies that tie are identical.
    auto before = [](const Copy& a, const Copy& b) {
        return a.id != b.id ? a.id < b.id : a.ingest < b.ingest;
    };
    for (size_t begin = 0; begin < suspects.size();) {
        size_t end = begin + 1;
        while (end < suspects.size() && suspects[end] >> tagShift == suspects[begin] >> tagShift) ++end;
        if (end == begin + 2) {
            // Most repeated IDs have just two copies. A differing full ID
            // is a tag collision and leaves both rows untouched.
            auto& a = copies[begin];
            auto& b = copies[begin + 1];
            if (a.id == b.id) {
                StagedEvent* loser = a.ingest <= b.ingest ? b.staged : a.staged;
                if (loser) loser->ts = invalidTime;
            }
            begin = end;
            continue;
        }
        if (end - begin <= 16) {
            for (size_t i = begin + 1; i < end; ++i) {
                Copy value = copies[i];
                size_t hole = i;
                for (; hole > begin && before(value, copies[hole - 1]); --hole) copies[hole] = copies[hole - 1];
                copies[hole] = value;
            }
        } else {
            std::stable_sort(copies.begin() + begin, copies.begin() + end, before);
        }
        for (size_t i = begin + 1; i < end; ++i)
            if (copies[i].id == copies[i - 1].id && copies[i].staged)
                copies[i].staged->ts = invalidTime;
        begin = end;
    }
}
} // namespace

int64_t timestamp(std::string_view s) {
    const char* p = s.data();
    if (s.size() == 13) return decimal13(p);
    if ((s.size() != 20 && s.size() != 25) || p[4] != '-') {
        int64_t ms;
        if (s.empty() || !digit(*p)) return invalidTime;
        auto [end, ec] = std::from_chars(p, p + s.size(), ms);
        return ec == std::errc{} && end == p + s.size() ? ms : invalidTime;
    }
    if (p[7]!='-' || p[10]!='T') return invalidTime;
    const auto base = midnight(p);
    const int seconds = clockSeconds<true>(p+11);
    if (base==invalidTime || seconds<0) return invalidTime;
    int offset = 0;
    if (s.size() == 25) {
        offset = utcOffsetMinutes(p+19);
        if (offset == 2000) return invalidTime;
    } else if (p[19] != 'Z') {
        return invalidTime;
    }
    return base + 1000LL * (seconds - 60 * offset);
}


void ingest(const Context& ctx, const std::vector<std::string_view>& eventFiles,
            const std::vector<std::string_view>& clickFiles, std::vector<Bucket>& out,
            unsigned workers, const std::function<void()>& awaitIdentities) {
    if (!std::has_single_bit(out.size())) throw std::logic_error("Bucket count must be a power of two");
    lap("start");
    workers = std::min(workers, 1u << readerBitsMax);

    Source source;
    source.files = eventFiles;
    size_t bytes = 0;
    for (auto file : eventFiles) {
        source.starts.push_back(bytes);
        bytes += file.size();
    }
    if (bytes >> refBits) throw std::runtime_error("Event input too large");
    source.spans = chunks(eventFiles);
    source.eventSpans = source.spans.size();
    for (auto& span : chunks(clickFiles)) source.spans.push_back(span);
    if (source.spans.empty()) { awaitIdentities(); return; }
    std::vector<uint64_t> spanStart(source.eventSpans);
    for (size_t i = 0, f = 0; i < source.eventSpans; ++i) {
        while (source.files[f].data() != source.spans[i].file.data() ||
               source.files[f].size() != source.spans[i].file.size()) ++f;
        spanStart[i] = source.starts[f];
    }

    // Staging partitions follow the top hash bits, like the identity table, so
    // one partition later probes one slice of it. ID shards use the low bits.
    size_t clickBytes = 0;
    for (auto file : clickFiles) clickBytes += file.size();
    const unsigned partBits = std::clamp<int>(std::bit_width((bytes + clickBytes) >> 23), 0, 9);
    const size_t parts = size_t{1} << partBits;
    const size_t idShards = std::clamp<size_t>(std::bit_ceil(bytes >> 22), 1, 1024);
    auto partOf = [&](uint64_t hash) { return static_cast<size_t>((hash >> 1) >> (63 - partBits)); };

    const unsigned readerCount = static_cast<unsigned>(std::min<size_t>(source.spans.size(), workers));
    std::vector<std::unique_ptr<Reader>> readers(readerCount);
    std::atomic<size_t> next = 0;
    parallel(readerCount, readerCount, [&](size_t r) {
        readers[r] = std::make_unique<Reader>(parts, idShards);
        Reader& reader = *readers[r];
        ParsedEvent event;
        Click click;
        for (;;) {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= source.spans.size()) break;
            const InputChunk& span = source.spans[i];
            if (i < source.eventSpans) {
                const uint64_t base = spanStart[i] - reinterpret_cast<uintptr_t>(span.file.data());
                chunkFields<7>(span, [&](std::string_view line, const auto& fields) {
                    if (!parseEvent(fields, ctx, event)) return;
                    const uint64_t offset = base + reinterpret_cast<uintptr_t>(line.data());
                    const uint64_t hash = event.id.hash();
                    uint64_t record = offset;
                    if (event.valid) {
                        if (!(event.row.payload & StagedEvent::inlineId)) event.row.origin = offset;
                        const size_t part = partOf(event.row.user.hash());
                        const uint64_t index = reader.events.add(part, event.row);
                        if (index >> indexBits) throw std::runtime_error("Too many rows in one partition");
                        record = stagedFlag | (uint64_t(r) << (indexBits + partBitsMax)) |
                                 (uint64_t(part) << indexBits) | index;
                    }
                    record |= (hash >> 32) << tagShift;
                    reader.ids.add(hash & (idShards - 1), record);
                });
            } else {
                chunkFields<4>(span, [&](std::string_view, const auto& fields) {
                    if (parseClick(fields, ctx, click))
                        reader.clicks.add(partOf(click.person.hash()), click);
                });
            }
        }
        reader.events.finish();
        reader.clicks.finish();
        reader.ids.finish();
    });
    lap("parse");

    parallel(idShards, workers, [&](size_t shard) {
        deduplicate(shard, ctx, source, readers);
        std::vector<void*> spent;
        for (auto& reader : readers) reader->ids.release(shard, spent);
        recycled.give(spent);
    });
    lap("dedup");
    awaitIdentities();
    lap("await-links");

    // Route every surviving row to its person's bucket. A partition's lookups
    // stay within one slice of the identity table.
    std::vector<std::unique_ptr<Router>> routers(workers);
    std::atomic<size_t> nextPart = 0;
    const size_t bucketMask = out.size() - 1;
    parallel(workers, workers, [&](size_t w) {
        routers[w] = std::make_unique<Router>(out.size());
        Router& router = *routers[w];
        std::vector<void*> spent;
        for (;;) {
            const size_t p = nextPart.fetch_add(1, std::memory_order_relaxed);
            if (p >= parts) break;
            spent.clear();
            for (auto& reader : readers) {
                reader->events.pieces(p, [&](const StagedEvent* rows, size_t n) {
                    for (size_t i = 0; i < n; ++i) {
                        const StagedEvent& row = rows[i];
                        if (row.ts == invalidTime) continue;
                        uint64_t personHash = row.user.hash();
                        const Key person = ctx.identities.person(row.user, personHash);
                        router.events.add(personHash & bucketMask,
                                          Event::pack(person, ctx.start, {row.ts, row.price(), row.category()}));
                    }
                });
                reader->clicks.pieces(p, [&](const Click* rows, size_t n) {
                    for (size_t i = 0; i < n; ++i) {
                        Click click = rows[i];
                        uint64_t personHash = click.person.hash();
                        click.person = ctx.identities.person(click.person, personHash);
                        router.clicks.add(personHash & bucketMask, click);
                    }
                });
                reader->events.release(p, spent);
                reader->clicks.release(p, spent);
            }
            recycled.give(spent);
        }
        router.events.finish();
        router.clicks.finish();
    });
    for (size_t b = 0; b < out.size(); ++b)
        for (auto& router : routers) {
            router->events.pieces(b, [&](const Event* rows, size_t n) { out[b].events.add(rows, n); });
            router->clicks.pieces(b, [&](const Click* rows, size_t n) { out[b].clicks.add(rows, n); });
        }
    // The blocks outlive this function: buckets point into them.
    for (auto& reader : readers) (void)reader.release();
    for (auto& router : routers) (void)router.release();
    lap("route");
}
