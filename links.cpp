#include "shared.hpp"
#include <numeric>

namespace {
using SV = std::string_view;
constexpr size_t shards = Identities::shards;
struct Batch {
    SV rows;
    std::array<std::vector<Key>, shards> parts;
    std::vector<uint8_t> order;
    std::vector<uint32_t> ids;
};

// Balance work even if one CSV holds a sizable fraction of the entire input.
std::vector<Batch> batches(const std::vector<SV>& files) {
    std::vector<Batch> out;
    for (SV file : files) {
        size_t header = file.find('\n');
        if (header == SV::npos) continue;
        size_t begin = header + 1;
        while (begin < file.size()) {
            size_t end = file.find('\n', begin + (1u << 20));
            end = end == SV::npos ? file.size() : end + 1;
            out.emplace_back().rows = file.substr(begin, end - begin);
            begin = end;
        }
    }
    return out;
}

void endpoints(Batch& batch) {
    SV file = batch.rows;
    batch.order.reserve(file.size() / 10);

    while (!file.empty()) {
        std::array<SV, 2> fields;
        size_t end;
        bool valid;
        const char* p = file.data();
        uint32_t newlines = 0;
        __m256i block{};
        if (file.size() >= 32) {
            block = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
            newlines = _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, _mm256_set1_epi8('\n')));
        }
        if (newlines) {
            end = std::countr_zero(newlines);
            // A short edge is typically entirely in this load. Mask the
            // following row out before counting commas or validating quotes.
            const uint32_t keep = (newlines & -newlines) - 1;
            const uint32_t commas = keep & _mm256_movemask_epi8(
                _mm256_cmpeq_epi8(block, _mm256_set1_epi8(',')));
            valid = std::has_single_bit(commas);
            if (valid) {
                const size_t split = std::countr_zero(commas);
                const bool left = split >= 2 && p[0]=='"' && p[split-1]=='"';
                const bool right = end-split >= 3 && p[split+1]=='"' && p[end-1]=='"';
                const uint32_t quotes = keep & _mm256_movemask_epi8(
                    _mm256_cmpeq_epi8(block, _mm256_set1_epi8('"')));
                valid = std::popcount(quotes) == 2*(left+right);
                fields = {SV(p+left,split-2*left), SV(p+split+1+right,end-split-1-2*right)};
            }
        } else {
            end = file.find('\n');
            if (end == SV::npos) end = file.size();
            valid = csv(file.substr(0,end),fields);
        }
        if (valid && !fields[0].empty() && !fields[1].empty()) {
            for (SV f : fields) {
                Key k = Key::from(f);
                const auto shard = static_cast<uint8_t>(Identities::shard(k.hash()));
                batch.parts[shard].push_back(k);
                batch.order.push_back(shard);
            }
        }
        file.remove_prefix(std::min(end + 1, file.size()));
    }
}

struct Shard {
    std::vector<Key> names;
    std::vector<Identities::Alias> aliases;
};

void numberEndpoints(size_t s, std::vector<Batch>& input, Shard& shard) {
    size_t count = 0;
    for (auto& b : input) count += b.parts[s].size();
    // The number of distinct IDs is bounded by the number of endpoints.
    size_t capacity = std::bit_ceil(std::max(size_t{2}, count + count / 2 + 1));
    std::vector<uint32_t> table(capacity, UINT32_MAX);
    auto& names = shard.names;
    names.reserve(count);
    for (auto& b : input) {
        for (Key& entry : b.parts[s]) {
            const Key key = entry;
            size_t pos = key.hash() & (capacity - 1);
            while (table[pos] != UINT32_MAX && !(names[table[pos]] == key))
                pos = (pos + 1) & (capacity - 1);
            if (table[pos] == UINT32_MAX) {
                table[pos] = static_cast<uint32_t>(names.size());
                names.push_back(key);
            }
            // Every shard is processed by one worker. Reuse the raw-key
            // storage after interning instead of scattering writes into ids.
            entry.bits = table[pos];
        }
    }
}
} // namespace

