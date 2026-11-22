#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <cstdlib>
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

// Nine ASCII bytes have 63 payload bits. Pack them in lexical order so
// equality and tie-breaking avoid input reads. Up to seven raw bytes fit
// inline with a length tag. Other keys borrow the immutable mapped input.
struct Key {
    using Buffer = std::array<char,9>;
    static constexpr uint64_t shortTag = uint64_t{1} << 63;
    static constexpr uint64_t referenceTag = uint64_t{3} << 62;
    static constexpr uint64_t indirectTag = uint64_t{11} << 60;
    static constexpr uint64_t shopperTag = uint64_t{10} << 60;
    static constexpr uint64_t pointerMask = (uint64_t{1} << 48) - 1;
    static constexpr uint64_t asciiMask = 0x7f7f7f7f7f7f7f7fULL;
    static constexpr uint64_t eightTag = uint64_t{0x88} << 56;
    uint64_t bits = shortTag;
    bool eightAscii() const { return (bits >> 56) == 0x88; }
    bool needsDecoding() const { return nineAscii() || eightAscii(); }

    // Slots never move or change after publication. Exact equal shopper spellings
    // encountered by different workers can therefore share a single Key.
    struct ShopperSlot {
        uint32_t state;
        uint32_t pad;
        uint64_t hash;
        char spelling[48];
    };
    static_assert(sizeof(ShopperSlot) == 64);
    static inline ShopperSlot* shopperSlots = nullptr;
    static inline size_t shopperMask = 0;
    static void initializeShoppers(size_t inputBytes) {
        if (inputBytes < (1u << 20)) return;
        const size_t count = std::bit_floor(std::clamp<size_t>(inputBytes / 2048, 1024, 1u << 21));
        auto* pool = static_cast<ShopperSlot*>(std::calloc(count, sizeof(ShopperSlot)));
        if (!pool) return;
        if (reinterpret_cast<uintptr_t>(pool + count - 1) > pointerMask) {
            std::free(pool);
            return;
        }
        shopperSlots = pool;
        shopperMask = count - 1;
    }
    static uint64_t spellingHash(std::string_view s) {
#if !defined(__AES__)
        return std::hash<std::string_view>{}(s);
#else
        const size_t n = s.size();
        if (n < 8 || n > 64) return std::hash<std::string_view>{}(s);
        const auto seed = _mm_set_epi64x(INT64_C(0x2645ff95aabc9b37), int64_t(n));
        __m128i first, last;
        if (n > 32) {
            auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.data()));
            auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.data()+16));
            auto c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.data()+n-32));
            auto d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.data()+n-16));
            first = _mm_aesenc_si128(a,b);
            last = _mm_aesenc_si128(c,d);
        } else if (n <= 16) {
            first = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(s.data()));
            last = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(s.data()+n-8));
            first = _mm_unpacklo_epi64(first,last);
            last = seed;
        } else {
            first = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.data()));
            last = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.data()+n-16));
        }
        auto mixed = _mm_aesenc_si128(first,seed);
        mixed = _mm_aesenc_si128(mixed,last);
        mixed = _mm_aesenc_si128(mixed,seed);
        return static_cast<uint64_t>(_mm_cvtsi128_si64(mixed));
