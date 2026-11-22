#pragma once
#include <array>
#include <climits>
#include <immintrin.h>
#include <bit>
#include <span>
#include <string>
#include <vector>

struct DeltaNames {
    struct Page { std::string bytes; uint32_t count; };
    std::vector<Page> pages;
    std::vector<int32_t> prefix, suffix;
    std::string bytes, encoded;

    static constexpr size_t rowGroup = 65536;
    size_t rowsInGroup = 0;
    std::vector<std::vector<Page>> groups;

    static void varint(std::string& out, uint64_t value) {
        while (value >= 128) {
            out.push_back(static_cast<char>((value & 127) | 128));
            value >>= 7;
        }
        out.push_back(static_cast<char>(value));
    }
    static uint64_t zigzag(int64_t value) {
        return (uint64_t(value) << 1) ^ (uint64_t(0) - (uint64_t(value) >> 63));
    }
    static void deltas(std::string& out, std::span<const int32_t> src) {
        varint(out,128); varint(out,4); varint(out,src.size());
        int32_t prev=src.front();
        varint(out,zigzag(prev));
        for(size_t start=1;start<src.size();start+=128) {
            const size_t n=std::min<size_t>(128,src.size()-start);
            std::array<int32_t,128> delta{};
            int32_t minimum=INT32_MAX;
            for(size_t j=0;j<n;++j) {
                const int32_t next=src[start+j];
                delta[j]=next-prev;
                minimum=std::min(minimum,delta[j]);
                prev=next;
            }
            varint(out,zigzag(minimum));
            size_t head=out.size();
            out.resize(head+4,0);
            for(size_t i=0;i<n;i+=32) {
                std::array<uint32_t,32> residual{};
                uint32_t mask=0;
                for(size_t j=0;j<std::min<size_t>(32,n-i);++j) {
                    residual[j]=uint32_t(delta[i+j])-uint32_t(minimum);
                    mask|=residual[j];
                }
                // A wider valid bit width removes the generic bit-packing loop on short IDs.
                unsigned width=(std::bit_width(mask)+7)/8;
                out[head+i/32]=char(width*8);
                if (!width) continue;
                const size_t off=out.size();
                out.resize(off+32*width);
                char* p=out.data()+off;
                if(width==1) {
                    for(size_t j=0;j<32;++j) p[j]=char(residual[j]);
                } else if(width==2) {
                    for(size_t j=0;j<32;++j) {
                        uint16_t x=residual[j]; std::memcpy(p+2*j,&x,2);
                    }
                } else if(width==4) {
                    std::memcpy(p,residual.data(),128);
                } else {
                    for(size_t j=0;j<32;++j) std::memcpy(p+3*j,&residual[j],3);
                }
            }
        }
    }
    static void narrowDeltas(std::string& out,std::span<const uint8_t> src) {
        varint(out,128);varint(out,4);varint(out,src.size());varint(out,zigzag(src.front()));
        for(size_t start=1;start<src.size();start+=128){
            const size_t n=std::min<size_t>(128,src.size()-start);
            if(n==128){
                __m256i d[4];
                for(unsigned m=0;m<4;++m){
                    auto* p=src.data()+start+32*m;
                    d[m]=_mm256_sub_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)),_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p-1)));
                }
                auto m=_mm256_min_epi8(_mm256_min_epi8(d[0],d[1]),_mm256_min_epi8(d[2],d[3]));
                auto v=_mm_min_epi8(_mm256_castsi256_si128(m),_mm256_extracti128_si256(m,1));
                v=_mm_min_epi8(v,_mm_srli_si128(v,8));v=_mm_min_epi8(v,_mm_srli_si128(v,4));
                v=_mm_min_epi8(v,_mm_srli_si128(v,2));v=_mm_min_epi8(v,_mm_srli_si128(v,1));
                const int8_t minimum=int8_t(_mm_cvtsi128_si32(v));
                varint(out,zigzag(minimum));
                const size_t head=out.size();out.resize(head+4);
                auto base=_mm256_set1_epi8(minimum);
                for(unsigned k=0;k<4;++k){
                    auto residual=_mm256_sub_epi8(d[k],base);
                    bool any=!_mm256_testz_si256(residual,residual);
                    out[head+k]=any?8:0;
                    if(any){size_t at=out.size();out.resize(at+32);_mm256_storeu_si256(reinterpret_cast<__m256i*>(out.data()+at),residual);}
                }
            }else{
                std::array<int8_t,128> d{};
                int minimum=INT_MAX;
                for(size_t k=0;k<n;++k){int value=int(src[start+k])-int(src[start+k-1]);d[k]=int8_t(value);minimum=std::min(minimum,value);}
                varint(out,zigzag(minimum));
                const size_t head=out.size();out.resize(head+4,0);
                for(size_t k=0;k<n;k+=32){
                    std::array<uint8_t,32> residual{};unsigned mask=0;
                    for(size_t j=0;j<std::min<size_t>(32,n-k);++j){residual[j]=uint8_t(int(d[k+j])-minimum);mask|=residual[j];}
                    if(mask){out[head+k/32]=8;out.append(reinterpret_cast<const char*>(residual.data()),32);}
                }
            }
        }
    }
    std::vector<uint8_t> smallPrefix,smallSuffix;
    bool wideLengths=false;
    size_t pageRows() const {return wideLengths?prefix.size():smallPrefix.size();}
    void promoteLengths() {
        prefix.assign(smallPrefix.begin(),smallPrefix.end());
        suffix.assign(smallSuffix.begin(),smallSuffix.end());
        smallPrefix.clear();smallSuffix.clear();wideLengths=true;
    }
    void finishPage() {
        size_t n=pageRows();if(!n)return;
        encoded.clear();
        if(wideLengths){deltas(encoded,prefix);deltas(encoded,suffix);prefix.clear();suffix.clear();}
        else {narrowDeltas(encoded,smallPrefix);narrowDeltas(encoded,smallSuffix);smallPrefix.clear();smallSuffix.clear();}
        encoded.append(bytes);pages.push_back({std::move(encoded),uint32_t(n)});
        bytes.clear();wideLengths=false;
    }
    void appendToGroup(std::string_view name,uint32_t count) {
        if(name.size()>INT32_MAX)throw std::length_error("Parquet string too long");
        while(count){
            size_t pos=pageRows();
            if(pos && (pos>=16384 || bytes.size()+name.size()>1048576)){finishPage();pos=0;}
            if(!wideLengths && name.size()>127)promoteLengths();
            const uint32_t take=std::min<uint32_t>(count,16384-pos);
            const uint32_t length=name.size();bytes.append(name);
            if(wideLengths){
                prefix.insert(prefix.end(),take,length);suffix.insert(suffix.end(),take,0);
                prefix[pos]=0;suffix[pos]=length;
            }else{
                smallPrefix.insert(smallPrefix.end(),take,uint8_t(length));smallSuffix.insert(smallSuffix.end(),take,0);
                smallPrefix[pos]=0;smallSuffix[pos]=uint8_t(length);
            }
            count-=take;
        }
    }
    void sealGroup() {
        if (!rowsInGroup) return;
        finishPage();
        groups.emplace_back(std::move(pages));
        rowsInGroup = 0;
    }
    void append(std::string_view name, uint32_t count) {
        while (count) {
            const auto take = std::min<size_t>(count, rowGroup - rowsInGroup);
            appendToGroup(name, take);
            count -= take;
            rowsInGroup += take;
            if (rowsInGroup == rowGroup) sealGroup();
        }
    }
};

