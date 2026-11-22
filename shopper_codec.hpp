#pragma once
#include <array>
#include <bit>
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

// Twelve deterministic bytes retain the real graph and grouping hash as well as
// the exact original spelling and timestamp. One reserved bit selects an already
// published shared string slot instead of a reversible nine-byte ASCII spelling.
struct __attribute__((packed)) ShopperReceipt { uint64_t keyAndHash; uint32_t timeAndHash; };
static_assert(sizeof(ShopperReceipt)==12);
struct ShopperCodec {
    uint64_t invariant{},variable{};
    unsigned width{},partitionBits{},spill{};
    uint32_t spillMask{},lowHashMask{},buckets{};
    bool shortEnabled{};
    void learn(const std::vector<std::string_view>& names) {
        *this=ShopperCodec{};
        std::array<unsigned,128> leads{};
        for(auto v:names)if(v.size()==9 && uint8_t(v[0])<128)++leads[uint8_t(v[0])];
        const auto top=std::max_element(leads.begin(),leads.end());
        if(names.size()<96||*top<=names.size()/2)return;
        const uint8_t lead=uint8_t(top-leads.begin());
        std::array<unsigned,63> ones{};unsigned count=0;
        for(auto v:names)if(v.size()==9 && uint8_t(v[0])==lead){
            auto k=Key::nine(v.data());if(!k.nineAscii())continue;++count;
            for(unsigned i=0;i<63;++i)ones[i]+=(k.bits>>i)&1;
        }
        if(count<96)return;
        unsigned trim=count/128;
        for(unsigned i=0;i<63;++i){
            if(ones[i]>=count-trim)invariant|=UINT64_C(1)<<i;
            else if(ones[i]>trim)variable|=UINT64_C(1)<<i;
        }
        width=std::popcount(variable);shortEnabled=width && width<=54;
    }
    void place(size_t parts) {
        buckets=(shortEnabled||Key::shopperSlots)?parts:0;
        partitionBits=std::countr_zero(parts);
        lowHashMask=UINT32_MAX>>partitionBits;
        const unsigned storedInKey=64-width;
        const unsigned rest=32-partitionBits;
        spill=storedInKey<rest?rest-storedInKey:0;
        spillMask=(uint32_t{1}<<spill)-1;
    }
    bool pack(Key k,uint64_t h,uint64_t ms,ShopperReceipt& r) const {
        if(!buckets || ms>=(UINT64_C(1)<<31))return false;
        uint32_t high=h>>32;
        if(k.interned()){
            const auto ordinal=size_t(k.shopperSlot()-Key::shopperSlots);
            const unsigned len=(k.bits>>48)&63;
            r={(uint64_t(high)<<32)|(uint64_t(len)<<21)|ordinal,0x80000000U|uint32_t(ms)};
            return true;
        }
        if(!shortEnabled || (k.bits&~variable)!=invariant)return false;
        uint64_t ticks=ms;
        if(spill){ticks=ms/1000;if(ticks*1000!=ms || ticks>(0x7fffffffU>>spill))return false;}
        uint32_t rem=high&lowHashMask;
        r.keyAndHash=_pext_u64(k.bits,variable)|(uint64_t(rem)<<width);
        r.timeAndHash=uint32_t(ticks)<<spill;
        if(spill)r.timeAndHash|=rem>>(64-width);
        return true;
    }
    Key decode(ShopperReceipt r) const {
        if(r.timeAndHash>>31){
            auto* slot=Key::shopperSlots+(r.keyAndHash&0x1fffff);
            auto len=(r.keyAndHash>>21)&63;
            return Key{Key::shopperTag|(len<<48)|reinterpret_cast<uintptr_t>(slot)};
        }
        return Key{invariant|_pdep_u64(r.keyAndHash,variable)};
    }
    uint64_t hash(uint32_t partition,ShopperReceipt r) const {
        if(r.timeAndHash>>31)return r.keyAndHash&UINT64_C(0xffffffff00000000);
        uint64_t rem=(r.keyAndHash>>width);
        if(spill)rem|=uint64_t(r.timeAndHash&spillMask)<<(64-width);
        return (((uint64_t(partition)<<(32-partitionBits))|rem)<<32);
    }
    uint32_t milliseconds(ShopperReceipt r)const {
        if(r.timeAndHash>>31)return r.timeAndHash&0x7fffffff;
        const uint32_t ticks=r.timeAndHash>>spill;
        return spill?ticks*1000:ticks;
    }
};