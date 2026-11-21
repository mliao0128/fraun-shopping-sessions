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
    uint64_t offset = 0, rawBytes = 0, storedBytes = 0;
};
struct Group { size_t rows; std::vector<Chunk> columns; };

class Writer {
    std::ofstream stream;
    std::span<const Field> schema;
    std::vector<Group> groups;
    std::vector<char> compressed;
    std::string header, values, levels, combined;
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
    void page(Chunk& c, string_view raw, size_t n) {
        if (raw.size() > INT32_MAX) throw std::length_error("Parquet page too large");
        const int bytes = static_cast<int>(raw.size());
        const int bound = LZ4_compressBound(bytes);
        if (!bound) throw std::length_error("Parquet page exceeds LZ4 maximum");
        if (compressed.size() < size_t(bound)) compressed.resize(bound);
        const int size = LZ4_compress_fast(raw.data(),compressed.data(),bytes,bound,2);
        if (!size) throw std::runtime_error("LZ4 compression failed");
        header.clear();
        Compact h{header};
        h.integer(1,I32,0); // DATA_PAGE (v1)
        h.integer(2,I32,bytes);
        h.integer(3,I32,size);
        h.nested(5,[&](Compact& d) {
            d.integer(1,I32,n); d.integer(2,I32,plain);
            d.integer(3,I32,rle); d.integer(4,I32,rle);
        });
        h.finish();
        emit(header);
        emit({compressed.data(),size_t(size)});
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
                    d.list(2,I32,field.nullable ? 2 : 1);
                    varint(d.out,plain << 1);
                    if (field.nullable) varint(d.out,rle << 1);
                    d.list(3,Binary,1); d.bytes(field.name);
                    d.integer(4,I32,lz4Raw); d.integer(5,I64,group.rows);
                    d.integer(6,I64,c.rawBytes); d.integer(7,I64,c.storedBytes);
                    d.integer(9,I64,c.offset);
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