// A shopper contributes one consecutive run of output days. Encode that run
// once at its source. A dictionary is local to one Parquet column chunk.
struct RunNames {
    struct Group {
        std::string dictionary;
        std::string codes;
        uint32_t entries = 0, rows = 0;
        std::vector<DeltaNames::Page> fallback;
    };
    static constexpr size_t dictionaryLimit = 1 << 20;
    Group current;
    DeltaNames wideNames;
    bool wide = false;
    std::vector<Group> groups;

    void promote() {
        size_t textAt = 0, codeAt = 1;
        for (uint32_t entry = 0; entry < current.entries; ++entry) {
            uint32_t size;
            std::memcpy(&size, current.dictionary.data() + textAt, sizeof(size));
            textAt += sizeof(size);
            uint32_t run = 0;
            for (unsigned shift = 0;; shift += 7) {
                const auto byte = static_cast<uint8_t>(current.codes[codeAt++]);
                run |= uint32_t(byte & 127) << shift;
                if (!(byte & 128)) break;
            }
            codeAt += sizeof(uint16_t);
            wideNames.append(std::string_view(current.dictionary).substr(textAt, size), run >> 1);
            textAt += size;
        }
        current.dictionary.clear();
        current.codes.clear();
        wide = true;
    }

    void sealGroup() {
        if (!current.rows) return;
        if (wide) {
            wideNames.sealGroup();
            current.fallback = std::move(wideNames.groups.front());
            wideNames.groups.clear();
        }
        groups.emplace_back(std::move(current));
        current = {};
        wide = false;
    }

    void append(std::string_view name, uint32_t count) {
        while (count) {
            const auto take = std::min<uint32_t>(count, DeltaNames::rowGroup - current.rows);
            if (!wide && (name.size() > dictionaryLimit - 4 ||
                current.dictionary.size() > dictionaryLimit - 4 - name.size()))
                promote();
            if (wide) {
                wideNames.append(name, take);
            } else {
                const auto size = static_cast<uint32_t>(name.size());
                current.dictionary.append(reinterpret_cast<const char*>(&size), sizeof(size));
                current.dictionary.append(name);
                if (!current.entries) current.codes.push_back(16);
                DeltaNames::varint(current.codes, uint64_t(take) << 1);
                const auto id = static_cast<uint16_t>(current.entries++);
                current.codes.append(reinterpret_cast<const char*>(&id), sizeof(id));
            }
            current.rows += take;
            count -= take;
            if (current.rows == DeltaNames::rowGroup) sealGroup();
        }
    }
};
