#pragma once
#include "shared.hpp"
// The complete quote count certifies that no quote was left in an interior
// position. Every counted pair belongs to a distinct, nonoverlapping field.
[[gnu::target("avx512f,avx512bw"),gnu::noinline]]
inline bool wideEventFields(std::string_view row, std::array<std::string_view,7>& fields) {
    const char *p = row.data();
    const unsigned n = static_cast<unsigned>(row.size());
    unsigned stops[7], count = 0, quotes = 0;
    for (unsigned at = 0; at < n; at += 64) {
        const unsigned span = std::min(64u, n - at);
        const uint64_t live = _bzhi_u64(~uint64_t{0}, span);
        const __m512i bytes = _mm512_maskz_loadu_epi8(live, p + at);
        uint64_t commas = _mm512_cmpeq_epi8_mask(bytes, _mm512_set1_epi8(','));
        if (count + std::popcount(commas) > 6) return false;
        quotes += std::popcount(_mm512_cmpeq_epi8_mask(bytes, _mm512_set1_epi8('"')));
        while (commas) {
            stops[count++] = at + std::countr_zero(commas);
            commas &= commas - 1;
        }
    }
    if (count != 6) return false;
    stops[6] = n;
    unsigned begin = 0, wrapped = 0;
    for (unsigned i = 0; i < 7; ++i) {
        const unsigned end = stops[i];
        const unsigned width = end - begin;
        const unsigned trim = width >= 2 && p[begin] == '"' && p[end-1] == '"';
        fields[i] = {p + begin + trim, width - 2 * trim};
        wrapped += trim;
        begin = end + 1;
    }
    return quotes == 2 * wrapped;
}
// Most event rows place the first two delimiters in the first 32 bytes.
// Parse those and the remaining four independently. The split lies inside
// the third field, so it introduces no new quote boundary.
inline bool eventFields(std::string_view s, std::array<std::string_view,7>& f) {
    const size_t n = s.size();
    if (n > 96 && n <= 512 && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512f"))
        return wideEventFields(s, f);
    if (n > 96 && n <= 128) return csvPacked<7,true,true>(s,f);
    if (n <= 64 || n > 96) return csv(s,f);
    const char* p = s.data();
    auto head = classify32(p);
    auto middle = classify32(p + 32);
    auto tail = classify32(p + n - 32);
    const uint32_t low = head.comma;
    const uint64_t high = middle.comma | (uint64_t(tail.comma) << (n - 64));
    if (std::popcount(low) != 2 || (low & 0x80000001u) || (high & 1)) return csv(s,f);
    if (std::popcount(high) != 4) return false;
    const uint64_t highQuotes = middle.quote | (uint64_t(tail.quote) << (n - 64));
    const uint32_t lowStarts = (low << 1) | 1, lowEnds = low >> 1;
    const uint64_t highStarts = high << 1;
    const uint64_t highEnds = (high >> 1) | (uint64_t{1} << (n - 33));
    const unsigned opened = _pext_u32(head.quote,lowStarts) |
                            (_pext_u64(highQuotes,highStarts) << 3);
    const unsigned closed = _pext_u32(head.quote,lowEnds) |
                            (_pext_u64(highQuotes,highEnds) << 2);
    if (opened != closed || (head.quote & ~(lowStarts ^ lowEnds)) ||
        (highQuotes & ~(highStarts ^ highEnds))) return false;
    const uint64_t second = high & (high - 1);
    const uint64_t third = second & (second - 1);
    const unsigned ends[] = {
        unsigned(std::countr_zero(low)), 31u - std::countl_zero(low),
        32u + std::countr_zero(high), 32u + std::countr_zero(second),
        32u + std::countr_zero(third), 95u - std::countl_zero(high), unsigned(n)
    };
    unsigned begin = 0;
    for (unsigned i = 0; i < 7; ++i) {
        unsigned trim = (opened >> i) & 1;
        f[i] = {p + begin + trim, ends[i] - begin - 2*trim};
        begin = ends[i]+1;
    }
    return true;
}
