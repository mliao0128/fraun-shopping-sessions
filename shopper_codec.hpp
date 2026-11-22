#pragma once
#include <bitset>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>
#include <algorithm>
struct ShopperCodec {
    static constexpr uint64_t domain=UINT64_C(36)*36*36*36*36*36;
    static constexpr uint64_t seven=UINT64_C(0x7f7f7f7f7f7f7f7f);
    struct Sample {uint32_t n{};std::bitset<128> a,b;};
    std::array<uint32_t,65536> base;
    std::array<uint16_t,1296> inverse{};
    static constexpr auto pairs=[] {
        std::array<uint16_t,65536> t{};
        t.fill(0x8000);
        constexpr char alpha[]="0123456789abcdefghijklmnopqrstuvwxyz";
        for(unsigned a=0;a<36;++a)for(unsigned b=0;b<36;++b)
            t[uint8_t(alpha[a]) | (uint16_t(uint8_t(alpha[b]))<<8)]=a*36+b;
        return t;
    }();
    static constexpr auto spell=[] {
        std::array<uint16_t,1296> t{};
        constexpr char alpha[]="0123456789abcdefghijklmnopqrstuvwxyz";
        for(unsigned a=0;a<36;++a)for(unsigned b=0;b<36;++b)
            t[a*36+b]=(uint16_t(uint8_t(alpha[a]))<<8)|uint8_t(alpha[b]);
        return t;
    }();
    uint64_t marks=UINT64_MAX;
    uint32_t buckets=0;
    void learn(const std::vector<std::string_view>& names) {
        std::array<Sample,128> s{};
        base.fill(UINT32_MAX);buckets=0;marks=UINT64_MAX;
        for(auto x:names){
            if(x.size()!=9 || uint8_t(x[0])>=128 || uint8_t(x[1])>=128 || uint8_t(x[2])>=128)continue;
            uint64_t raw;std::memcpy(&raw,x.data()+1,8);
            if((pairs[uint16_t(raw>>16)]|pairs[uint16_t(raw>>32)]|pairs[uint16_t(raw>>48)])&0x8000)continue;
            auto& v=s[uint8_t(x[0])];++v.n;v.a.set(uint8_t(raw));v.b.set(uint8_t(raw>>8));
        }
        auto top=std::max_element(s.begin(),s.end(),[](const auto& a,const auto& b){return a.n<b.n;});
        if(names.size()<96 || top->n<=names.size()/2 || top->a.count()*top->b.count()>1296)return;
        unsigned count=0;
        for(unsigned a=0;a<128;++a)if(top->a[a])for(unsigned b=0;b<128;++b)if(top->b[b]){
            base[a|(b<<8)]=count*(domain/4096);
            inverse[count++]=(a<<8)|b;
        }
        buckets=(uint64_t(count)*domain+UINT32_MAX)>>32;
        char representative[9]={'0','0','0','0','0','0','0','0','0'};
        representative[0]=char(top-s.begin());marks=Key::nine(representative).bits&~seven;
    }
    bool encode(uint64_t key,uint64_t& code) const {
        if((key&~seven)!=marks)return false;
        uint64_t v=__builtin_bswap64(key&seven);
        auto start=base[uint16_t(v)];
        unsigned a=pairs[uint16_t(v>>16)],b=pairs[uint16_t(v>>32)],c=pairs[uint16_t(v>>48)];
        if(start==UINT32_MAX||((a|b|c)&0x8000))return false;
        code=(uint64_t(start)<<12)+(a*1296+b)*1296+c;
        return true;
    }
    uint64_t decode(uint64_t code) const {
        const unsigned first=code/domain;
        uint32_t trailing=code-first*domain;
        const uint32_t hi=trailing/1296;
        const uint32_t low=trailing%1296;
        return marks|(uint64_t(inverse[first])<<48)|(uint64_t(spell[hi/1296])<<32)|
               (uint64_t(spell[hi%1296])<<16)|spell[low];
    }
};