#endif
    }
    static Key internHashedShopper(std::string_view text, uint64_t hash) {
        const uint32_t ready = static_cast<uint32_t>(text.size()) + 2;
        for (const size_t index : {size_t(hash) & shopperMask, size_t(hash >> 32) & shopperMask}) {
            auto& slot = shopperSlots[index];
            std::atomic_ref<uint32_t> state(slot.state);
            uint32_t found = state.load(std::memory_order_acquire);
            if (!found && state.compare_exchange_strong(found, 1, std::memory_order_acq_rel)) {
                std::memcpy(slot.spelling, text.data(), text.size());
                slot.hash = hash;
                state.store(ready, std::memory_order_release);
            } else if (found != ready || slot.hash != hash ||
                       std::memcmp(slot.spelling, text.data(), text.size()) != 0) {
                continue;
            }
            return {shopperTag | (uint64_t{text.size()} << 48) | reinterpret_cast<uintptr_t>(&slot)};
        }
        return fromReference(text);
    }
    static Key internShopper(std::string_view text) {
        if (text.size() > 48 || !shopperSlots) return fromReference(text);
        return internHashedShopper(text, spellingHash(text));
    }
    static Key shopper(std::string_view text) {
        return text.size() > 9 ? internShopper(text) : from(text);
    }
    bool interned() const { return bits >> 60 == shopperTag >> 60; }
    const ShopperSlot* shopperSlot() const {
        return reinterpret_cast<const ShopperSlot*>(bits & pointerMask);
    }

    static constexpr uint64_t headerMask = 0x0080808080808080ULL;
    static constexpr uint64_t scatterPrefix(unsigned char c) {
        uint64_t r = 0;
        for (unsigned i = 0; i < 7; ++i)
            r |= uint64_t((c >> i) & 1) << (7 + 8 * i);
        return r;
    }
    template <unsigned Hint = 255> static Key nine(const char *p) {
        uint64_t suffix;
        std::memcpy(&suffix, p + 1, 8);
        unsigned c = static_cast<unsigned char>(*p);
        if ((suffix & ~asciiMask) || c >= 128)
            return fromReference({p, 9});
        uint64_t marks;
        if constexpr (Hint != 255) {
            marks = (c == Hint) ? scatterPrefix(Hint) : _pdep_u64(c, headerMask);
        } else
            marks = _pdep_u64(c, headerMask);
        return {__builtin_bswap64(suffix) | marks};
    }
    unsigned prefix() const { return _pext_u64(bits, headerMask); }
    void writeNine(char *target) const {
        target[0] = static_cast<char>(prefix());
        uint64_t value = __builtin_bswap64(bits & asciiMask);
        std::memcpy(target + 1, &value, 8);
    }
    static Key from(std::string_view s) {
        if (s.size() == 9)
            return nine(s.data());
        if (s.size() == 8) {
            uint64_t bytes;
            std::memcpy(&bytes, s.data(), 8);
            if (!(bytes & ~asciiMask)) return {eightTag | _pext_u64(bytes, asciiMask)};
        }
        if (s.size() < 8) {
            uint64_t value = 0;
            if (s.size() >= 4) {
                uint32_t first,last;
                std::memcpy(&first,s.data(),4);
                std::memcpy(&last,s.data()+s.size()-4,4);
                value = uint64_t{first} | (uint64_t{last} << (8*(s.size()-4)));
            } else if (!s.empty()) {
                value = static_cast<unsigned char>(s[0]);
                if (s.size() >= 2) {
                    uint16_t last;
                    std::memcpy(&last,s.data()+s.size()-2,2);
                    value |= uint64_t{last} << (8*(s.size()-2));
                }
            }
            return {shortTag | (uint64_t{s.size()} << 56) | value};
        }
        return fromReference(s);
    }
    static Key fromReference(std::string_view s) {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(s.data());
        if (s.size() < (1u<<14) && addr <= pointerMask)
            return {referenceTag | (uint64_t{s.size()} << 48) | addr};
        // Huge fields (or high virtual addresses) need the full pointer and
        // length. Input mappings and these pages both live until process exit.
        static thread_local std::string_view* page;
        static thread_local unsigned used = 512;
        if (used == 512) { page = new std::string_view[512]; used = 0; }
        auto* saved = &page[used++];
        *saved = s;
        return {indirectTag | reinterpret_cast<uintptr_t>(saved)};
    }
    bool empty() const { return bits == shortTag; }
    bool nineAscii() const { return !(bits & shortTag); }
    bool external() const { return bits >= shopperTag; }
    std::string_view borrowed() const {
        if (interned()) return {shopperSlot()->spelling, (bits >> 48) & 63};
        if (bits >= referenceTag)
            return {reinterpret_cast<const char*>(bits & pointerMask), (bits >> 48) & 0x3fff};
        if (external())
            return *reinterpret_cast<const std::string_view*>(bits & ((uint64_t{1}<<60)-1));
        return {reinterpret_cast<const char*>(&bits), (bits >> 56) & 7};
    }
    std::string_view view(Buffer& buffer) const {
        if (eightAscii()) {
            uint64_t bytes = _pdep_u64(bits, asciiMask);
            std::memcpy(buffer.data(), &bytes, 8);
            return {buffer.data(), 8};
        }
        if (!nineAscii()) return borrowed();
        writeNine(buffer.data());
        return {buffer.data(),9};
    }
    uint64_t hash() const {
        if (interned()) return shopperSlot()->hash;
        if (external()) return spellingHash(borrowed());
        const __uint128_t product = static_cast<__uint128_t>(bits ^ 0xa0761d6478bd642fULL)
                                    * (bits ^ 0xe7037ed1a0b428dbULL);
        return static_cast<uint64_t>(product) ^ static_cast<uint64_t>(product >> 64);
    }
    bool operator==(const Key& o) const {
        return bits == o.bits || (external() && o.external() && borrowed()==o.borrowed());
    }
    bool operator<(const Key& o) const {
        if (nineAscii() && o.nineAscii()) {
            if (((bits ^ o.bits) & headerMask) != 0)
                return prefix() < o.prefix();
            return bits < o.bits;
        }
        Buffer a,b;
        return view(a) < o.view(b);
    }
};
static_assert(sizeof(Key)==8 && std::endian::native==std::endian::little);
#include "shopper_codec.hpp"
inline ShopperCodec shopperCodec;
inline uint32_t compactBucketShift=0, compactRemainder=UINT32_MAX;
inline bool fitsShopperReceipt(Key person) {
    return shopperCodec.buckets && (person.bits & ~shopperCodec.variable)==shopperCodec.invariant;
}
inline uint32_t receiptBucket(Key person) {
    const uint32_t mixed=_mm_crc32_u64(0,person.bits&shopperCodec.lowRawMask);
    const uint32_t upper=uint32_t(_pext_u64(person.bits,shopperCodec.highRawMask));
    const auto bits=shopperCodec.partBits;
    return upper^(bits?mixed>>(32-bits):0);
}
inline size_t physicalOwnerBucket(Key person,uint64_t originalHash,unsigned shift) {
    if (fitsShopperReceipt(person)) return receiptBucket(person);
    return (originalHash>>1)>>shift;
}
inline Key restoreReceiptOwner(uint32_t partition,uint64_t receipt) {
    const uint64_t low=_pdep_u64(receipt,shopperCodec.lowRawMask);
    const uint32_t mixed=_mm_crc32_u64(0,low);
    const auto bits=shopperCodec.partBits;
    const uint32_t upper=partition^(bits?mixed>>(32-bits):0);
    return Key{shopperCodec.invariant|low|_pdep_u64(upper,shopperCodec.highRawMask)};
}
void learnShopperEncoding(const std::vector<std::string_view>&,size_t,size_t);

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

