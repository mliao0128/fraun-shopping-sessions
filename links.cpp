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

template <class F> void linkRows(SV file, F action) {

    struct Pending {
        std::array<SV,2> fields;
        std::array<uint64_t,2> hashes;
    };
    std::array<Pending,16> queue;
    unsigned used=0;
    const auto eligible=[](SV s) {return Key::shopperSlots && s.size()>9 && s.size()<=48;};
    const auto consume=[&](const Pending& item) {
        const auto resolve=[&](unsigned i) {
            const auto text=item.fields[i];
            return eligible(text) ? Key::internHashedShopper(text,item.hashes[i]) : Key::from(text);
        };
        const auto left=resolve(0);
        action(left,resolve(1));
    };
    while (!file.empty()) {
        std::array<SV, 2> fields;
        size_t end;
        bool valid;
        const char *p = file.data();
        uint32_t newlines = 0;
        __m256i block{};
        if (file.size() >= 32) {
            block = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p));
            newlines = _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, _mm256_set1_epi8('\n')));
        }
        if (newlines) {
            end = std::countr_zero(newlines);
            // A short edge is typically entirely in this load. Mask the
            // following row out before counting commas or validating quotes.
            const uint32_t keep = (newlines & -newlines) - 1;
            const uint32_t commas =
                keep & _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, _mm256_set1_epi8(',')));
            valid = std::has_single_bit(commas);
            if (valid) {
                const size_t split = std::countr_zero(commas);
                const bool left = split >= 2 && p[0] == '"' && p[split - 1] == '"';
                const bool right = end - split >= 3 && p[split + 1] == '"' && p[end - 1] == '"';
                const uint32_t quotes =
                    keep & _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, _mm256_set1_epi8('"')));
                valid = std::popcount(quotes) == 2 * (left + right);
                fields = {SV(p + left, split - 2 * left),
                          SV(p + split + 1 + right, end - split - 1 - 2 * right)};
            }
        } else {
            end = file.find('\n');
            if (end == SV::npos)
                end = file.size();
            valid = csv(file.substr(0, end), fields);
        }
        if (valid && !fields[0].empty() && !fields[1].empty()) {
            {
                Pending& item=queue[used%queue.size()];
                if(used>=queue.size()) consume(item);
                item.fields=fields;
                for(unsigned i=0;i<2;++i) {
                    if(!eligible(fields[i])) continue;
                    const auto hash=Key::spellingHash(fields[i]);
                    item.hashes[i]=hash;
                    __builtin_prefetch(&Key::shopperSlots[hash&Key::shopperMask],0,3);
                }
                ++used;
            }
        }
        file.remove_prefix(std::min(end + 1, file.size()));
    }
    for(unsigned i=used-std::min<unsigned>(used,queue.size());i<used;++i)
        consume(queue[i%queue.size()]);
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
                Key k = Key::shopper(f);
                const auto shard = static_cast<uint8_t>(Identities::shard(shopperRoutingHash(k)));
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
            size_t pos = shopperRoutingHash(key) & (capacity - 1);
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

Key Identities::fallbackPerson(const Key& raw, uint64_t& hash) const {
    const auto& group = (*fallback)[shard(hash)];
    if (group.slots.empty()) return raw;
    const size_t mask = group.slots.size() - 1;
    const size_t pos = (hash >> group.shift) & mask;
    if (!(group.fingerprints[pos] & fingerprint(hash))) return raw;
    return probe(group.slots.data(), mask, pos, raw, hash);
}

void fallbackLinks(const std::vector<std::string_view> &files, Context &ctx, unsigned workers) {
    // All producers of the direct table have joined. Release its allocation
    // before constructing the exact graph and its replacement directory.
    std::vector<Identities::Alias>().swap(ctx.identities.slots);
    std::vector<uint16_t>().swap(ctx.identities.fingerprints);
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
    if (std::all_of(groups.begin(), groups.end(), [](const Shard& group) {
        return group.aliases.empty();
    })) return;

    auto directory = std::make_unique<std::array<Identities::FallbackShard, shards>>();
    parallel(shards, workers, [&](size_t s) {
        const size_t capacity = Identities::FallbackShard::capacity(groups[s].aliases.size());
        if (!capacity) return;
        auto& group = (*directory)[s];
        group.slots.resize(capacity);
        group.fingerprints.resize(capacity);
        group.shift = 64 - Identities::shardBits - std::countr_zero(capacity);
        const size_t mask = capacity - 1;
        for (const auto& [k,m] : groups[s].aliases) {
            const uint64_t hash = shopperRoutingHash(k);
            size_t pos = (hash >> group.shift) & mask;
            group.fingerprints[pos] |= Identities::fingerprint(hash);
            while (!group.slots[pos].raw.empty()) pos = (pos + 1) & mask;
            group.slots[pos] = {k, m};
        }
    });
    ctx.identities.fallback = std::move(directory);
}

