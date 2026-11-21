#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Public LZ4 block ABI provided by liblz4.so.1 in Debian bookworm.
extern "C" int LZ4_compressBound(int);
extern "C" int LZ4_compress_fast(const char*, char*, int, int, int);

namespace flat {
using std::string_view;
enum Physical { Int32 = 1, Int64 = 2, Bytes = 6 };
enum CompactType { I32 = 5, I64 = 6, Binary = 8, List = 9, Structure = 12 };
constexpr int plain = 0, rle = 3, lz4Raw = 7;
struct Field { string_view name; Physical type; bool nullable = false; };

inline void appendU32(std::string& out, uint32_t v) {
    out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}
inline void varint(std::string& out, uint64_t v) {
    while (v >= 128) { out.push_back(static_cast<char>((v & 127) | 128)); v >>= 7; }
    out.push_back(static_cast<char>(v));
}

// All fields here are in strictly increasing order, with gaps of at most 15.
struct Compact {
    std::string& out;
    unsigned previous = 0;
    void field(unsigned id, CompactType type) {
        out.push_back(static_cast<char>(((id - previous) << 4) | type));
        previous = id;
    }
    void integer(unsigned id, CompactType type, uint64_t v) {
        field(id,type);
        varint(out,v << 1); // all metadata integers emitted here are nonnegative
    }
    void bytes(string_view v) {
        varint(out,v.size());
        out.append(v);
    }
    void string(unsigned id,string_view v) { field(id,Binary); bytes(v); }
    void list(unsigned id,CompactType type,size_t n) {
        field(id,List);
        out.push_back(static_cast<char>((std::min<size_t>(15,n) << 4) | type));
        if (n >= 15) varint(out,n);
    }
    template<class F> void element(F body) {
        Compact child{out};
        body(child);
        out.push_back(0);
    }
    template<class F> void nested(unsigned id,F body) { field(id,Structure); element(body); }
    void finish() { out.push_back(0); }
};
struct Chunk {
    uint64_t offset = 0, rawBytes = 0, storedBytes = 0, firstData = 0;
    bool hasDictionary = false;
    uint8_t dataEncoding = plain;
    uint8_t codec = lz4Raw;
};
struct Group { size_t rows; std::vector<Chunk> columns; };

class Writer {
    std::ofstream stream;
    std::span<const Field> schema;
    std::vector<Group> groups;
    std::vector<char> compressed;
    std::vector<int64_t> widened;
    std::string header, values, levels, combined, indices;
    uint64_t written = 0, totalRows = 0;
    static constexpr size_t targetPage = 1 << 20;
    void emit(string_view s) {
        stream.write(s.data(), static_cast<std::streamsize>(s.size()));
        written += s.size();
    }
    Chunk& column() {
        auto& chunks = groups.back().columns;
        if (chunks.size() == schema.size()) throw std::logic_error("Too many columns");
        return chunks.emplace_back(Chunk{written});
    }
    void page(Chunk &c, string_view raw, size_t n, int encoding = plain, bool dictionary = false) {
        if (dictionary)
            c.hasDictionary = true;
        else if (!c.firstData)
            c.firstData = written;
        if (raw.size() > INT32_MAX)
            throw std::length_error("Parquet page too large");
        const int bytes = static_cast<int>(raw.size());
        string_view payload = raw;
        if (c.codec == lz4Raw) {
            const int bound = LZ4_compressBound(bytes);
            if (!bound) throw std::length_error("Parquet page exceeds LZ4 maximum");
            if (compressed.size() < static_cast<size_t>(bound))
                compressed.resize(bound);
            const int stored = LZ4_compress_fast(raw.data(), compressed.data(), bytes, bound, 2);
            if (!stored) throw std::runtime_error("LZ4 compression failed");
            payload = {compressed.data(), static_cast<size_t>(stored)};
        }
        const size_t size = payload.size();
        header.clear();
        Compact h{header};
        h.integer(1, I32, dictionary ? 2 : 0); // DATA_PAGE (v1)
        h.integer(2,I32,bytes);
        h.integer(3,I32,size);
        if (dictionary)
            h.nested(7, [&](Compact &d) {
                d.integer(1, I32, n);
                d.integer(2, I32, plain);
            });
        else
            h.nested(5, [&](Compact &d) {
                d.integer(1, I32, n);
                d.integer(2, I32, encoding);
                d.integer(3, I32, rle);
                d.integer(4, I32, rle);
            });
        h.finish();
        emit(header);
        emit(payload);
        c.rawBytes += header.size()+bytes;
        c.storedBytes += header.size()+size;
    }
    void writeSchema(Compact& f) const {
        f.list(2,Structure,schema.size()+1);
        f.element([&](Compact& s) { s.string(4,"schema"); s.integer(5,I32,schema.size()); });
        for (const Field& name : schema) f.element([&](Compact& s) {
            s.integer(1,I32,name.type); s.integer(3,I32,name.nullable); s.string(4,name.name);
            if (name.type != Int64) {
                const bool text = name.type == Bytes;
                s.integer(6,I32,text ? 0 : 6); // UTF8 / DATE converted types
                s.nested(10,[&](Compact& logical) { logical.nested(text ? 1 : 6, [](Compact&) {}); });
            }
        });
    }
    void writeGroup(Compact& m, const Group& group) const {
        if (group.columns.size() != schema.size()) throw std::logic_error("Incomplete Parquet row group");
        uint64_t raw = 0, stored = 0;
        m.list(1,Structure,schema.size());
        for (size_t j = 0; j < schema.size(); ++j) {
            const Field& field = schema[j];
            const Chunk& c = group.columns[j];
            raw += c.rawBytes; stored += c.storedBytes;
            m.element([&](Compact& chunk) {
                chunk.integer(2,I64,0); // deprecated ColumnChunk.file_offset
                chunk.nested(3,[&](Compact& d) {
                    d.integer(1,I32,field.type);
                    d.list(2, I32, 1 + size_t(field.nullable) + size_t(c.hasDictionary));
                    varint(d.out, c.dataEncoding << 1);
                    if (field.nullable) varint(d.out,rle << 1);
                    if (c.hasDictionary)
                        varint(d.out, 8u << 1);
                    d.list(3,Binary,1); d.bytes(field.name);
                    d.integer(4,I32,c.codec); d.integer(5,I64,group.rows);
                    d.integer(6,I64,c.rawBytes); d.integer(7,I64,c.storedBytes);
                    d.integer(9, I64, c.firstData);
                    if (c.hasDictionary)
                        d.integer(11, I64, c.offset);
                });
            });
        }
        m.integer(2,I64,raw); m.integer(3,I64,group.rows); m.integer(6,I64,stored);
    }
public:
    Writer(const std::filesystem::path& path,std::span<const Field> fields) : schema(fields) {
        stream.exceptions(std::ios_base::badbit | std::ios_base::failbit);
        stream.open(path,std::ios::binary);
        emit("PAR1");
    }
    void group(size_t n) {
        groups.push_back({n,{}});
        groups.back().columns.reserve(schema.size());
        totalRows += n;
    }
    template<class T> void numbers(const T* data,size_t n) {
        static_assert(std::is_same_v<T,int32_t> || std::is_same_v<T,int64_t>);
        auto& c = column();
        page(c,{reinterpret_cast<const char*>(data),n*sizeof(T)},n);
    }
    template <class T, unsigned Plane> static void bytePlane(const T *src, char *dst, size_t n) {
        using U = std::make_unsigned_t<T>;
        for (size_t i = 0; i < n; ++i)
            dst[i] = static_cast<char>(static_cast<U>(src[i]) >> (Plane * 8));
    }
    void userRuns(size_t n, const std::vector<Key> &keys, const std::vector<uint32_t> &counts,
                  size_t &run, size_t &consumed) {
        const size_t initial = run, skip = consumed;
        const auto fallback = [&] {
            run = initial;
            consumed = skip;
            strings<false>(n, [&](size_t) {
                const Key *k = &keys[run];
                if (++consumed == counts[run]) {
                    ++run;
                    consumed = 0;
                }
                return k;
            });
        };
        if (n < 256 || n > 65536) {
            fallback();
            return;
        }
        values.resize(n * 13);
        char *p = values.data();
        combined.clear();
        combined.push_back(16);
        size_t distinct = 0, remaining = n;
        while (remaining) {
            const Key &k = keys[run];
            if (k.nineAscii()) {
                const uint32_t len = 9;
                const uint64_t bytes = __builtin_bswap64(_pdep_u64(k.bits >> 7, Key::asciiMask));
                std::memcpy(p, &len, 4);
                std::memcpy(p + 4, &bytes, 8);
                p[12] = static_cast<char>(k.bits & 127);
                p += 13;
            } else if (k.eightAscii()) {
                const uint32_t len = 8;
                const uint64_t bytes = _pdep_u64(k.bits, Key::asciiMask);
                std::memcpy(p, &len, 4);
                std::memcpy(p + 4, &bytes, 8);
                p += 12;
            } else if (!k.external()) {
                const uint32_t len = (k.bits >> 56) & 7;
                std::memcpy(p, &len, 4);
                std::memcpy(p + 4, &k.bits, 8);
                p += 4 + len;
            } else {
                fallback();
                return;
            }
            const size_t count = std::min(remaining, size_t(counts[run]) - consumed);
            remaining -= count;
            consumed += count;
            if (consumed == counts[run]) {
                ++run;
                consumed = 0;
            }
            varint(combined, count * 2);
            const uint16_t index = distinct++;
            combined.append(reinterpret_cast<const char *>(&index), 2);
        }
        auto &c = column();
        page(c, {values.data(), static_cast<size_t>(p - values.data())}, distinct, plain, true);
        page(c, combined, n, 8);
    }
    template<unsigned Width> void smallCountPage(const uint32_t *src, size_t n, uint32_t largest) {
        std::array<int64_t, 64> dictionary;
        for (uint32_t i = 0; i <= largest; ++i)
            dictionary[i] = i;
        auto &c = column();
        c.codec = 0;
        page(c, {reinterpret_cast<const char *>(dictionary.data()), (largest + 1) * sizeof(int64_t)}, largest + 1, plain, true);
        const size_t groups = (n + 7) / 8;
        combined.clear();
        combined.push_back(Width);
        varint(combined, (groups << 1) | 1);
        const size_t header = combined.size();
        combined.resize(header + groups * Width);
        indices.resize(groups * 8);
        bytePlane<uint32_t, 0>(src, indices.data(), n);
        std::memset(indices.data() + n, 0, groups * 8 - n);
        constexpr uint64_t mask = 0x0101010101010101ULL * ((1u << Width) - 1);
        for (size_t j = 0; j < groups; ++j) {
            uint64_t eight;
            std::memcpy(&eight, indices.data() + 8 * j, 8);
            const uint64_t packed = _pext_u64(eight, mask);
            std::memcpy(combined.data() + header + Width * j, &packed, Width);
        }
        page(c, combined, n, 8);
    }