template<size_t N, bool Long, bool Wide = false>
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
        if constexpr (Wide) {
            auto third = classify32(p + 64);
            upperCommas = third.comma | (uint64_t(last.comma) << (size - 96));
            upperQuotes = third.quote | (uint64_t(last.quote) << (size - 96));
        } else {
            upperCommas = last.comma >> (96-size);
            upperQuotes = last.quote >> (96-size);
        }
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
  template <class Fn> void visit(Fn &&fn) const {
      for (const auto &slot : slots)
          if (slot.value != UINT32_MAX)
              fn(slot.key, slot.value);
  }
    void reserve(size_t n);
    uint32_t insert(Key key, uint32_t value);
    uint32_t find(const Key &key) const { return slots.empty() ? UINT32_MAX : slots[locate(key)].value; }
};

// An inline key family with only a few varying bits has an exact dense
// address. Constant bits are checked first, then BMI2 compresses the rest.
class DimensionIndex : public Index {
    struct Family {
        uint64_t fixed = 0, variable = 0, fixedMask = 0;
        uint32_t base = UINT32_MAX;
    };
    std::array<Family, 8> families;
    std::vector<uint32_t> values;
    bool ready = false;
    static unsigned group(Key key) { return static_cast<unsigned>((key.bits >> 56) - 0x80); }

  public:
    uint32_t insert(Key key, uint32_t value) {
        if (ready) {
            families = {};
            values.clear();
            ready = false;
        }
        return Index::insert(key, value);
    }
    void finish();
    uint32_t lookup(std::string_view text, const char *limit) const {
        size_t n = text.size();
        if (n < families.size()) {
            const auto &f = families[n];
            if (f.base != UINT32_MAX && limit - text.data() >= 8) {
                uint64_t raw;
                std::memcpy(&raw, text.data(), 8);
                if ((raw & f.fixedMask) != f.fixed)
                    return UINT32_MAX;
                return values[f.base + _pext_u64(raw, f.variable)];
            }
        }
        return find(Key::from(text));
    }
};

