#pragma once
#include <array>
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
    void finishPage() {
        if(prefix.empty()) return;
        encoded.clear();
        deltas(encoded,prefix); deltas(encoded,suffix); encoded.append(bytes);
        pages.push_back({std::move(encoded),uint32_t(prefix.size())});
        prefix.clear(); suffix.clear(); bytes.clear();
    }
    void appendToGroup(std::string_view name,uint32_t count) {
        if(name.size()>INT32_MAX) throw std::length_error("Parquet string too long");
        while(count) {
            if(!prefix.empty() && (prefix.size()>=16384 || bytes.size()+name.size()>1048576)) finishPage();
            const uint32_t take=std::min<uint32_t>(count,16384-prefix.size());
            const uint32_t length=name.size();
            bytes.append(name);
            const size_t pos=prefix.size();
            prefix.insert(prefix.end(),take,length);
            suffix.insert(suffix.end(),take,0);
            prefix[pos]=0; suffix[pos]=length;
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
