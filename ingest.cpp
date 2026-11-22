#include <condition_variable>
#include <mutex>
#include <thread>

#include <sys/mman.h>
#include "shared.hpp"
#include "event_fields.hpp"

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

inline bool hourTooLarge(const char *hour) {
    uint16_t word;
    std::memcpy(&word, hour, 2);
    return __builtin_bswap16(word) > 0x3233;
}
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

inline int64_t cachedMidnight(const char* date, uint64_t digits) {
    struct Entry { uint64_t digits; int64_t ms = invalidTime; };
    static thread_local std::array<Entry, 64> cache{};
    const unsigned slot = ((uint8_t(date[8]) & 3) << 4) | (uint8_t(date[9]) & 15);
    auto& entry = cache[slot];
    if (entry.digits != digits) entry = {digits, calendarMidnight(date)};
    return entry.ms;
}

inline int64_t midnight(const char* p) {
    const auto data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const auto packed = _mm_shuffle_epi8(data,
        _mm_setr_epi8(0,1,2,3,5,6,8,9,-1,-1,-1,-1,-1,-1,-1,-1));
    const uint64_t digits = _mm_cvtsi128_si64(packed);
    return cachedMidnight(p, digits);
}

// Convert all three clock pairs in one vector. For untrusted event times
// validate the digit ranges and literal colons before returning the value.
template<bool Check> inline int clockSeconds(const char* p) {
    const auto raw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p));
    const auto digits = _mm_sub_epi8(raw, _mm_cvtsi64_si128(0x30303a30303a3030LL));
    if constexpr (Check) {
        const auto excess = _mm_subs_epu8(digits, _mm_cvtsi64_si128(0x0905000905000902LL));
        if (!_mm_testz_si128(excess, excess) || hourTooLarge(p))
            return -1;
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

// Parse 13 decimal digits with two bounded overlapping loads. Combining digit
// pairs, then four-digit groups, leaves two eight-digit chunks for the scalar sum.
int64_t decimal13(const char* p) {
    const auto first = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p));
    const auto last = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p+5));
    const auto digits = _mm_sub_epi8(_mm_unpacklo_epi64(first,last), _mm_set1_epi8('0'));
    const auto invalid = _mm_subs_epu8(digits, _mm_set1_epi8(9));
    if (!_mm_testz_si128(invalid,invalid)) return invalidTime;
    const auto padded = _mm_shuffle_epi8(digits,
        _mm_setr_epi8(-1,-1,-1,0,1,2,3,4,5,6,7,11,12,13,14,15));
    const auto pairs = _mm_maddubs_epi16(padded, _mm_set1_epi16(0x010a));
    const auto quads = _mm_madd_epi16(pairs, _mm_set1_epi32(0x00010064));
    const auto chunks = _mm_madd_epi16(_mm_packs_epi32(quads,quads), _mm_set1_epi32(0x00012710));
    return int64_t(_mm_cvtsi128_si32(chunks)) * 100'000'000 + _mm_extract_epi32(chunks,1);
}

struct InputChunk {
    std::string_view file;
    size_t begin, end;
};

// Boundaries depend only on file sizes. Readers fault in their own chunks
// and consume every line whose first byte belongs to their range.
std::vector<InputChunk> chunks(const std::vector<std::string_view>& files, unsigned chunkBits) {
    std::vector<InputChunk> result;
    const size_t bytes = size_t{1} << chunkBits;
    for (auto file : files)
        for (size_t begin = 0; begin < file.size(); begin += bytes)
            result.push_back({file, begin, std::min(file.size(), begin + bytes)});
    return result;
}

// Input chunks start at page-aligned offsets in file mappings. Request
// translations for the reader's own range immediately before scanning it.
inline void populateChunk(const InputChunk& c) {
#ifdef MADV_POPULATE_READ
    if (c.end - c.begin >= (256u << 10))
        (void)madvise(const_cast<char*>(c.file.data() + c.begin), c.end - c.begin, MADV_POPULATE_READ);
#endif
}

template<class F> void chunkLines(const InputChunk& chunk, F&& visit) {
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
    // Newlines are found 64 bytes at a time; `pos` is the open line's start.
    auto newlinesIn = [](const char* p) -> uint64_t {
#ifdef __AVX512BW__
        return _mm512_cmpeq_epi8_mask(_mm512_loadu_si512(reinterpret_cast<const void*>(p)),
                                      _mm512_set1_epi8('\n'));
#else   // local builds for emulators without AVX-512
        const __m256i newline = _mm256_set1_epi8('\n');
        const uint32_t a = _mm256_movemask_epi8(_mm256_cmpeq_epi8(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)), newline));
        const uint32_t b = _mm256_movemask_epi8(_mm256_cmpeq_epi8(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 32)), newline));
        return a | (uint64_t{b} << 32);
#endif
    };
    size_t scan = pos;
    while (pos < chunk.end) {
        // Collect offsets from a small tile before row decoding. The common
        // zero/one newline case advances the compact list without a branch.
        constexpr unsigned tileBytes = 1024;
        if (scan + tileBytes <= fileEnd) {
            uint16_t ends[tileBytes + 1];
            unsigned count = 0;
            for (unsigned k = 0; k < tileBytes; k += 64) {
                uint64_t mask = newlinesIn(bytes + scan + k);
                ends[count] = k + std::countr_zero(mask);
                count += mask != 0;
                mask &= mask - 1;
                while (mask) {
                    ends[count++] = k + std::countr_zero(mask);
                    mask &= mask - 1;
                }
            }
            for (unsigned k = 0; k < count; k++) {
                const size_t end = scan + ends[k];
                visit(std::string_view(bytes + pos, end - pos));
                pos = end + 1;
                if (pos >= chunk.end)
                    return;
            }
            scan += tileBytes;
        } else if (scan + 64 <= fileEnd) {
            uint64_t found = newlinesIn(bytes + scan);
            while (found) {
                const size_t end = scan + std::countr_zero(found);
                visit(std::string_view(bytes + pos, end - pos));
                pos = end + 1;
                if (pos >= chunk.end) return;
                found &= found - 1;
            }
            scan += 64;
        } else {
            size_t end = chunk.file.find('\n', std::max(pos, scan));
            if (end == chunk.file.npos) end = fileEnd;
            visit(std::string_view(bytes + pos, end - pos));
            pos = scan = end + 1;
        }
    }
}

// ---- Block storage -------------------------------------------------------
// Every row stream is a chain of equally sized, cache-line-aligned blocks cut
// from large per-thread extents. Nothing is freed: the process exits first.
constexpr size_t blockBytes = 16u << 10;
constexpr size_t extentBytes = 8u << 20;
static_assert(extentBytes % blockBytes == 0 && extentBytes % (2u << 20) == 0);
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
    size_t takeBatch(void** dest, size_t limit) {
        if (!available.load(std::memory_order_relaxed)) return 0;
        std::lock_guard guard(lock);
        size_t n = std::min(limit,blocks.size());
        if (n) {
            std::copy_n(blocks.end()-n,n,dest);
            blocks.resize(blocks.size()-n);
            available.store(blocks.size(),std::memory_order_relaxed);
        }
        return n;
    }
} recycled;

// Requests describe an already mapped anonymous writer range. At most eight
// 2 MiB hints can wait, plus one currently being populated. Data ownership
// stays with the original reader throughout and readers never wait for space.
class LiveSlabAdvice {
    static constexpr size_t quantum=2u<<20;
    struct Ticket { char* address; unsigned reader; uint64_t epoch; };
    std::array<Ticket,8> queue{};
    std::array<std::atomic<uint64_t>,64> position{};
    std::mutex lock;
    std::atomic<uint64_t> changed{0};
    const std::vector<InputChunk>& spans;
    std::atomic<size_t>& claimed;
    const size_t eventCount,lead;
    size_t nextInput=0;
    bool warmInput;
    void signal() {
        changed.fetch_add(1,std::memory_order_release);
        changed.notify_one();
    }
    bool handleInput() {
        if(!warmInput)return false;
        const size_t head=claimed.load(std::memory_order_acquire);
        nextInput=std::max(nextInput,head);
        if(nextInput>=eventCount || nextInput-head>=lead)return false;
        const auto& s=spans[nextInput++];
#if defined(MADV_POPULATE_READ)
        if(s.end-s.begin>=(256u<<10) && ::madvise(const_cast<char*>(s.file.data()+s.begin),
                  s.end-s.begin,MADV_POPULATE_READ) && (errno==EINVAL || errno==ENOSYS))
            readSupported=false;
#endif
        return true;
    }
    bool readSupported=true;
    std::thread helper;
    size_t first=0,size=0,capacity=0;
    bool closing=false;
    enum class Mode { demand, sync, background } mode=Mode::demand;
    std::atomic<bool> supported{true};

