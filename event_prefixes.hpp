#pragma once

#include "event_fields.hpp"
#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>

// IDs are compared for equality only. Reserve a disjoint token range for
// a verified prefix and eight ASCII bytes. An unrepresented ID keeps the
// byte-exact external path. The same immutable dictionary serves all readers.
class EventPrefixes {
    static constexpr size_t maxPrefix = 64;
    static constexpr size_t capacity = 16;
    struct Span { uint8_t begin{}, end{}; };
    std::array<Span, maxPrefix + 1> byLength{};
    std::array<std::string_view, capacity> entries{};

public:
    explicit EventPrefixes(const std::vector<std::string_view>& files) {
        std::unordered_map<std::string_view, unsigned> counts;
        // Sampling only chooses which prefixes to compress. A hit is checked
        // against every byte of the actual ID before a compact token is used.
        const size_t fileCount = std::min<size_t>(files.size(), 16);
        for (size_t i = 0; i < fileCount; ++i) {
            auto file = files[i * files.size() / fileCount];
            for (size_t w = 0; w < 3; ++w) {
                size_t at = file.find('\n', w * (file.size() / 3));
                if (at == file.npos) continue;
                ++at;
                for (unsigned j = 0; j < 24 && at < file.size(); ++j) {
                    auto window = file.substr(at, 4096);
                    size_t end = window.find('\n');
                    if (end == window.npos) break;
                    auto line = window.substr(0, end);
                    at += end + 1;
                    size_t comma = line.find(',');
                    if (comma == line.npos) continue;
                    auto id = line.substr(0, comma);
                    if (id.size() >= 2 && id.front() == '"' && id.back() == '"')
                        id = id.substr(1, id.size() - 2);
                    if (id.size() <= 9 || id.size() > maxPrefix + 8) continue;
                    uint64_t suffix;
                    std::memcpy(&suffix, id.data() + id.size() - 8, 8);
                    if (!(suffix & ~Key::asciiMask))
                        ++counts[id.substr(0, id.size() - 8)];
                }
            }
        }
        std::vector<std::pair<std::string_view, unsigned>> ranked;
        for (auto [prefix, count] : counts)
            if (count >= 3) ranked.emplace_back(prefix, count);
        auto popular = [](const auto& a, const auto& b) {
            return a.second == b.second ? a.first < b.first : a.second > b.second;
        };
        std::sort(ranked.begin(), ranked.end(), popular);
        if (ranked.size() > capacity) ranked.resize(capacity);
        // Keep most frequent first within each length.
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            return a.first.size() < b.first.size();
        });
        for (size_t i = 0; i < ranked.size(); ++i) {
            entries[i] = ranked[i].first;
            Span& span = byLength[entries[i].size()];
            if (!span.end) span.begin = static_cast<uint8_t>(i);
            span.end = static_cast<uint8_t>(i + 1);
        }
    }

    Key encode(std::string_view id) const {
        if (__builtin_expect(id.size() <= 9, true)) return eventID(id);
        size_t n = id.size() - 8;
        if (n <= maxPrefix) {
            Span span = byLength[n];
            for (unsigned i = span.begin; i < span.end; ++i) {
                if (std::memcmp(entries[i].data(), id.data(), n)) continue;
                uint64_t suffix;
                std::memcpy(&suffix, id.data() + n, 8);
                if (suffix & ~Key::asciiMask) break;
                return {UINT64_C(0x9000000000000000) |
                        (uint64_t(i) << 56) | _pext_u64(suffix, Key::asciiMask)};
            }
        }
        return Key::fromReference(id);
    }
};
