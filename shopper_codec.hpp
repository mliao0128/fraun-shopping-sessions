#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <string_view>
#include <vector>

struct ShopperCodec {
    uint64_t invariant{},variable{},lowMask{};
    unsigned width{},lowBits{},partBits{};
    uint64_t lowRawMask{},highRawMask{};
    uint32_t buckets{};
    void learn(const std::vector<std::string_view>& sample) {
        *this=ShopperCodec{};
        std::array<uint32_t,128> leads{};
        for(auto bytes:sample)if(bytes.size()==9 && static_cast<unsigned char>(bytes[0])<128)
            ++leads[static_cast<unsigned char>(bytes[0])];
        auto lead=std::max_element(leads.begin(),leads.end())-leads.begin();
        if(sample.size()<96 || leads[lead]<=sample.size()/2)return;
        std::array<uint32_t,64> ones{};unsigned n=0;
        for(auto bytes:sample)if(bytes.size()==9 && static_cast<unsigned char>(bytes[0])==lead){
            auto key=Key::nine(bytes.data());if(!key.nineAscii())continue;++n;
            for(unsigned bit=0;bit<63;++bit)ones[bit]+=(key.bits>>bit)&1;
        }
        if(n<96)return;
        const unsigned tolerated=n/128;
        invariant=0;variable=0;
        for(unsigned bit=0;bit<63;++bit){
            if(ones[bit]>=n-tolerated)invariant |= uint64_t{1}<<bit;
            else if(ones[bit]>tolerated)variable |= uint64_t{1}<<bit;
        }
        width=std::popcount(variable);
        if(width>54 || !width)return;
        buckets=1;
    }
    void place(size_t maximum) {
        if(!buckets)return;
        partBits=std::min<unsigned>(width,std::countr_zero(maximum));
        buckets=uint32_t{1}<<partBits;
        lowBits=width-partBits;
        lowMask=(uint64_t{1}<<lowBits)-1;
        lowRawMask=_pdep_u64(lowMask,variable);
        highRawMask=variable^lowRawMask;
    }
    bool encode(uint64_t raw,uint64_t& out) const {
        if((raw&~variable)!=invariant)return false;
        out=_pext_u64(raw,variable);return true;
    }
    uint64_t decode(uint64_t code) const {return invariant|_pdep_u64(code,variable);}
    uint32_t partition(uint64_t code,uint32_t noise)const {
        const uint32_t upper=uint32_t(code>>lowBits);
        return partBits?((upper^(noise>>(32-partBits)))&(buckets-1)):0;
    }
    uint64_t complete(size_t part,uint64_t low)const {
        low&=lowMask;
        uint32_t noise=_mm_crc32_u64(0,low);
        const auto upper=uint64_t(part)^(partBits?noise>>(32-partBits):0);
        return (upper<<lowBits)|low;
    }
    bool pack(uint64_t code,uint64_t ms,uint64_t& receipt) const {
        if(ms>=(UINT64_C(1)<<31))return false;
        uint64_t tick=ms;
        if(lowBits>32){
            tick=ms/1000;
            if(tick*1000!=ms)return false;
        }
        if(tick>(UINT64_MAX>>lowBits))return false;
        receipt=(tick<<lowBits)|(code&lowMask);return true;
    }
    uint32_t milliseconds(uint64_t receipt) const {
        uint64_t tick=receipt>>lowBits;
        return uint32_t(lowBits>32?tick*1000:tick);
    }
};