// Only aliases occupy the immutable lookup table. Canonical and unlinked IDs
// both fall through to the input key.
struct Identities {
    static constexpr unsigned shardBits = 7;
    static constexpr size_t shards = 1u << shardBits;
    struct Alias { Key raw, name; };
    std::vector<Alias> slots;
    // Each home slot records every four-bit fingerprint inserted there,
    // including keys ultimately displaced by linear probing. This compact
    // directory can reject most missing keys without touching any Key.
    std::vector<uint16_t> fingerprints;
    static uint16_t fingerprint(uint64_t hash) { return static_cast<uint16_t>(1u << ((hash >> 32) & 15)); }
    size_t shardSize = 0;
    unsigned slotShift = 0;  // 64 - shardBits - log2(shardSize)
    size_t home(uint64_t hash) const { return (hash >> slotShift) & (shardSize - 1); }

    static size_t shard(uint64_t hash) { return hash >> (64 - shardBits); }
    // hash initially belongs to raw. Return the canonical key and its hash
    // together so unlinked rows never hash their person twice.
    Key person(const Key& raw, uint64_t& hash) const {
        if (slots.empty()) return raw;
        const size_t start = shard(hash) * shardSize;
        size_t mask = shardSize - 1, pos = home(hash);
        if (!(fingerprints[start + pos] & fingerprint(hash))) return raw;
        const Alias* table = slots.data() + start;
        while (!table[pos].raw.empty()) {
            if (table[pos].raw == raw) {
                hash = table[pos].name.hash();
                return table[pos].name;
            }
            pos = (pos + 1) & mask;
        }
        return raw;
    }
};

struct Context {
    bool populateInput = false;
    struct ExecutionSettings {
        unsigned userBits, idBits, chunkBits, linkWorkers, clickLanes;
    };
    ExecutionSettings settings{10, 9, 21, 3, 2};

    int64_t start, end;
    Identities identities;
    DimensionIndex products, campaigns;
    std::vector<Key> categories, campaignNames, channels;
    uint32_t unknown;
    uint32_t category(Key product) const {
        uint32_t value = products.find(product);
        return value == UINT32_MAX ? unknown : value;
    }
    uint32_t campaign(Key id) const { return campaigns.find(id); }
};

