#pragma once
#include <array>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <vector>
#include <string_view>

// The modal lead and up to sixteen observed second bytes identify a small family.
// Seven validated ASCII digits or lowercase letters each use a five-bit lane.
// One position may carry a sixth bit. Other spellings remain full original Keys.
struct ShopperCodec {
    static constexpr uint64_t valueMask=(UINT64_C(1)<<42)-1;
    static constexpr uint64_t lowerDigits=UINT64_C(0x001f1f1f1f1f1f1f);
    std::array<uint8_t,128> rank{};
    std::array<uint8_t,16> original{};
    uint64_t fixed{};
    uint32_t buckets{};

    void learn(const std::vector<std::string_view>& names) {
        *this=ShopperCodec{};
        std::array<uint32_t,128> leads{};
        for(auto text:names)if(text.size()==9 && uint8_t(text[0])<128)++leads[uint8_t(text[0])];
        auto top=std::max_element(leads.begin(),leads.end());
        if(names.size()<96 || *top<=names.size()/2)return;
        uint8_t lead=uint8_t(top-leads.begin());
        std::array<uint32_t,128> counts{};
        for(auto text:names)if(text.size()==9 && uint8_t(text[0])==lead && uint8_t(text[1])<128)++counts[uint8_t(text[1])];
        std::array<uint8_t,128> sorted{};
        for(unsigned i=0;i<128;++i)sorted[i]=uint8_t(i);
        std::partial_sort(sorted.begin(),sorted.begin()+16,sorted.end(),[&](uint8_t a,uint8_t b){
            return counts[a]==counts[b]?a<b:counts[a]>counts[b];
        });
        rank.fill(255);
        for(unsigned n=0;n<16 && counts[sorted[n]];++n){rank[sorted[n]]=n;original[n]=sorted[n];}
        fixed=Key::scatterPrefix(lead);
        buckets=1;
    }
    void place(size_t count){if(buckets)buckets=count;}
    bool encode(uint64_t key,uint64_t& code)const {
        if(!buckets || (key&~Key::asciiMask)!=fixed)return false;
        const auto first=rank[(key>>56)&127];
        if(first==255)return false;
        auto bytes=_mm_cvtsi64_si128(key&Key::asciiMask);
        auto lo=_mm_and_si128(bytes,_mm_set1_epi8(15));
        auto hi=_mm_and_si128(_mm_srli_epi16(bytes,4),_mm_set1_epi8(15));
        const auto checks=_mm_setr_epi8(-128,-128,-128,118,-128,-128,-1,117,-128,-128,-128,-128,-128,-128,-128,-128);
        if(_mm_movemask_epi8(_mm_add_epi8(lo,_mm_shuffle_epi8(checks,hi)))&127)return false;
        const auto biases=_mm_setr_epi8(0,0,0,0,0,0,9,25,0,0,0,0,0,0,0,0);
        auto digits=_mm_add_epi8(lo,_mm_shuffle_epi8(biases,hi));
        uint32_t high=_mm_movemask_epi8(_mm_cmpgt_epi8(digits,_mm_set1_epi8(31)))&127;
        if(high&(high-1))return false;
        uint32_t rare=std::bit_width(high);
        code=(uint64_t(first)<<38)|(uint64_t(rare)<<35)|_pext_u64(_mm_cvtsi128_si64(digits),lowerDigits);
        return true;
    }
    uint64_t decode(uint64_t receipt)const {
        const auto code=receipt&valueMask;
        uint64_t digitWord=_pdep_u64(code,lowerDigits);
        unsigned rare=unsigned(code>>35)&7;
        if(rare)digitWord|=UINT64_C(32)<<(8*(rare-1));
        auto digits=_mm_cvtsi64_si128(digitWord);
        auto letter=_mm_cmpgt_epi8(digits,_mm_set1_epi8(9));
        auto bytes=_mm_add_epi8(_mm_add_epi8(digits,_mm_set1_epi8('0')),_mm_and_si128(letter,_mm_set1_epi8(39)));
        return fixed|(uint64_t(original[code>>38])<<56)|(uint64_t(_mm_cvtsi128_si64(bytes))&UINT64_C(0x00ffffffffffffff));
    }
    bool pack(uint64_t key,uint64_t ms,uint64_t& receipt)const {
        if(ms>=(UINT64_C(1)<<31))return false;
        auto seconds=ms/1000;
        if(seconds*1000!=ms)return false;
        uint64_t code;
        if(!encode(key,code))return false;
        receipt=(seconds<<42)|code;return true;
    }
    uint32_t milliseconds(uint64_t receipt)const {return uint32_t((receipt>>42)*1000);}
};