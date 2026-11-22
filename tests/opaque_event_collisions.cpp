#include "../ingest.cpp"
#include <cassert>

// Force unrelated IDs into the same probe sequence. Independent readers
// hold different addresses for equal text, as happens across CSV files.
int main() {
    const std::array<std::string, 3> first = {
        "two-different-opaque-keys-107358", "two-different-opaque-keys-27090",
        std::string(70000, 'x')
    };
    const auto second = first;
    const Key shopper = Key::from("shopper");
    std::vector<std::unique_ptr<Reader>> readers;
    for (const auto* names : {&first, &second}) {
        auto& reader = *readers.emplace_back(std::make_unique<Reader>(1, 1));
        reader.externalHashes.resize(1);
        for (const auto& name : *names) {
            auto token = encodeEventToken(eventID(name));
            assert(token.external());
            const int64_t arrival = names == &first ? 300 : 200;
            auto row = reader.events.add(0, Event::pack(shopper, 0, {arrival, 0, 0}, shopper.hash()));
            reader.ids.add(0, IdRecord{token, location(reader, arrival, 0, 0, row)});
            reader.externalHashes[0].push_back(91);
        }
        reader.ids.finish();
        reader.events.finish();
    }
    deduplicate(0, 0, Source{}, readers);
    for (size_t i = 0; i < first.size(); ++i) {
        assert(readers[0]->events.at(0, i).person.empty());
        assert(readers[1]->events.at(0, i).person == shopper);
    }
}
