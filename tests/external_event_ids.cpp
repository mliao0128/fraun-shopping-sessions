// Compile with -ffunction-sections -Wl,--gc-sections as for locator.cpp.
#include "../ingest.cpp"
#include <cassert>
#include <string>

int main() {
    std::vector<std::unique_ptr<Reader>> readers;
    for (unsigned i = 0; i < 3; ++i) {
        readers.push_back(std::make_unique<Reader>(1, 1));
        readers.back()->externalIds.resize(1);
    }
    // Each copy lives in a different allocation. All distinct names below
    // deliberately share an entire hash, including the shard and slot bits.
    const std::string prefix = "exact-external-";
    std::vector<std::string> names{prefix + "A", prefix + "B", std::string(16383, 'x'),
        std::string(16384, 'x'), std::string(65539, 'z'), "\xc3\xa9-identifiant-externe",
        std::string("long\0binary-id", 14), std::string(65539, 'z')};
    names.back().back() = 'y';
    std::vector<std::vector<std::string>> copies(3, names);
    Source source;
    const Key shopper = Key::from("buyer");
    auto add = [&](size_t r, size_t id, int64_t time, bool valid) {
        auto& reader = *readers[r];
        uint64_t info = 0;
        if (valid) {
            uint32_t pos = reader.events.add(0, Event::pack(shopper, 0, {time, 0, 0}, shopper.hash()));
            info = location(reader, time, 0, 0, pos);
        } else {
            info = (relativeArrival(time, 0) << 32) | invalidArrivalTag;
        }
        const auto key = eventID(copies[r][id]);
        assert(key.external());
        reader.externalIds[0].push_back({key, info});
    };
    for (size_t i = 0; i < names.size(); ++i) {
        add(0, i, 401, true);
        add(1, i, 400, true);
        add(2, i, i % 2 ? 399 : 402, false);
    }
    // One malformed copy has to reread its arrival from the original input.
    // Its earlier time removes the otherwise valid long-ID winner.
    const std::string malformed = names[2] + ",u,wrong,0000000000398,view,p,\n";
    source.starts.push_back(0);
    source.files.push_back(malformed);
    readers[2]->externalIds[0].push_back({eventID(copies[2][2]), special(0, sourceTag)});

    // A valid copy outside the packed arrival range tests the slow locator.
    add(2, 4, int64_t{1} << 36, true);
    // Tied arrivals retain one physically identical copy.
    add(2, 0, 400, true);
    for (auto& reader : readers) reader->events.finish();
    deduplicateExternal(0, 0, source, readers, [](Key) { return uint64_t{0}; });
    for (size_t i = 0; i < names.size(); ++i) {
        assert(readers[0]->events.at(0, i).person.empty());
        assert(readers[1]->events.at(0, i).person.empty() == (bool(i % 2) || i == 2));
    }
    assert(readers[2]->events.at(0, 0).person.empty());  // wide-arrival loser
    assert(readers[2]->events.at(0, 1).person.empty());  // tied duplicate
}