void linkPeople(const std::vector<std::string_view>& files, Context& ctx, unsigned workers) {
    auto input = batches(files);
    parallel(input.size(), workers, [&](size_t i) { endpoints(input[i]); });
    std::array<Shard, shards> groups;
    parallel(shards, workers, [&](size_t s) { numberEndpoints(s, input, groups[s]); });

    std::array<uint32_t,shards> base{};
    size_t people = 0;
    for (size_t s = 0; s < shards; ++s) {
        base[s] = static_cast<uint32_t>(people);
        people += groups[s].names.size();
        if (people > UINT32_MAX) throw std::runtime_error("Too many identity endpoints");
    }
    if (!people) return;
    parallel(input.size(),workers,[&](size_t i) {
        auto& b = input[i];
        std::array<size_t,shards> next{};
        b.ids.resize(b.order.size());
        for (size_t e = 0; e < b.order.size(); ++e) {
            const unsigned s = b.order[e];
            b.ids[e] = base[s] + static_cast<uint32_t>(b.parts[s][next[s]++].bits);
        }
        for (auto& part : b.parts) std::vector<Key>().swap(part);
        std::vector<uint8_t>().swap(b.order);
    });

    // Root and its component minimum share a cache line during unions.
    // Shard-major indices keep the post-union scan sequential.
    struct Node { Key minimum; uint32_t parent; };
    std::vector<Node> nodes(people);
    parallel(shards,workers,[&](size_t s) {
        const auto& names = groups[s].names;
        for (size_t i=0; i<names.size(); ++i)
            nodes[base[s]+i] = {names[i], static_cast<uint32_t>(base[s]+i)};
    });
    auto root = [&](uint32_t i) {
        while (nodes[i].parent != i) {
            nodes[i].parent = nodes[nodes[i].parent].parent;
            i = nodes[i].parent;
        }
        return i;
    };
    for (const auto& b : input) {
        for (size_t i = 0; i < b.ids.size(); i += 2) {
            uint32_t a = root(b.ids[i]), c = root(b.ids[i+1]);
            if (a == c) continue;
            if (a > c) std::swap(a,c);
            nodes[c].parent = a;
            if (nodes[c].minimum < nodes[a].minimum) nodes[a].minimum = nodes[c].minimum;
        }
    }
    // Each parent precedes its children, so one forward pass gives the
    // exact component root for all IDs independently of input edge order.
    for (auto& node : nodes) node.parent = nodes[node.parent].parent;
    parallel(shards,workers,[&](size_t s) {
        auto& g = groups[s];
        for (size_t i=0; i<g.names.size(); ++i) {
            Key key = g.names[i];
            Key name = nodes[nodes[base[s]+i].parent].minimum;
            if (!(key == name)) g.aliases.push_back({key,name});
        }
    });
    size_t mostAliases = 0;
    for (const auto& g : groups) mostAliases = std::max(mostAliases, g.aliases.size());
    if (!mostAliases) return;
    auto& dict = ctx.identities;
    dict.shardSize = std::bit_ceil(mostAliases + mostAliases / 2 + 1);
    dict.slotShift = 64 - Identities::shardBits - std::countr_zero(dict.shardSize);
    dict.slots.resize(dict.shardSize * shards);
    dict.fingerprints.resize(dict.slots.size());
    parallel(shards, workers, [&](size_t s) {
        auto* table = dict.slots.data() + s * dict.shardSize;
        size_t mask = dict.shardSize - 1;
        for (const auto& [k,m] : groups[s].aliases) {
            const uint64_t hash = k.hash();
            size_t pos = dict.home(hash);
            dict.fingerprints[s * dict.shardSize + pos] |= Identities::fingerprint(hash);
            while (!table[pos].raw.empty()) pos = (pos + 1) & mask;
            table[pos] = {k, m};
        }
    });
}