    // Most counts fit in the first 64 entries. Less frequent values get a
    // dictionary entry on demand, preserving the full 32-bit count.
    bool countDictionary(const uint32_t *src, size_t n) {
        if (n < 256)
            return false;
        uint32_t largest = 0;
        for (size_t i = 0; i < n; ++i)
            largest = std::max(largest, src[i]);
        if (largest < 64) {
            if (largest < 16)
                smallCountPage<4>(src, n, largest);
            else if (largest < 32)
                smallCountPage<5>(src, n, largest);
            else
                smallCountPage<6>(src, n, largest);
            return true;
        }
        constexpr unsigned common = 64;
        std::array<uint32_t, 256> keys;
        std::array<uint16_t, 256> slots;
        slots.fill(UINT16_MAX);
        std::array<int64_t, 256> dictionary;
        for (unsigned j = 0; j < common; ++j)
            dictionary[j] = j;
        unsigned used = common;
        indices.resize(n);
        bytePlane<uint32_t, 0>(src, indices.data(), n);
        const auto ceiling = _mm256_set1_epi32(common - 1);
        for (size_t i = 0; i < n; i += 8) {
            unsigned exceptions = 0;
            if (i + 8 <= n) {
                const auto v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + i));
                const auto inRange = _mm256_cmpeq_epi32(_mm256_min_epu32(v, ceiling), v);
                exceptions = ~static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(inRange))) & 255;
            } else {
                for (size_t j = i; j < n; ++j)
                    exceptions |= static_cast<unsigned>(src[j] >= common) << (j - i);
            }
            while (exceptions) {
                const size_t row = i + std::countr_zero(exceptions);
                exceptions &= exceptions - 1;
                const uint32_t value = src[row];
                unsigned slot = (value * 0x9e3779b1u) >> 24;
                while (slots[slot] != UINT16_MAX && keys[slot] != value)
                    slot = (slot + 1) & 255;
                if (slots[slot] == UINT16_MAX) {
                    if (used == dictionary.size())
                        return false;
                    keys[slot] = value;
                    slots[slot] = used;
                    dictionary[used++] = value;
                }
                indices[row] = static_cast<char>(slots[slot]);
            }
        }
        auto &c = column();
        c.codec = 0;
        page(c, {reinterpret_cast<const char *>(dictionary.data()), used * sizeof(int64_t)}, used, plain, true);
        combined.clear();
        dictionaryIndices(combined, n);
        page(c, combined, n, 8);
        return true;
    }

    void days(const int32_t *src, size_t n) {
        if (n < 256) {
            numbers(src, n);
            return;
        }
        int32_t low = src[0], high = src[0];
        for (size_t j = 1; j < n; ++j) {
            low = std::min(low, src[j]);
            high = std::max(high, src[j]);
        }
        const int64_t range = int64_t(high) - low;
        if (range > 15) {
            numbers(src, n);
            return;
        }
        const size_t entries = static_cast<size_t>(range) + 1;
        std::array<int32_t, 16> dictionary;
        for (size_t j = 0; j < entries; ++j)
            dictionary[j] = static_cast<int32_t>(int64_t(low) + static_cast<int64_t>(j));
        auto &c = column();
        c.codec = 0;
        page(c, {reinterpret_cast<const char *>(dictionary.data()), entries * sizeof(int32_t)}, entries, plain, true);
        combined.clear();
        combined.push_back(4);
        const size_t groups = (n + 7) / 8;
        varint(combined, (groups << 1) | 1);
        const size_t headerBytes = combined.size();
        combined.resize(headerBytes + 4 * groups);
        indices.resize(groups * 8);
        for (size_t j = 0; j < n; ++j)
            indices[j] = static_cast<char>(uint32_t(src[j]) - uint32_t(low));
        std::memset(indices.data() + n, 0, groups * 8 - n);
        for (size_t j = 0; j < groups; ++j) {
            uint64_t eight;
            std::memcpy(&eight, indices.data() + j * 8, 8);
            const uint32_t packed = static_cast<uint32_t>(_pext_u64(eight, 0x0f0f0f0f0f0f0f0fULL));
            std::memcpy(combined.data() + headerBytes + 4 * j, &packed, 4);
        }
        page(c, combined, n, 8);
    }
    void counts64(const uint32_t *src, size_t n) {
        if (countDictionary(src, n))
            return;
        widened.assign(src, src + n);
        numbers(widened.data(), n);
    }

    void dictionaryIndices(std::string &out, size_t n) {
        out.push_back(8);
        if (!n)
            return;
        const size_t groups = (n + 7) / 8;
        varint(out, (groups << 1) | 1);
        out.append(indices.data(), n);
        out.append(groups * 8 - n, '\0');
    }
    void categories(const uint32_t *ids, size_t n, const std::vector<Key> &keys) {
        if (keys.empty() || keys.size() > 256 || n < 256) {
            strings<true>(n, [&](size_t i) { return ids[i] == UINT32_MAX ? nullptr : &keys[ids[i]]; });
            return;
        }
        values.clear();
        for (const Key &key : keys) {
            Key::Buffer tmp;
            auto v = key.view(tmp);
            if (v.size() > targetPage || values.size() + v.size() + 4 > targetPage) {
                strings<true>(n,
                              [&](size_t i) { return ids[i] == UINT32_MAX ? nullptr : &keys[ids[i]]; });
                return;
            }
            appendU32(values, v.size());
            values.append(v);
        }
        auto &c = column();
        page(c, values, keys.size(), plain, true);
        size_t bytes = (n + 7) / 8;
        combined.clear();
        appendU32(combined, 0);
        varint(combined, (bytes << 1) | 1);
        const size_t start = combined.size();
        combined.resize(start + bytes);
        const uint32_t length = static_cast<uint32_t>(combined.size() - 4);
        std::memcpy(combined.data(), &length, 4);
        indices.resize(n);
        size_t nonnull = 0;
        for (size_t i = 0; i < n; i += 8) {
            uint32_t mask = 0;
            if (i + 8 <= n) {
                auto v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(ids + i));
                auto missing = _mm256_cmpeq_epi32(v, _mm256_set1_epi32(-1));
                mask = (~_mm256_movemask_ps(_mm256_castsi256_ps(missing))) & 255;
            } else
                for (size_t j = i; j < n; ++j)
                    mask |= uint32_t(ids[j] != UINT32_MAX) << (j - i);
            combined[start + i / 8] = static_cast<char>(mask);
            while (mask) {
                auto j = std::countr_zero(mask);
                mask &= mask - 1;
                indices[nonnull++] = static_cast<char>(ids[i + j]);
            }
        }
        dictionaryIndices(combined, nonnull);
        page(c, combined, n, 8);
    }
    template<bool nullable, class F> void strings(size_t n,F keyAt) {
        auto& c = column();
        size_t i = 0;
        while (i < n) {
            const size_t start = i;
            values.clear();
            values.reserve(std::min(n-start,size_t(65536))*13);
            levels.clear();
            while (i < n && values.size() < targetPage) {
                const Key* key = keyAt(i);
                if constexpr (nullable) {
                    const unsigned bit = (i-start) % 8;
                    if (!bit) levels.push_back(0);
                    levels.back() |= static_cast<char>((key != nullptr) << bit);
                }
                if (key) {
                    Key::Buffer decoded;
                    auto v = key->view(decoded);
                    if (v.size() > INT32_MAX) throw std::length_error("Parquet string too long");
                    appendU32(values,static_cast<uint32_t>(v.size()));
                    values.append(v);
                }
                ++i;
            }
            if constexpr (nullable) {
                // One bit-packed hybrid run. Pad the final 8-row group with zeros.
                combined.clear();
                appendU32(combined,0);
                varint(combined,(levels.size() << 1) | 1);
                combined.append(levels);
                const uint32_t length = static_cast<uint32_t>(combined.size()-4);
                std::memcpy(combined.data(),&length,4);
                combined.append(values);
                page(c,combined,i-start);
            } else page(c,values,i-start);
        }
    }
    void close() {
        std::string footer;
        Compact meta{footer};
        meta.integer(1,I32,1);
        writeSchema(meta);
        meta.integer(3,I64,totalRows);
        meta.list(4,Structure,groups.size());
        for (const Group& g : groups) meta.element([&](Compact& m) { writeGroup(m,g); });
        meta.finish();
        if (footer.size() > UINT32_MAX) throw std::length_error("Parquet footer too large");
        appendU32(footer,static_cast<uint32_t>(footer.size()));
        footer.append("PAR1");
        emit(footer);
        stream.close();
    }
};
}