void linkPeople(const std::vector<std::string_view> &files, Context &ctx, unsigned workers) {
    auto chunks = batches(files);
    size_t rows = 0;
    for (const auto &b : chunks)
        rows += std::count(b.rows.begin(), b.rows.end(), '\n') + 1;
    if (!rows)
        return;
    const size_t cap = std::bit_ceil(std::max<size_t>(32, rows / shards + rows / shards / 2 + 64));
    if (cap > UINT32_MAX / shards) {
        fallbackLinks(files, ctx, workers);
        return;
    }
    auto &dict = ctx.identities;
    dict.shardSize = cap;
    dict.slotShift = 64 - Identities::shardBits - std::countr_zero(cap);
    dict.slots.resize(cap * shards);
    std::atomic<bool> overflow = false;
    const uint64_t empty = Key{}.bits;
    auto intern = [&](Key key, uint64_t h) -> uint32_t {
        const size_t base = Identities::shard(h) * cap;
        size_t pos = dict.home(h);
        for (size_t steps = 0; steps < cap; ++steps, pos = (pos + 1) & (cap - 1)) {
            auto idx = base + pos;
            auto slot = std::atomic_ref<uint64_t>(dict.slots[idx].raw.bits);
            uint64_t existing = slot.load(std::memory_order_acquire);
            if (existing == empty &&
                slot.compare_exchange_strong(existing, key.bits, std::memory_order_acq_rel,
                                             std::memory_order_acquire))
                return static_cast<uint32_t>(idx);
            if (Key{existing} == key)
                return static_cast<uint32_t>(idx);
        }
        overflow.store(true, std::memory_order_relaxed);
        return UINT32_MAX;
    };
    // Each edge points from its larger key to a smaller neighbor. Redirecting
    // an occupied edge leaves precisely one outstanding connection between
    // the old neighbor and the proposed new one; its greatest key decreases.
    auto merge = [&](Key high, Key low, uint64_t hash) {
        for (;;) {
            const uint32_t i = intern(high, hash);
            if (i == UINT32_MAX)
                return;
            auto cell = std::atomic_ref<uint64_t>(dict.slots[i].name.bits);
            uint64_t old = cell.load(std::memory_order_relaxed);
            for (;;) {
                if (old == empty) {
                    if (cell.compare_exchange_weak(old, low.bits, std::memory_order_relaxed))
                        return;
                    continue;
                }
                Key previous{old};
                if (previous == low)
                    return;
                if (previous < low) {
                    high = low;
                    low = previous;
                    break;
                }
                if (cell.compare_exchange_weak(old, low.bits, std::memory_order_relaxed)) {
                    high = previous;
                    break;
                }
            }
            hash = shopperRoutingHash(high);
        }
    };
    parallel(chunks.size(), workers, [&](size_t n) {
        struct Edge {
            Key high, low;
            uint64_t hash;
        };
        std::array<Edge, 32> pending;
        unsigned used = 0;
        auto flush = [&] {
            for (unsigned i = 0; i < used; ++i)
                merge(pending[i].high, pending[i].low, pending[i].hash);
            used = 0;
        };
        linkRows(chunks[n].rows, [&](Key a, Key b) {
            if (a == b || overflow.load(std::memory_order_relaxed))
                return;
            if (a < b)
                std::swap(a, b);
            const uint64_t h = shopperRoutingHash(a);
            __builtin_prefetch(&dict.slots[Identities::shard(h) * cap + dict.home(h)], 1);
            pending[used++] = {a, b, h};
            if (used == pending.size())
                flush();
        });
        flush();
    });
    if (overflow.load(std::memory_order_relaxed)) {
        fallbackLinks(files, ctx, workers);
        return;
    }
    dict.fingerprints.resize(dict.slots.size());
    parallel(shards, workers, [&](size_t sh) {
        size_t base = sh * cap;
        for (size_t i = base; i < base + cap; ++i) {
            auto raw = dict.slots[i].raw;
            if (!raw.empty()) {
                uint64_t h = shopperRoutingHash(raw);
                dict.fingerprints[base + dict.home(h)] |= Identities::fingerprint(h);
            }
        }
    });
    parallel(shards, workers, [&](size_t shard) {
        const size_t base = shard * cap;
        size_t live = 0;
        struct Path {
            uint32_t start;
            Key next;
            uint64_t hash;
        };
        constexpr unsigned width = 32;
        Path paths[width];
        unsigned pending = 0;
        auto flush = [&]() {
            while (pending) {
                unsigned active = 0;
                for (unsigned j = 0; j < pending; ++j) {
                    auto path = paths[j];
                    uint64_t h = path.hash;
                    size_t begin = Identities::shard(h) * cap, pos = dict.home(h);
                    uint32_t idx = UINT32_MAX;
                    if (dict.fingerprints[begin + pos] & Identities::fingerprint(h)) {
                        auto &key = path.next;
                        for (size_t step = 0; step < cap; ++step, pos = (pos + 1) & (cap - 1)) {
                            auto found = dict.slots[begin + pos].raw;
                            if (found.empty())
                                break;
                            if (found == key) {
                                idx = uint32_t(begin + pos);
                                break;
                            }
                        }
                    }
                    if (idx == UINT32_MAX) {
                        std::atomic_ref<uint64_t>(dict.slots[path.start].name.bits)
                            .store(path.next.bits, std::memory_order_relaxed);
                    } else {
                        path.next = {std::atomic_ref<uint64_t>(dict.slots[idx].name.bits)
                                         .load(std::memory_order_relaxed)};
                        path.hash = shopperRoutingHash(path.next);
                        __builtin_prefetch(&dict.fingerprints[path.hash >> dict.slotShift], 0, 3);
                        paths[active++] = path;
                    }
                }
                pending = active;
            }
        };
        for (size_t i = base; i < base + cap; ++i) {
            if (dict.slots[i].raw.empty())
                continue;
            ++live;
            Key next{std::atomic_ref<uint64_t>(dict.slots[i].name.bits).load(std::memory_order_relaxed)};
            auto h = shopperRoutingHash(next);
            __builtin_prefetch(&dict.fingerprints[h >> dict.slotShift], 0, 3);
            paths[pending++] = {uint32_t(i), next, h};
            if (pending == width)
                flush();
        }
        flush();
        if (live == cap)
            overflow.store(true, std::memory_order_relaxed);
    });
    if (overflow.load(std::memory_order_relaxed)) {
        fallbackLinks(files, ctx, workers);
    }
}
