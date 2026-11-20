#include "shared.hpp"
#include <numeric>

namespace {
using SV = std::string_view;
constexpr size_t shards = Identities::shards;
struct Endpoint { Key key; uint32_t offset; };
struct Batch {
    SV rows;
    std::array<std::vector<Endpoint>, shards> parts;
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
    uint32_t offset = 0;

    while (!file.empty()) {
        size_t end = file.find('\n');
        if (end == SV::npos) end = file.size();
        std::array<SV, 2> fields;
        if (csv(file.substr(0, end), fields) && !fields[0].empty() && !fields[1].empty()) {
            for (SV f : fields) {
                Key k = Key::from(f);
                batch.parts[Identities::shard(k.hash())].push_back({k, offset++});
            }
        }
        file.remove_prefix(std::min(end + 1, file.size()));
    }
    batch.ids.resize(offset);
}

struct Shard {
    std::vector<Key> names;
    size_t aliases = 0;
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
        for (const auto& e : b.parts[s]) {
            size_t pos = e.key.hash() & (capacity - 1);
            while (table[pos] != UINT32_MAX && !(names[table[pos]] == e.key))
                pos = (pos + 1) & (capacity - 1);
            if (table[pos] == UINT32_MAX) {
                table[pos] = static_cast<uint32_t>(names.size());
                names.push_back(e.key);
            }
            b.ids[e.offset] = (table[pos] << Identities::shardBits) | static_cast<uint32_t>(s);
        }
        std::vector<Endpoint>().swap(b.parts[s]);
    }
}
} // namespace

void linkPeople(const std::vector<std::string_view>& files, Context& ctx, unsigned workers) {
    auto input = batches(files);
    parallel(input.size(), workers, [&](size_t i) { endpoints(input[i]); });
    std::array<Shard, shards> groups;
    parallel(shards, workers, [&](size_t s) { numberEndpoints(s, input, groups[s]); });

    size_t longest = 0;
    for (const auto& g : groups) longest = std::max(longest, g.names.size());
    if (!longest) return;
    // Interleaving shard and local ordinal gives both endpoints an ID on their
    // first pass. Only the integer union-find touches the shared graph.
    std::vector<uint32_t> parent(longest * shards);
    std::iota(parent.begin(), parent.end(), uint32_t{0});
    auto root = [&](uint32_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    for (auto& b : input) {
        for (size_t i = 0; i < b.ids.size(); i += 2) {
            uint32_t a = root(b.ids[i]), c = root(b.ids[i + 1]);
            if (a > c) std::swap(a, c);
            parent[c] = a;
        }
    }
    // Every parent is smaller than its child, so this pass flattens every tree.
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = parent[parent[i]];
    std::vector<Key> minimum(parent.size());
    for (size_t s = 0; s < shards; ++s) {
        const auto& names = groups[s].names;
        for (size_t i = 0; i < names.size(); ++i) {
            auto& m = minimum[parent[(i << Identities::shardBits) | s]];
            if (m.empty() || names[i] < m) m = names[i];
        }
    }
    parallel(shards, workers, [&](size_t s) {
        auto& g = groups[s];
        for (size_t i = 0; i < g.names.size(); ++i)
            g.aliases += !(g.names[i] == minimum[parent[(i << Identities::shardBits) | s]]);
    });
    size_t mostAliases = 0;
    for (const auto& g : groups) mostAliases = std::max(mostAliases, g.aliases);
    if (!mostAliases) return;
    auto& dict = ctx.identities;
    dict.shardSize = std::bit_ceil(mostAliases + mostAliases / 2 + 1);
    dict.slots.resize(dict.shardSize * shards);
    parallel(shards, workers, [&](size_t s) {
        auto* table = dict.slots.data() + s * dict.shardSize;
        const auto& names = groups[s].names;
        size_t mask = dict.shardSize - 1;
        for (size_t i = 0; i < names.size(); ++i) {
            const Key& k = names[i];
            const Key& m = minimum[parent[(i << Identities::shardBits) | s]];
            if (k == m) continue;
            size_t pos = k.hash() & mask;
            while (!table[pos].raw.empty()) pos = (pos + 1) & mask;
            table[pos] = {k, m};
        }
    });
}