    void populate(char* address) {
#if defined(MADV_POPULATE_WRITE)
        if (::madvise(address,quantum,MADV_POPULATE_WRITE) && (errno==EINVAL || errno==ENOSYS))
            supported.store(false,std::memory_order_relaxed);
#else
        supported.store(false,std::memory_order_relaxed);
#endif
    }
    void work() {
        for (;;) {
            const uint64_t state=changed.load(std::memory_order_acquire);
            Ticket t{};
            bool hasWrite=false;
            {
                std::lock_guard guard(lock);
                if(closing) return;
                if(size) {
                    t=queue[first];
                    first=(first+1)%queue.size();
                    --size;hasWrite=true;
                }
            }
            if(hasWrite && supported.load(std::memory_order_relaxed) &&
               position[t.reader].load(std::memory_order_acquire)==t.epoch)
                populate(t.address);
            const bool hasInput=readSupported && handleInput();
            if(!hasWrite && !hasInput)changed.wait(state,std::memory_order_acquire);
        }
    }
public:
    LiveSlabAdvice(unsigned readers,bool enabled,const std::vector<InputChunk>& chunks,
                   size_t n,std::atomic<size_t>& next,bool warm)
        : spans(chunks),claimed(next),eventCount(n),lead(std::min<size_t>(16,2*readers)),warmInput(warm) {
        if (!enabled) return;
        const char* value=std::getenv("SHOP_SLAB_MODE");
        if(value && std::string_view(value)=="demand") return;
        if(value && std::string_view(value)=="sync") {mode=Mode::sync;return;}
        capacity=std::min<size_t>(readers,queue.size());
        try {helper=std::thread([this]{work();});mode=Mode::background;}
        catch(const std::system_error&) {}
    }
    bool enabled() const {return mode!=Mode::demand;}
    size_t takeInput() {
        const size_t i=claimed.fetch_add(1,std::memory_order_relaxed);
        if(warmInput && mode==Mode::background)signal();
        return i;
    }
    void advance(unsigned reader, uint64_t epoch, char* next) {
        position[reader].store(epoch,std::memory_order_release);
        if(!next || !supported.load(std::memory_order_relaxed)) return;
        if(mode==Mode::sync) {populate(next);return;}
        std::unique_lock guard(lock);
        if(size==capacity) return;
        queue[(first+size)%queue.size()]={next,reader,epoch};
        ++size;
        guard.unlock();
        signal();
    }
    void finish() {
        if(!helper.joinable()) return;
        {std::lock_guard guard(lock);closing=true;}
        signal();
        helper.join();
    }
    ~LiveSlabAdvice() {finish();}
};

class Slab {
    // Each router draws a batch from the common pool under one lock.
    std::array<void*,32> local;
    size_t left = 0;
    char* next = nullptr;
    char* end = nullptr;
    char* nextHint=nullptr;
    LiveSlabAdvice* advice=nullptr;
    unsigned readerId=0;
    uint64_t epoch=0;
public:
    void enableHints(LiveSlabAdvice& owner,unsigned id) {
        if(owner.enabled()){advice=&owner;readerId=id;}
    }
    void stopHints() {advice=nullptr;nextHint=nullptr;}
    void* take() {
        if (!left) left = recycled.takeBatch(local.data(),local.size());
        if (left) return local[--left];
        if (next == end) {
            constexpr size_t huge = 2u << 20;
            void* raw = mmap(nullptr, extentBytes + huge, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (raw == MAP_FAILED) throw std::bad_alloc();
            next = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(raw) + huge - 1) & ~(huge - 1));
            end = next + extentBytes;
            madvise(next, extentBytes, MADV_HUGEPAGE);
            if(advice) nextHint=next;
        }
        if(next==nextHint) {
            constexpr size_t quantum=2u<<20;
            nextHint=next+quantum;
            advice->advance(readerId,++epoch,nextHint<end ? nextHint : nullptr);
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
    // Keep the active block address by the row count. Most group flushes
    // can now reach their cache line without traversing the block chain.
    struct Cursor { T* active = nullptr; uint32_t count = 0; };
    std::vector<Cursor> cursors;
    T* destination(size_t part, size_t start) {
        const size_t offset = start % rowsPerBlock<T>;
        auto& cursor = cursors[part];
        if (!offset) {
            cursor.active = static_cast<T*>(slab.take());
            blocks[part].push_back(cursor.active);
        }
        return cursor.active + offset;
    }
public:
    uint32_t count(size_t part) const { return cursors[part].count; }
    std::vector<std::vector<T*>> blocks;
    Scatter(Slab& slab, size_t parts) : slab(slab), buffers(parts), cursors(parts), blocks(parts) {}

    // Returns the row's position in its partition's chain.
    uint32_t add(size_t part, const T& row) {
        const uint32_t index = cursors[part].count++;
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
        for (size_t part = 0; part < cursors.size(); ++part) {
            const size_t rest = cursors[part].count % Group;
            if (rest) std::memcpy(destination(part, cursors[part].count - rest), &buffers[part], rest * sizeof(T));
        }
        _mm_sfence();
        std::vector<Buffer>().swap(buffers);
    }
    void release(size_t part, std::vector<void*>& to) {
        to.insert(to.end(), blocks[part].begin(), blocks[part].end());
    }
    T& at(size_t part, size_t index) { return blocks[part][index / rowsPerBlock<T>][index % rowsPerBlock<T>]; }
    template<class F> void pieces(size_t part, F&& visit) {
        size_t remaining = cursors[part].count;
        for (T* block : blocks[part]) {
            const size_t count = std::min(remaining, rowsPerBlock<T>);
            visit(block, count);
            remaining -= count;
        }
    }
};

// ---- Rows ----------------------------------------------------------------
// Each ID and its arrival/locator fit in 16 bytes for ordinary windows.
// The low word carries the bucket and row index; its high bit selects one
// of the exact fallbacks. The high word is the biased arrival in milliseconds.
constexpr unsigned refBits = 40;
constexpr unsigned indexBits = 21;
constexpr uint32_t indexLimit = 1u << indexBits;
constexpr uint32_t sourceTag = 0x80000000u, invalidArrivalTag = 0xa0000000u, slowTag = 0xc0000000u;

struct EventToken {
    uint64_t bits{};
    bool external() const { return bits >= Key::indirectTag; }
    static constexpr unsigned hashBits = 23;
    static constexpr uint64_t hashMask = (uint64_t{1} << hashBits) - 1;
    static constexpr uint64_t addressMask = (uint64_t{1} << (60 - hashBits)) - 1;
    bool direct() const { return (bits >> 60) == 10; }
    bool witness() const { return (bits >> 60) == 9 || direct(); }
    uint32_t witnessIndex() const { return uint32_t(bits >> 32) & 0x0fffffffu; }
    bool operator==(EventToken other) const {
        return bits == other.bits ||
            (external() && other.external() && Key{bits}.borrowed() == Key{other.bits}.borrowed());
    }
    uint64_t hash() const {
        if (external()) return Key{bits}.hash();
        return (bits ^ (bits >> 27)) * 0x9e3779b97f4a7c15ull;
    }
};
inline EventToken encodeEventToken(Key key) {
    if (key.external()) return {key.bits};
    // For fixed high 32 bits, CRC32C maps the low 32 bits bijectively.
    // Retaining that high word therefore retains the exact inline key.
    const uint32_t crc = _mm_crc32_u64(0, key.bits);
    return {(key.bits & UINT64_C(0xffffffff00000000)) | crc};
}
inline uint32_t externalHash(EventToken token) {
    const uint64_t hash = Key{token.bits}.hash();
    return static_cast<uint32_t>(hash ^ (hash >> 32));
}
struct IdRecord {
    EventToken id;
    uint64_t info;
};
struct Extended { int64_t arrival; uint32_t bucket,index; };
static_assert(sizeof(IdRecord) == 16);

struct Reader {
    Slab slab;
    Scatter<Event, 16> events;
    Scatter<Click, 8> clicks;
    Scatter<IdRecord, 16> ids;
    std::vector<Extended> slow;
    std::vector<size_t> cancelBases;
    std::vector<uint8_t> cancelled;
    void prepareCancellations(size_t partitions) {
        cancelBases.resize(partitions);
        size_t total = 0;
        for (size_t bucket = 0; bucket < partitions; ++bucket) {
            cancelBases[bucket] = total;
            total += events.count(bucket);
        }
        // Give the final partial vector a bounded, readable tail.
        cancelled.resize(total + 31);
    }
    void cancel(size_t bucket, size_t index) {
        // Every exact losing occurrence is distinct, including when its
        // previous winner is displaced. Workers never write the same byte.
        cancelled[cancelBases[bucket] + index] = 0xff;
    }