struct EventData { int64_t ts; uint32_t price, category; };
struct Event {
    Key person;
    uint64_t bits;
    static constexpr uint64_t overflowFlag = uint64_t{1} << 63;
    // An ordinary short-window event has room for the high 32 hash bits used
    // by every person partition and fingerprint. Rich payloads recompute them.
    static Event pack(Key person, int64_t origin, EventData row, uint64_t hash) {
        const uint64_t off = uint64_t(row.ts) - uint64_t(origin);
        if (!row.price && off < (uint64_t{1} << 31))
            return {person, off | ((hash >> 32) << 31)};
        if (off <= UINT32_MAX && row.price < (1u << 23) && row.category < 128)
            return {person,
                    (uint64_t{2} << 62) | off | (uint64_t(row.price) << 32) | (uint64_t(row.category) << 55)};
        static thread_local EventData* page;
        static thread_local unsigned used = 1024;
        if (used == 1024) {
            page = new EventData[1024];
            used = 0;
        }
        auto *v = &page[used++];
        *v = row;
        return {person, (uint64_t{3} << 62) | reinterpret_cast<uintptr_t>(v)};
    }
    uint64_t routingHash() const {
        if (!(bits >> 63))
            return uint64_t(uint32_t(bits >> 31)) << 32;
        return person.hash();
    }
    void rename(Key to, uint64_t hash) {
        person = to;
        if (!(bits >> 63))
            bits = (bits & 0x7fffffff) | ((hash >> 32) << 31);
    }
    EventData unpack(int64_t origin) const {
        if (!(bits >> 63))
            return {origin + int64_t(bits & 0x7fffffff), 0, 0};
        if ((bits >> 62) == 2)
            return {origin + uint32_t(bits), uint32_t(bits >> 32) & 0x7fffff, uint32_t(bits >> 55) & 127};
        return *reinterpret_cast<const EventData *>(bits & ~(uint64_t{3} << 62));
    }
};
struct Click {
    Key person, id;
    int64_t ts;
    uint32_t campaign;
    uint32_t hash32{};
    uint64_t routingHash() const { return uint64_t(hash32) << 32; }
    void rename(Key to, uint64_t h) {
        person = to;
        hash32 = h >> 32;
    }
};
// A bucket's rows stay in the blocks the ingest wrote them to.
template<class T> struct Pieces {
    std::vector<std::pair<const T*, size_t>> spans;
    size_t size = 0;
    void add(const T* rows, size_t count) { spans.emplace_back(rows, count); size += count; }
    template<class F> void each(F&& visit) const {
        for (auto [rows, count] : spans)
            for (size_t i = 0; i < count; ++i) visit(rows[i]);
    }
};
struct Bucket {
    Pieces<Event> events;
    Pieces<Click> clicks;
    Pieces<uint64_t> compact;
    size_t partition{};
    size_t eventCount() const { return events.size+compact.size; }
};
struct UserRow { Key person; int32_t day; uint32_t sessions, events; int64_t revenue; uint32_t top; };
struct CampaignStats { int64_t sessions=0, purchasing=0, revenue=0, assists=0; };
struct DayCounters {
    struct Credit {
        uint32_t sessions = 0, purchasing = 0;
        int64_t revenue = 0;
    };
    std::vector<Credit> credits;
    std::vector<uint32_t> assists;
    // Revenue is already 64-bit. Only the bounded counters need a carry.
    struct Carried {
        int64_t sessions = 0, purchasing = 0, assists = 0;
    };
    std::vector<Carried> carried;
    explicit DayCounters(size_t n) : credits(n), assists(n) {}
    size_t size() const { return assists.size(); }
    void credit(size_t id, int64_t rev) {
        auto &c = credits[id];
        ++c.sessions;
        if (rev) {
            ++c.purchasing;
            c.revenue += rev;
        }
    }
    void addAssists(size_t id, uint32_t n) { assists[id] += n; }
    CampaignStats local(size_t i) const {
        auto &c = credits[i];
        return {c.sessions, c.purchasing, c.revenue, assists[i]};
    }
    CampaignStats total(size_t i) const {
        CampaignStats r = local(i);
        if (!carried.empty()) {
            r.sessions += carried[i].sessions;
            r.purchasing += carried[i].purchasing;
            r.assists += carried[i].assists;
        }
        return r;
    }
    void carryCounts() {
        if (carried.empty())
            carried.resize(size());
        for (size_t i = 0; i < size(); ++i) {
            auto r = local(i);
            carried[i].sessions += r.sessions;
            carried[i].purchasing += r.purchasing;
            carried[i].assists += r.assists;
            credits[i].sessions = credits[i].purchasing = 0;
            assists[i] = 0;
        }
    }
};

// Numeric buffers are kept in their output order through aggregation so the
// Parquet worker can pass them directly to WriteBatch.
#include "delta_names.hpp"
struct UserColumns {
    DeltaNames names;
    void person(Key key, uint32_t count) {
        Key::Buffer buffer;
        names.append(key.view(buffer),count);
    }
    std::vector<int32_t> days;
    std::vector<uint32_t> sessions, events;
    std::vector<int64_t> revenue;
    std::vector<uint32_t> top;
    void reserve(size_t n) {
        days.reserve(n);
        sessions.reserve(n);
        events.reserve(n);
        revenue.reserve(n);
        top.reserve(n);
    }
    void append(const UserRow& row) {
        days.push_back(row.day);
        sessions.push_back(row.sessions);
        events.push_back(row.events);
        revenue.push_back(row.revenue);
        top.push_back(row.top);
    }
};
struct Result {
    UserColumns users;
    std::unordered_map<int32_t, DayCounters> byDay;
    uint64_t eventsSinceCarry = 0;
    // Each campaign receives at most one credit and one assist per session.
    // The event total bounds each counter even for a single enormous shopper.
    void startBucket(uint64_t events) {
        if (events > UINT32_MAX - eventsSinceCarry) {
            for (auto &item : byDay)
                item.second.carryCounts();
            eventsSinceCarry = 0;
        }
        eventsSinceCarry += events;
    }
};

void linkPeople(const std::vector<std::string_view>&, Context&, unsigned);
// Parsing does not need identities; they are awaited before rows are routed to people.
int64_t timestamp(std::string_view value);
void ingest(const Context&, const std::vector<std::string_view>&, const std::vector<std::string_view>&,
            std::vector<Bucket>&, unsigned, const std::function<void()>& awaitIdentities);
void aggregate(Bucket&, Result&, size_t categoryCount, size_t campaignCount, int64_t origin);
void writeOutputs(const std::filesystem::path&, const Context&, std::vector<Result>&, unsigned, const std::function<bool(size_t)>&);
