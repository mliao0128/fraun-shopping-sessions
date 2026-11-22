#include "../links.cpp"
#include <cassert>
#include <iostream>

int main() {
    using Shard = Identities::FallbackShard;
    assert(Shard::capacity(0) == 0);
    assert(Shard::capacity(400) == 1024);
    assert(Shard::capacity(2'800'000) == 8'388'608);
    assert(Shard::capacity(5'600'000) == 16'777'216);

    const std::string canonical = "!root";
    std::string links = "left_id,right_id\n";
    std::array<size_t, Identities::shards> target{}, actual{};
    target[0] = 400;
    target[45] = 43;
    target[127] = 1;
    std::vector<std::string> aliases;
    for (size_t i = 0; aliases.size() < 444; ++i) {
        std::string name = "opaque-" + std::to_string(i);
        const size_t shard = Identities::shard(shopperRoutingHash(Key::from(name)));
        if (actual[shard] == target[shard]) continue;
        ++actual[shard];
        links += name + "," + canonical + "\n";
        aliases.push_back(std::move(name));
    }
    Context context;
    fallbackLinks({links}, context, 3);
    const auto& identities = context.identities;
    assert(identities.fallback && identities.slots.empty());
    size_t slots = 0;
    for (size_t i = 0; i < target.size(); ++i) {
        const auto& group = (*identities.fallback)[i];
        assert(group.slots.size() == Shard::capacity(target[i]));
        assert(group.slots.size() == group.fingerprints.size());
        slots += group.slots.size();
    }
    assert(slots == 1024 + 128 + 2);
    const auto check = [&](std::string_view raw, std::string_view expected) {
        const Key before = Key::from(raw), after = Key::from(expected);
        uint64_t h = shopperRoutingHash(before);
        assert(identities.person(before, h) == after && h == shopperRoutingHash(after));
    };
    for (const auto& alias : aliases) check(alias, canonical);
    check(canonical, canonical);
    // Require an absent real string whose home slot and fingerprint hit.
    // It must still be rejected by full key equality.
    for (size_t i = 0;; ++i) {
        const auto name = "absent-" + std::to_string(i);
        const uint64_t h = shopperRoutingHash(Key::from(name));
        const auto& group = (*identities.fallback)[Identities::shard(h)];
        if (group.slots.empty()) continue;
        const size_t pos = (h >> group.shift) & (group.slots.size() - 1);
        if (!(group.fingerprints[pos] & Identities::fingerprint(h))) continue;
        check(name, name);
        break;
    }
    std::cout << "444 aliases across 3 shards, 1154 slots, all resolutions and a false fingerprint hit matched\n";
}