    static constexpr uint32_t witnessLimit = 1u << 28;
    bool rawOverflow = false;
    uint64_t directBase = 0;
    static constexpr unsigned pageBits = 13;
    static constexpr size_t pageSize = size_t{1} << pageBits;
    uint32_t count = 0;
    std::vector<std::unique_ptr<uint64_t[]>> pages;
    uint64_t* active = nullptr;
    size_t opaqueCount() const { return count; }
    void saveOpaque(uint64_t word) {
        const uint32_t at = count++ & (pageSize - 1);
        if (!at) {
            pages.push_back(std::make_unique_for_overwrite<uint64_t[]>(pageSize));
            active = pages.back().get();
        }
        active[at] = word;
    }
    uint64_t originalBits(uint32_t ordinal) const { return pages[ordinal >> pageBits][ordinal & (pageSize - 1)]; }
    void releaseOpaque() { pages.clear(); active = nullptr; }
    EventToken rememberOpaque(EventToken full, uint32_t hash) {
        const uint64_t base = full.bits & ~EventToken::addressMask;
        if (!directBase) directBase = base;
        if (base == directBase) {
            return {UINT64_C(0xa000000000000000) | ((full.bits & EventToken::addressMask) << EventToken::hashBits) |
                    (hash & EventToken::hashMask)};
        }
        const size_t ordinal = opaqueCount();
        if (ordinal >= witnessLimit) { rawOverflow = true; return full; }
        saveOpaque(full.bits);
        return {UINT64_C(0x9000000000000000) | (uint64_t(ordinal) << 32) | hash};
    }
    EventToken original(EventToken token) const {
        if (token.direct()) {
            return {directBase | ((token.bits >> EventToken::hashBits) & EventToken::addressMask)};
        }
        return token.witness() ? EventToken{originalBits(token.witnessIndex())} : token;
    }
    Reader(size_t parts, size_t idShards) : events(slab, parts), clicks(slab, parts), ids(slab, idShards) {}
};

uint64_t special(uint64_t value, uint32_t tag) {
    return (value << 32) | (value >> 32) | tag;
}
uint64_t specialValue(uint64_t info) {
    return (info >> 32) | ((info & 0x1fffffffu) << 32);
}
uint64_t relativeArrival(int64_t arrival, int64_t origin) {
    return uint64_t(arrival) - uint64_t(origin) + (uint64_t{1} << 31);
}
uint64_t location(Reader& reader, int64_t arrival, int64_t origin, uint32_t bucket, uint32_t index) {
    const uint64_t relative = relativeArrival(arrival,origin);
    if (relative <= UINT32_MAX && index < indexLimit)
        return (relative << 32) | (bucket << indexBits) | index;
    const auto offset = reader.slow.size();
    reader.slow.push_back({arrival,bucket,index});
    return special(offset,slowTag);
}

struct Router {
    Slab slab;
    Scatter<Event, 16> events;
    Scatter<Click, 8> clicks;
    explicit Router(size_t buckets) : events(slab, buckets), clicks(slab, buckets) {}
};

struct ParsedEvent {
    EventToken id;
    uint32_t idHash;
    bool valid = false;
    int64_t arrival;
    Key user;
    EventData row;
};

// Match midnight()'s eight-byte calendar key: YYYY followed by MMDD.
inline uint64_t isoDateKey(const char* date, __m128i fields) {
    uint32_t year;
    std::memcpy(&year, date, sizeof(year));
    const uint32_t monthDay = _mm_extract_epi32(fields, 2);
    return uint64_t(year) | (uint64_t(monthDay) << 32);
}

template<bool HasOffset>
inline bool isoTimes(const char* event, const char* arrival, const Context& ctx,
                     int64_t& eventMs, int64_t& arrivalMs) {
    // Each lane spans the first dash through the zone marker, "-MM-DDTHH:MM:SSZ".
    // Subtraction normalizes clock digits and punctuation while preserving MM and DD.
    const auto eventBytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(event + 4));
    const auto arrivalBytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(arrival + 4));
    const auto pattern = _mm_setr_epi8('-', 0, 0, '-', 0, 0, 'T', '0', '0', ':', '0', '0', ':', '0', '0', 'Z');
    const auto normalized = _mm256_sub_epi8(_mm256_set_m128i(arrivalBytes, eventBytes),
                                             _mm256_broadcastsi128_si256(pattern));

    // Arrival has guaranteed UTC syntax. Check event punctuation, clock digits and UTC
    // suffix here. Calendar validation runs on a cache miss using the complete date key.
    const auto limits = _mm_setr_epi8(0, -1, -1, 0, -1, -1, 0, 2, 9, 0, 5, 9, 0, 5, 9, HasOffset ? -1 : 0);
    const auto excess = _mm_subs_epu8(_mm256_castsi256_si128(normalized), limits);
    if (!_mm_testz_si128(excess, excess) || hourTooLarge(event + 11))
        return false;

    int offsetSeconds = 0;
    if constexpr (HasOffset) {
        const char sign = event[19];
        if ((sign != '+' && sign != '-') || event[22] != ':' ||
            !digit(event[20]) || !digit(event[21]) || !digit(event[23]) || !digit(event[24]))
            return false;
        const int hours = pairValue(event + 20);
        const int minutes = pairValue(event + 23);
        if (hours > 23 || minutes > 59)
            return false;
        offsetSeconds = (sign == '+' ? 1 : -1) * (3600 * hours + 60 * minutes);
    }

    // Each lane becomes HHMMSS, two zero bytes, MMDD, and four zero bytes.
    const auto layout = _mm_setr_epi8(7, 8, 10, 11, 13, 14, -1, -1, 1, 2, 4, 5, -1, -1, -1, -1);
    const auto fields = _mm256_shuffle_epi8(normalized, _mm256_broadcastsi128_si256(layout));
    const auto pairs = _mm256_maddubs_epi16(fields, _mm256_set1_epi16(0x010a));
    const auto weights = _mm_setr_epi16(3600, 60, 1, 0, 0, 0, 0, 0);
    const auto partialSeconds = _mm256_madd_epi16(pairs, _mm256_broadcastsi128_si256(weights));
    const auto seconds = _mm256_add_epi32(partialSeconds, _mm256_srli_si256(partialSeconds, 4));

    const uint64_t eventKey = isoDateKey(event, _mm256_castsi256_si128(fields));
    const int64_t eventMidnight = cachedMidnight(event, eventKey);
    if (eventMidnight == invalidTime)
        return false;
    const int eventSeconds = _mm_cvtsi128_si32(_mm256_castsi256_si128(seconds));
    eventMs = eventMidnight + 1000LL * (eventSeconds - offsetSeconds);
    if (eventMs < ctx.start || eventMs >= ctx.end)
        return false;

    const uint64_t arrivalKey = isoDateKey(arrival, _mm256_extracti128_si256(fields, 1));
    const int64_t arrivalMidnight = eventKey == arrivalKey ? eventMidnight : cachedMidnight(arrival, arrivalKey);
    const int arrivalSeconds = _mm_cvtsi128_si32(_mm256_extracti128_si256(seconds, 1));
    arrivalMs = arrivalMidnight + 1000LL * arrivalSeconds;
    return arrivalMs - dayMs <= eventMs;
}

[[gnu::always_inline]] inline bool eventTimes(std::string_view event, std::string_view arrival,
                                              const Context &ctx, int64_t &ts, int64_t &ingest) {
    const char* e = event.data();
    const char* a = arrival.data();
    if (event.size() == 13 && arrival.size() == 20) {
        // There are at least three more bytes in this CSV row beyond the epoch field.
        const auto raw = _mm_loadu_si128(reinterpret_cast<const __m128i *>(e));
        const auto digits = _mm_sub_epi8(raw, _mm_set1_epi8('0'));
        const auto padded = _mm_slli_si128(digits, 3);
        const auto excess = _mm_subs_epu8(padded, _mm_set1_epi8(9));
        if (!_mm_testz_si128(excess, excess))
            return false;
        const auto pairs = _mm_maddubs_epi16(padded, _mm_set1_epi16(0x010a));
        const auto quads = _mm_madd_epi16(pairs, _mm_set1_epi32(0x00010064));
        const auto chunks = _mm_madd_epi16(_mm_packs_epi32(quads, quads), _mm_set1_epi32(0x00012710));
        ts = int64_t(_mm_cvtsi128_si32(chunks)) * 100000000 + _mm_extract_epi32(chunks, 1);
        if (ts < ctx.start || ts >= ctx.end)
            return false;
        ingest = validIngest(a);
        return ingest - dayMs <= ts;
    }
    if (arrival.size() == 20 && (event.size() == 20 || event.size() == 25) && e[4]=='-') {
        if (event.size() == 20)
            return isoTimes<false>(e, a, ctx, ts, ingest);
        return isoTimes<true>(e, a, ctx, ts, ingest);
    }
    ts = timestamp(event);
    if (ts < ctx.start || ts >= ctx.end)
        return false;
    ingest = arrival.size() == 20 ? validIngest(a) : timestamp(arrival);
    return ingest-dayMs <= ts;
}

// IDs are opaque CSV values. This lossless compact Key uses all the bytes,
// including case and leading zeroes, or borrows the exact mapped string.
inline Key eventID(std::string_view text) { return Key::from(text); }
inline EventToken tokenFromText(std::string_view text) {
    if (text.size() == 9) {
        uint64_t first, last;
        std::memcpy(&first, text.data(), 8);
        std::memcpy(&last, text.data() + 1, 8);
        if (!((first | last) & ~Key::asciiMask)) {
            // Of 63 ASCII bits, keep bytes 0..3 and the low three bits
            // of byte 4. CRC32C has full rank on the other 32 bits.
            // Together the two words identify the original nine bytes exactly.
            constexpr uint64_t kept = UINT64_C(0x000000077f7f7f7f);
            const uint64_t prefix = _pext_u64(first, kept);
            const uint32_t crc = _mm_crc32_u64(uint8_t(first), last);
            return {(prefix << 32) | crc};
        }
    }
    return encodeEventToken(eventID(text));
}
// Finish the identity record while its CSV spelling is still at the decoder.
inline void prepareIdentity(std::string_view text, Reader& reader, ParsedEvent& out) {
    EventToken token = tokenFromText(text);
    uint32_t hash = static_cast<uint32_t>(token.bits);
    if (token.external()) {
        const uint64_t whole = Key::spellingHash(text);
        hash = static_cast<uint32_t>(whole ^ (whole >> 32));
        token = reader.rememberOpaque(token, hash);
    }
    out.id = token;
    out.idHash = hash;
}
static_assert(sizeof(ParsedEvent) == 48);

// Malformed lines and empty IDs never take part in deduplication. Every other
// copy does, valid or not: an invalid earliest copy suppresses later ones.
[[gnu::noinline]] bool genericEvent(std::string_view line, const Context &ctx, ParsedEvent &out, Reader& reader) {
    std::array<std::string_view,7> f;
    if (!eventFields(line, f) || f[0].empty()) return false;
    prepareIdentity(f[0], reader, out);
    out.valid = false;
    out.arrival = invalidTime;
    if (f[1].empty() || f[5].empty()) return true;
    int64_t ts;
    if (!eventTimes(f[2], f[3], ctx, ts, out.arrival)) return true;
    uint32_t price = 0, category = 0;
    if (f[4] == "purchase") {
        auto priceEnd = f[6].data() + f[6].size();
        auto [end, error] = std::from_chars(f[6].data(), priceEnd, price);
        if (error != std::errc{} || end != priceEnd || price == 0 || price > 10'000'000)
            return true;
        category = ctx.products.lookup(f[5], line.data() + line.size());
        if (category == UINT32_MAX)
            category = ctx.unknown;
    } else if (f[4] != "view" && f[4] != "cart") {
        return true;
    }
    out.user = Key::from(f[1]);
    out.row = {ts, price, category};
    out.valid = true;
    return true;
}
template <unsigned Hint> inline Key inlineAsciiKey(const char *p, unsigned n) {
    if (n == 9)
        return Key::nine<Hint>(p);
    if constexpr (Hint == 'u')
        if (n > 9) return Key::internShopper({p, n});
    return Key::from({p, n});
}
struct HeadLayout {
    uint64_t tag{};
    uint8_t id, idLen, user, userLen, event, quoted;
};
struct TailLayout {
    uint64_t tag{};
    uint8_t arrivalEnd, quoted, kind, kindLen, product, productLen, price, priceLen;
};
[[gnu::noinline]] bool decodeHead(CsvMask m, uint64_t tag, HeadLayout &e) {
    if (std::popcount(m.comma) != 2 || (m.comma & 0x20000001u))
        return false;
    uint32_t start = (m.comma << 1) | 1, end = m.comma >> 1;
    unsigned q = _pext_u32(m.quote, start);
    if ((q & 3) != _pext_u32(m.quote, end) || (m.quote & ~(start ^ end)))
        return false;
    unsigned c0 = std::countr_zero(m.comma), c1 = 31u - std::countl_zero(m.comma);
    e.id = q & 1;
    e.idLen = c0 - 2 * e.id;
    e.user = c0 + 1 + (q >> 1 & 1);
    e.userLen = c1 - c0 - 1 - 2 * (q >> 1 & 1);
    e.quoted = q >> 2;
    e.event = c1 + 1 + e.quoted;
    e.tag = tag;
    return true;
}
[[gnu::noinline]] bool decodeTail(CsvMask m, uint64_t tag, TailLayout &e) {
    if (std::popcount(m.comma) != 3 || (m.comma & 1))
        return false;
    uint32_t start = m.comma << 1, end = (m.comma >> 1) | 0x20000000u;
    unsigned q = _pext_u32(m.quote, start), closed = _pext_u32(m.quote, end);
    if (q != (closed >> 1) || (m.quote & ~(start ^ end)))
        return false;
    unsigned a = std::countr_zero(m.comma), b = std::countr_zero(m.comma & (m.comma - 1)),
             c = 31u - std::countl_zero(m.comma);
    e.quoted = closed & 1;
    e.arrivalEnd = a - e.quoted;
    e.kind = a + 1 + (q & 1);
    e.kindLen = b - a - 1 - 2 * (q & 1);
    e.product = b + 1 + (q >> 1 & 1);
    e.productLen = c - b - 1 - 2 * (q >> 1 & 1);
    e.price = c + 1 + (q >> 2);
    e.priceLen = 30 - c - 1 - 2 * (q >> 2);
    e.tag = tag;
    return true;
}
struct PreparedMasks {
    std::string_view line;
    uint64_t ht, tt;
    uint32_t commas, quotes;
    HeadLayout *head;
    TailLayout *tail;
};
inline void saveMasks(PreparedMasks &packed, CsvMask mh, CsvMask mm, CsvMask mt) {
    static thread_local std::array<HeadLayout, 256> heads{};
    static thread_local std::array<TailLayout, 2048> tails{};
    packed.ht = mh.comma | (uint64_t(mh.quote) << 32);
    packed.tt = mt.comma | (uint64_t(mt.quote) << 32);
    packed.head = &heads[_mm_crc32_u64(0, packed.ht) & 255];
    packed.tail = &tails[_mm_crc32_u64(0, packed.tt) & 2047];
    packed.commas = mm.comma;
    packed.quotes = mm.quote;
    __builtin_prefetch(packed.head, 0, 3);
    __builtin_prefetch(packed.tail, 0, 3);
}
inline void prepMasks(std::string_view s, PreparedMasks &packed) {
    packed.line = s;
    const size_t n = s.size();
    if (n <= 61 || n > 92) {
        packed.head = nullptr;
        return;
    }
    const char *p = s.data();
    auto mh = classify32(p), mm = classify32(p + 30), mt = classify32(p + n - 32);
    mh.comma &= 0x3fffffffu;
    mh.quote &= 0x3fffffffu;
    mt.comma >>= 2;
    mt.quote >>= 2;
    saveMasks(packed, mh, mm, mt);
}
template <class F> void eventLines(const InputChunk &chunk, F &&visit) {
    const char *p = chunk.file.data() + chunk.begin;
    const char *finish = chunk.file.data() + chunk.end;
    const char *limit = chunk.file.data() + chunk.file.size();
    if (!chunk.begin || p[-1] != '\n') {
        const char *sep = static_cast<const char *>(std::memchr(p, '\n', finish - p));
        if (!sep)
            return;
        p = sep + 1;
    }
    const auto newline = _mm256_set1_epi8('\n');
    auto byteMask = [](__m256i v, char c) -> uint32_t {
        return _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(c)));
    };
    while (p < finish) {
        if (limit - p > 2048)
            __builtin_prefetch(p + 2048, 0, 3);
        if (limit - p >= 92) {
            auto h = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p));
            auto m = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + 30));
            auto z = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + 60));
            auto early = _mm256_or_si256(_mm256_cmpeq_epi8(h, newline), _mm256_cmpeq_epi8(m, newline));
            uint32_t last = byteMask(z, '\n');
            if (!(_mm256_movemask_epi8(early) & 0x3fffffffu) && last && !(last & 3)) {
                unsigned l = std::countr_zero(last), n = 60 + l;
                CsvMask mh{byteMask(h, ',') & 0x3fffffffu, byteMask(h, '"') & 0x3fffffffu};
                CsvMask mm{byteMask(m, ','), byteMask(m, '"')};
                auto tailMask = [&](uint32_t middle, char c) {
                    uint64_t bits = (middle & 0x3fffffffu) | (uint64_t(byteMask(z, c)) << 30);
                    return static_cast<uint32_t>(bits >> l) & 0x3fffffffu;
                };
                CsvMask mt{tailMask(mm.comma, ','), tailMask(mm.quote, '"')};
                PreparedMasks &row = visit();
                row.line = {p, n};
                saveMasks(row, mh, mm, mt);
                p += n + 1;
                continue;
            }
        }
        const char *end = static_cast<const char *>(std::memchr(p, '\n', limit - p));
        if (!end)
            end = limit;
        prepMasks({p, static_cast<size_t>(end - p)}, visit());
        if (end == limit)
            break;
        p = end + 1;
    }
}
inline bool parsePrepared(const PreparedMasks &packed, const Context &ctx, ParsedEvent &out, Reader& reader) {
    auto s = packed.line;
    const size_t n = s.size();
    const char *p = s.data();
    if (!packed.head)
        return genericEvent(s, ctx, out, reader);
    auto &h = *packed.head;
    auto &t = *packed.tail;
    auto ht = packed.ht, tt = packed.tt;
    CsvMask mh{uint32_t(ht), uint32_t(ht >> 32)}, mt{uint32_t(tt), uint32_t(tt >> 32)},
        mm{packed.commas, packed.quotes};
    if ((h.tag != ht || !ht) && !decodeHead(mh, ht, h))
        return genericEvent(s, ctx, out, reader);
    if ((t.tag != tt || !tt) && !decodeTail(mt, tt, t))
        return genericEvent(s, ctx, out, reader);
    const unsigned width = n - 60;
    uint32_t mc = _bzhi_u32(mm.comma, width), mq = _bzhi_u32(mm.quote, width);
    if (!std::has_single_bit(mc) || (mc & ((1u << (width - 1)) | 1u)))
        return genericEvent(s, ctx, out, reader);
    if (mq != ((mc >> 1) * h.quoted | (mc << 1) * t.quoted))
        return false;
    if (!h.idLen)
        return false;
    prepareIdentity({p + h.id, h.idLen}, reader, out);
    out.valid = false;
    out.arrival = invalidTime;
    if (!t.productLen || !h.userLen)
        return true;
    unsigned e2 = 30u + std::countr_zero(mc), a = e2 + 1 + t.quoted;
    const char *tail = p + n - 30;
    int64_t ts;
    if (!eventTimes({p + h.event, e2 - h.event - h.quoted}, {p + a, n - 30 + t.arrivalEnd - a}, ctx, ts,
                    out.arrival))
        return true;
    uint32_t price = 0, category = 0;
    if (t.kindLen == 4) {
        uint32_t word;
        std::memcpy(&word, tail + t.kind, 4);
        if (word != 0x77656976u && word != 0x74726163u)
            return true;
    } else if (t.kindLen == 8 && !std::memcmp(tail + t.kind, "purchase", 8)) {
        const char *end = tail + t.price + t.priceLen;
        auto [stop, ec] = std::from_chars(tail + t.price, end, price);
        if (ec != std::errc{} || stop != end || !price || price > 10000000)
            return true;
        category = ctx.products.lookup({tail + t.product, t.productLen}, p + n);
        if (category == UINT32_MAX)
            category = ctx.unknown;
    } else
        return true;
    out.user = h.userLen == 9 ? Key::nine<'u'>(p + h.user) : Key::from({p + h.user, h.userLen});
    out.row = {ts, price, category};
    out.valid = true;
    return true;
}
inline int64_t ingestTime(std::string_view text) {
    return text.size() == 20 ? validIngest(text.data()) : timestamp(text);
}

[[gnu::noinline]] bool otherClick(std::string_view line, const Context &ctx, Click &row) {
    std::array<std::string_view,4> f;
    if (!csv(line, f) || f[0].empty() || f[1].empty()) return false;
    const auto ts = timestamp(f[3]);
    if (ts < ctx.start - 7 * dayMs || ts >= ctx.end) return false;
    const auto campaign = ctx.campaigns.lookup(f[2], line.data() + line.size());
    if (campaign == UINT32_MAX) return false;
    row = {Key::from(f[1]), Key::from(f[0]), ts, campaign};
    return true;
}

inline bool parseClick(std::string_view line, const Context &ctx, Click &row) {
    size_t n = line.size();
    if (n < 32 || n > 64)
        return otherClick(line, ctx, row);
    auto p = line.data();
    auto first = classify32(p), last = classify32(p + n - 32);
    uint64_t commas = first.comma | (uint64_t(last.comma) << (n - 32));
    uint64_t quotes = first.quote | (uint64_t(last.quote) << (n - 32));
    if (std::popcount(commas) != 3 || (commas & 1))
        return false;
    uint64_t starts = 1 | (commas << 1), ends = (commas >> 1) | (1ull << (n - 1));
    unsigned trim = _pext_u64(quotes, starts);
    if (trim != _pext_u64(quotes, ends) || (quotes & ~(starts ^ ends)))
        return false;
    unsigned e0 = std::countr_zero(commas), e1 = std::countr_zero(commas & (commas - 1)),
             e2 = 63 - std::countl_zero(commas);
    unsigned t0 = trim & 1, t1 = (trim >> 1) & 1, t2 = (trim >> 2) & 1, t3 = trim >> 3;
    unsigned idLen = e0 - 2 * t0, userLen = e1 - e0 - 1 - 2 * t1;
    if (!idLen || !userLen)
        return false;
    const char *tp = p + e2 + 1 + t3;
    unsigned tn = n - e2 - 1 - 2 * t3;
    int64_t ts = tn == 13 ? decimal13(tp) : timestamp({tp, tn});
    if (ts < ctx.start - 7 * dayMs || ts >= ctx.end)
        return false;
    unsigned cn = e2 - e1 - 1 - 2 * t2;
    const char *cp = p + e1 + 1 + t2;
    auto campaign = ctx.campaigns.lookup({cp, cn}, p + n);
    if (campaign == UINT32_MAX)
        return false;
    row = {userLen == 9 ? Key::nine<'u'>(p + e0 + 1 + t1) : Key::from({p + e0 + 1 + t1, userLen}), inlineAsciiKey<'c'>(p + t0, idLen), ts, campaign};
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

// Only invalid copies that were rejected before parsing their arrivals need
// a source reread, and only after their complete IDs test equal.
int64_t arrival(uint64_t info, const Reader& reader, const Source& source, int64_t origin) {
    const uint32_t low = info;
    if (low < sourceTag || low == invalidArrivalTag)
        return static_cast<int64_t>(uint64_t(origin) + (info >> 32) - (uint64_t{1} << 31));
    const uint64_t ref = specialValue(info);
    if (low >= slowTag) return reader.slow[ref].arrival;
    auto [line,limit] = source.locate(ref);
    auto newline = static_cast<const char*>(std::memchr(line,'\n',limit-line));
    std::array<std::string_view,7> fields;
    if (!csv(std::string_view(line,(newline ? newline : limit)-line), fields))
        throw std::logic_error("Source line changed");
    return ingestTime(fields[3]);
}

void invalidate(uint64_t info, Reader& reader) {
    const uint32_t low = info;
    if (low < sourceTag) {
        reader.cancel(low >> indexBits, low & (indexLimit - 1));
    } else if (low >= slowTag) {
        const auto& row = reader.slow[specialValue(info)];
        reader.cancel(row.bucket,row.index);
    }
}

struct Winner { IdRecord row; Reader* reader = nullptr; };
size_t findWinner(const std::vector<Winner> &slots, uint64_t hash, const EventToken &id) {
    size_t pos = (hash >> 10) & (slots.size()-1);
    while (slots[pos].reader && !(slots[pos].row.id == id))
        pos = (pos+1) & (slots.size()-1);
    return pos;
}

inline uint32_t filterHash(EventToken id) {
    return id.external() ? externalHash(id) : static_cast<uint32_t>(id.bits);
}
void dedupOriginal(size_t shard, int64_t arrivalOrigin, const Source &source,
                   std::vector<std::unique_ptr<Reader>> &readers) {
    size_t count = 0;
    for (auto& r : readers) count += r->ids.count(shard);
    if (count < 2) return;
    const size_t bits = std::clamp<size_t>(std::bit_ceil(count)*16,64,1u<<23);
    const size_t mask = bits-1;
    struct Word { uint64_t seen, repeated; };
    static thread_local std::vector<Word> filter;
    filter.assign(bits/64, Word{});
    for (auto& reader : readers)
        reader->ids.pieces(shard, [&](const IdRecord* rows,size_t n) {
            for (size_t i = 0; i < n; ++i) {
                const size_t bit = filterHash(rows[i].id) & mask;
                const uint64_t flag = uint64_t{1} << (bit%64);
                auto& word = filter[bit/64];
                word.repeated |= word.seen & flag;
                word.seen |= flag;
            }
        });
    static thread_local std::vector<Winner> slots;
    size_t repeated = 0;
    for (auto word : filter) repeated += std::popcount(word.repeated);
    if (!repeated) return;
    slots.assign(std::bit_ceil(repeated*3+1),Winner{});
    size_t used = 0;
    for (auto& reader : readers)
        reader->ids.pieces(shard, [&](const IdRecord* rows,size_t n) {
            for (size_t i = 0; i < n; ++i) {
                auto row = rows[i];
                const size_t bit = filterHash(row.id) & mask;
                if (!((filter[bit / 64].repeated >> (bit % 64)) & 1))
                    continue;
                row.id = reader->original(row.id);
                const uint64_t hash = row.id.hash();
                size_t pos = findWinner(slots,hash,row.id);
                if (!slots[pos].reader) {
                    if (++used*10 >= slots.size()*8) {
                        std::vector<Winner> larger(slots.size()*2);
                        for (auto& old : slots)
                            if (old.reader)
                                larger[findWinner(larger,old.row.id.hash(),old.row.id)] = old;
                        slots.swap(larger);
                        pos = findWinner(slots,hash,row.id);
                    }
                    slots[pos] = {row,reader.get()};
                    continue;
                }
                Winner& old = slots[pos];
                const bool inlineTimes = (uint32_t(row.info) | uint32_t(old.row.info)) < sourceTag;
                const bool earlier = inlineTimes ? (row.info >> 32) < (old.row.info >> 32)
                    : arrival(row.info,*reader,source,arrivalOrigin) <
                      arrival(old.row.info,*old.reader,source,arrivalOrigin);
                if (earlier) {
                    invalidate(old.row.info,*old.reader);
                    old = {row,reader.get()};
                } else {
                    invalidate(row.info,*reader);
                }
            }
        });
}
struct RecordBlock {
    const IdRecord *rows;
    Reader *owner;
    size_t count;
};
struct RecordDirectory {
    std::vector<RecordBlock> blocks;
    size_t count = 0;
    void load(size_t shard, const std::vector<std::unique_ptr<Reader>> &readers) {
        blocks.clear();
        count = 0;
        for (auto &r : readers)
            r->ids.pieces(shard, [&](const IdRecord *rows, size_t n) {
                blocks.push_back({rows, r.get(), n});
                count += n;
            });
    }
    static constexpr unsigned rowBits = std::countr_zero(rowsPerBlock<IdRecord>);
    static constexpr size_t within = rowsPerBlock<IdRecord> - 1;
    const IdRecord &get(uint32_t ref) const { return blocks[ref >> rowBits].rows[ref & within]; }
    bool equal(uint32_t newer, uint32_t older) const {
        const auto& a = blocks[newer >> rowBits];
        const auto& b = blocks[older >> rowBits];
        const auto left = a.rows[newer & within].id;
        const auto right = b.rows[older & within].id;
        if (!left.witness() && !right.witness()) return left == right;
        if (left.witness() && right.witness() && ((left.bits ^ right.bits) & EventToken::hashMask) != 0) return false;
        return a.owner->original(left) == b.owner->original(right);
    }
    bool earlier(uint32_t newer, uint32_t prior, const Source &source, int64_t origin) const {
        const auto &a = blocks[newer >> rowBits];
        const auto &b = blocks[prior >> rowBits];
        const uint64_t ia = a.rows[newer & within].info, ib = b.rows[prior & within].info;
        const bool before =
            (uint32_t(ia) | uint32_t(ib)) < sourceTag
                ? (ia >> 32) < (ib >> 32)
                : arrival(ia, *a.owner, source, origin) < arrival(ib, *b.owner, source, origin);
        if (before)
            invalidate(ib, *b.owner);
        else
            invalidate(ia, *a.owner);
        return before;
    }
};
// One cache line holds membership tags and row locations for sixteen IDs.
// A tag hit always leads to a comparison of the complete original IDs.
struct alignas(64) FingerGroup {
    uint8_t tags[16];
    uint16_t lo[16];
    uint8_t hi[16];
    uint32_t ref(unsigned i) const { return lo[i] | (uint32_t(hi[i]) << 16); }
    void set(unsigned i, uint32_t v) {
        lo[i] = uint16_t(v);
        hi[i] = uint8_t(v >> 16);
    }
};
static_assert(sizeof(FingerGroup) == 64);
void deduplicate(size_t shard, int64_t origin, const Source &source,
                 std::vector<std::unique_ptr<Reader>> &readers) {
    static thread_local RecordDirectory dir;
    dir.load(shard, readers);
    if (dir.count < 2)
        return;
    if (dir.blocks.size() > (1u << 24) / rowsPerBlock<IdRecord>) {
        dedupOriginal(shard, origin, source, readers);
        return;
    }
    static thread_local std::vector<FingerGroup> groups;
    const size_t cap = std::max<size_t>(16, std::bit_ceil(dir.count + dir.count / 4 + 1));
    // A directory this large would probe beyond the retained hash bits.
    // The existing fallback filter uses at most 2^23 bits and then compares full keys.
    if (cap / 16 > (1u << (EventToken::hashBits - 8))) {
        dedupOriginal(shard, origin, source, readers);
        return;
    }
    groups.assign(cap / 16, FingerGroup{});
    const uint32_t mask = groups.size() - 1;
    for (uint32_t b = 0; b < dir.blocks.size(); ++b) {
        const auto &block = dir.blocks[b];
        constexpr unsigned batch = 32;
        for (uint32_t first = 0; first < block.count; first += batch) {
            const unsigned n = std::min<size_t>(batch, block.count - first);
            uint32_t hashes[batch];
            if (!block.owner->rawOverflow) {
                for (unsigned j = 0; j < n; ++j) {
                    hashes[j] = uint32_t(block.rows[first+j].id.bits);
                    __builtin_prefetch(&groups[(hashes[j] >> 8) & mask], 1, 2);
                }
            } else {
                for (unsigned j = 0; j < n; ++j) {
                    hashes[j] = filterHash(block.rows[first+j].id);
                    __builtin_prefetch(&groups[(hashes[j] >> 8) & mask], 1, 2);
                }
            }
            for (unsigned j = 0; j < n; ++j) {
                const uint32_t h = hashes[j], ref = (b << dir.rowBits) | uint32_t(first + j);
                const unsigned tag = std::max(1u, h & 255);
                uint32_t pos = (h >> 8) & mask;
                for (;; pos = (pos + 1) & mask) {
                    auto &g = groups[pos];
                    auto chars = _mm_load_si128(reinterpret_cast<const __m128i *>(g.tags));
                    auto hits = _mm_movemask_epi8(_mm_cmpeq_epi8(chars, _mm_set1_epi8(tag)));
                    while (hits) {
                        unsigned k = std::countr_zero(unsigned(hits));
                        uint32_t old = g.ref(k);
                        if (dir.equal(ref,old)) {
                            if (dir.earlier(ref, old, source, origin))
                                g.set(k, ref);
                            goto complete;
                        }
                        hits &= hits - 1;
                    }
                    auto free = _mm_movemask_epi8(_mm_cmpeq_epi8(chars, _mm_setzero_si128()));
                    if (free) {
                        auto k = std::countr_zero(unsigned(free));
                        g.tags[k] = tag;
                        g.set(k, ref);
                        break;
                    }
                }
            complete:;
            }
        }
    }
}
// A removed event never reaches the identity table. Compact an entire source
// block once. A donor comes from the suffix beyond the final retained count,
// so moving it cannot disturb the flags or cause a second donor move.
size_t compactCancelled(Event* rows, size_t count, const uint8_t* cancelled) {
    constexpr size_t lanes = 32;
    std::array<uint32_t, (rowsPerBlock<Event> + lanes - 1) / lanes> masks;
    size_t removed = 0;
    for (size_t offset = 0; offset < count; offset += lanes) {
        const auto flags = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(cancelled + offset));
        uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(flags));
        const size_t remaining = count - offset;
        if (remaining < lanes) mask &= (uint32_t{1} << remaining) - 1;
        masks[offset / lanes] = mask;
        removed += std::popcount(mask);
    }
    if (!removed) return count;
    const size_t kept = count - removed;
    size_t donor = count;
    for (size_t offset = 0; offset < kept; offset += lanes) {
        uint32_t holes = masks[offset / lanes];
        const size_t remaining = kept - offset;
        if (remaining < lanes) holes &= (uint32_t{1} << remaining) - 1;
        while (holes) {
            const size_t index = offset + std::countr_zero(holes);
            do { --donor; } while (cancelled[donor]);
            rows[index] = rows[donor];
            holes &= holes - 1;
        }
    }
    return kept;
}
template<class T, size_t N, class Reject>
void correct(T* rows, size_t count, size_t bucket, unsigned shift,
             const Identities& identities, Scatter<T,N>& moved, Pieces<T>& kept, Reject reject) {
    // Swap an excluded row with the tail. Retained non-aliases incur no write.
    size_t i = 0;
    while (i < count) {
        T& row = rows[i];
        if (!reject(row)) {
            uint64_t hash = row.routingHash();
            const Key canonical = identities.person(row.person, hash);
            if (canonical.bits == row.person.bits) { ++i; continue; }
            row.rename(canonical, hash);
            const size_t target = (hash >> 1) >> shift;
            if (target == bucket) { ++i; continue; }
            moved.add(target,row);
        }
        if (--count != i) rows[i] = rows[count];
    }
    if (count) kept.add(rows,count);
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
        if ((p[19] != '+' && p[19] != '-') || p[22] != ':' || !digit(p[20]) ||
            !digit(p[21]) || !digit(p[23]) || !digit(p[24])) return invalidTime;
        const int oh = pairValue(p+20), om = pairValue(p+23);
        if (oh > 23 || om > 59) return invalidTime;
        offset = (p[19] == '+' ? 1 : -1) * (60 * oh + om);
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

    Source source;
    source.files = eventFiles;
    size_t bytes = 0;
    for (auto file : eventFiles) {
        source.starts.push_back(bytes);
        bytes += file.size();
    }
    if (bytes >> refBits) throw std::runtime_error("Event input too large");
    source.spans = chunks(eventFiles, ctx.settings.chunkBits);
    source.eventSpans = source.spans.size();
    for (auto& span : chunks(clickFiles, ctx.settings.chunkBits)) source.spans.push_back(span);
    if (source.spans.empty()) { awaitIdentities(); return; }
    std::vector<uint64_t> spanStart(source.eventSpans);
    for (size_t i = 0, f = 0; i < source.eventSpans; ++i) {
        while (source.files[f].data() != source.spans[i].file.data() ||
               source.files[f].size() != source.spans[i].file.size()) ++f;
        spanStart[i] = source.starts[f];
    }

    // Raw and canonical buckets share high hash bits, so most rows can stay
    // in their original blocks while a worker probes its identity-table slice.
    const size_t parts = out.size();
    const int64_t arrivalOrigin = std::clamp<int64_t>(ctx.start, INT64_MIN + (int64_t{1} << 31),
                                                     INT64_MAX - INT32_MAX);
    const unsigned shift = 63 - std::countr_zero(parts);
    // Use more ID partitions than user partitions so a worker's duplicate bitmap fits in cache.
    const size_t idShards = std::clamp<size_t>(std::bit_ceil(bytes >> 21), 1, size_t{1} << ctx.settings.idBits);
    const unsigned idShift = 32 - std::countr_zero(idShards);
    auto partOf = [&](uint64_t hash) { return static_cast<size_t>((hash >> 1) >> shift); };

    const unsigned readerCount = static_cast<unsigned>(std::min<size_t>(source.spans.size(), workers));
    std::atomic<size_t> next = 0;
    LiveSlabAdvice advice(readerCount,bytes>=(256u<<20),source.spans,source.eventSpans,next,!ctx.populateInput);
    std::vector<std::unique_ptr<Reader>> readers(readerCount);
    parallel(readerCount, readerCount, [&](size_t r) {
        readers[r] = std::make_unique<Reader>(parts, idShards);
        Reader& reader = *readers[r];
        reader.slab.enableHints(advice,r);
        Click click;
        for (;;) {
            const size_t i = advice.takeInput();
            if (i >= source.eventSpans)
                break;
            const InputChunk& span = source.spans[i];
            if (ctx.populateInput) populateChunk(span);
            if (i < source.eventSpans) {
                const uint64_t base = spanStart[i] - reinterpret_cast<uintptr_t>(span.file.data());
                constexpr unsigned ahead = 1;
                std::array<PreparedMasks, ahead> queue;
                unsigned cursor = 0;
                auto storeEvent = [&](const ParsedEvent& event, uint64_t offset, uint64_t rawHash) {
                    const EventToken token = event.id;
                    const size_t idShard = uint64_t{event.idHash} >> idShift;
                    uint64_t info;
                    if (event.valid) {
                        const auto bucket = static_cast<uint32_t>(partOf(rawHash));
                        const uint32_t index =
                            reader.events.add(bucket, Event::pack(event.user, ctx.start, event.row, rawHash));
                        info = location(reader,event.arrival,arrivalOrigin,bucket,index);
                    } else {
                        const uint64_t relative = relativeArrival(event.arrival,arrivalOrigin);
                        info = event.arrival != invalidTime && relative <= UINT32_MAX
                             ? (relative << 32) | invalidArrivalTag : special(offset,sourceTag);
                    }
                    reader.ids.add(idShard, IdRecord{token, info});
                };
                // Only events with a long shopper need a potentially cold table probe.
                // Leave their entries in a short reader-local queue while continuing to
                // consume the CSV. Each ID record is emitted together with its locator.
                struct PendingShopper { ParsedEvent event; uint64_t offset, hash; };
                constexpr unsigned depth = 8;
                // The next physical slot is always separate from the eight live
                // lookups, so a decoder can write into its final storage directly.
                constexpr unsigned slots = 2 * depth;
                std::array<PendingShopper, slots> pending;
                unsigned queued = 0;
                auto flush = [&](PendingShopper& item) {
                    item.event.user = Key::internHashedShopper(item.event.user.borrowed(), item.hash);
                    storeEvent(item.event, item.offset, item.hash);
                };
                auto accept = [&](const char *line, ParsedEvent& event) {
                    const uint64_t offset = base + reinterpret_cast<uintptr_t>(line);
                    const uint64_t hash = event.valid ? event.user.hash() : 0;
                    if (event.valid && event.user.external() && Key::shopperSlots) {
                        const auto width = event.user.borrowed().size();
                        if (width > 9 && width <= 48) {
                            __builtin_prefetch(&Key::shopperSlots[hash & Key::shopperMask], 0, 3);
                            if (queued >= depth) flush(pending[(queued - depth) % slots]);
                            auto& item = pending[queued % slots];
                            item.offset = offset;
                            item.hash = hash;
                            ++queued;
                            return;
                        }
                    }
                    storeEvent(event, offset, hash);
                };
                // Keep only a bounded group of input spans. Revisions are
                // compared by their recorded arrival, independently of parse order.
                std::array<std::string_view, 128> generic;
                unsigned genericCount = 0;
                auto drainGeneric = [&] {
                    for (unsigned j = 0; j < genericCount; ++j) {
                        auto line = generic[j];
                        auto& decoded = pending[queued % slots].event;
                        if (genericEvent(line, ctx, decoded, reader)) accept(line.data(),decoded);
                    }
                    genericCount = 0;
                };
                auto emit = [&](PreparedMasks& masks) {
                    if (masks.head) {
                        auto& decoded = pending[queued % slots].event;
                        if (parsePrepared(masks, ctx, decoded, reader)) accept(masks.line.data(),decoded);
                        return;
                    }
                    generic[genericCount++] = masks.line;
                    if (genericCount == generic.size()) drainGeneric();
                };
                eventLines(span, [&]() -> PreparedMasks & {
                    auto &m = queue[cursor % ahead];
                    if (cursor >= ahead)
                        emit(m);
                    ++cursor;
                    return m;
                });
                for (unsigned j = cursor - std::min(cursor, ahead); j < cursor; ++j)
                    emit(queue[j % ahead]);
                drainGeneric();
                for (unsigned j = queued - std::min(queued, depth); j < queued; ++j)
                    flush(pending[j % slots]);
            }
        }
        reader.events.finish();
        reader.ids.finish();
        reader.prepareCancellations(parts);
    });
    advice.finish();
    for(auto& reader:readers)reader->slab.stopHints();
    lap("parse");

    const size_t clicks = source.spans.size() - source.eventSpans;
    auto processShard = [&](size_t shard) {
        deduplicate(shard, arrivalOrigin, source, readers);
        std::vector<void*> spent;
        for (auto &reader : readers) {
            reader->ids.release(shard, spent);
        }
        recycled.give(spent);
    };
    auto processClicks = [&](size_t part, Reader &reader) {
        const auto& span = source.spans[source.eventSpans + part];
        if (ctx.populateInput) populateChunk(span);
        struct PendingClick { Click row; uint64_t hash; };
        std::array<PendingClick,16> queue;
        unsigned used=0;
        const auto store=[&](PendingClick& item) {
            auto& row=item.row;
            if (row.person.external() && Key::shopperSlots) {
                auto text=row.person.borrowed();
                if(text.size()>9 && text.size()<=48)
                    row.person=Key::internHashedShopper(text,item.hash);
            }
            row.hash32=uint32_t(item.hash>>32);
            reader.clicks.add(partOf(item.hash),row);
        };
        chunkLines(span,[&](std::string_view line) {
            Click row;
            if(!parseClick(line,ctx,row)) return;
            const auto hash=row.person.hash();
            if(row.person.external() && Key::shopperSlots) {
                auto text=row.person.borrowed();
                if(text.size()>9 && text.size()<=48)
                    __builtin_prefetch(&Key::shopperSlots[hash&Key::shopperMask],0,3);
            }
            auto& item=queue[used%queue.size()];
            if(used>=queue.size()) store(item);
            item={row,hash};
            ++used;
        });
        for(unsigned i=used-std::min<unsigned>(used,queue.size());i<used;++i)
            store(queue[i%queue.size()]);
    };

    const unsigned clickLanes = bytes < (256u << 20)
        ? std::max(1u, readerCount / 3) : ctx.settings.clickLanes;
    std::atomic<size_t> nextShard = 0, nextClick = 0;
    parallel(readerCount, readerCount, [&](size_t w) {
        Reader &reader = *readers[w];
        auto drainClicks = [&] {
            for (;;) {
                const size_t i = nextClick.fetch_add(1, std::memory_order_relaxed);
                if (i >= clicks) break;
                processClicks(i, reader);
            }
        };
        // A small click lane overlaps its arithmetic with the random ID probes.
        // First return one ID block chain per worker to prime the shared recycler.
        if (w < clickLanes && readerCount > 1) {
            const size_t i = nextShard.fetch_add(1, std::memory_order_relaxed);
            if (i < idShards) processShard(i);
            drainClicks();
        }
        for (;;) {
            const size_t i = nextShard.fetch_add(1, std::memory_order_relaxed);
            if (i >= idShards) break;
            processShard(i);
        }
        drainClicks();
        reader.clicks.finish();
    });
    for (auto& reader : readers) reader->releaseOpaque();
    lap("dedup");
    // Borrowed variable-width shopper and click spellings are still read by
    // canonical routing, session attribution and the Parquet writer.
    awaitIdentities();
    lap("await-links");

    std::vector<std::unique_ptr<Router>> routers(workers);
    std::atomic<size_t> nextPart = 0;
    parallel(workers, workers, [&](size_t w) {
        routers[w] = std::make_unique<Router>(out.size());
        Router& router = *routers[w];
        for (;;) {
            const size_t p = nextPart.fetch_add(1, std::memory_order_relaxed);
            if (p >= parts) break;
            for (auto& reader : readers) {
                size_t cancelOffset = reader->cancelBases[p];
                reader->events.pieces(p, [&](Event* rows, size_t n) {
                    const size_t keep = compactCancelled(rows, n, reader->cancelled.data()+cancelOffset);
                    cancelOffset += n;
                    correct(rows,keep,p,shift,ctx.identities,router.events,out[p].events,
                            [](const Event&) { return false; });
                });
                reader->clicks.pieces(p, [&](Click* rows, size_t n) {
                    correct(rows,n,p,shift,ctx.identities,router.clicks,out[p].clicks,
                            [](const Click&) { return false; });
                });
            }
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
    for (auto& reader : readers) {
        std::vector<uint8_t>().swap(reader->cancelled);
        (void)reader.release();
    }
    for (auto& router : routers) (void)router.release();
    lap("route");
}
