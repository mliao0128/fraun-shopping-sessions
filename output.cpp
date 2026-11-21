#include "shared.hpp"

#include <arrow/io/file.h>
#include <parquet/column_writer.h>
#include <parquet/file_writer.h>
#include <parquet/schema.h>
#include <span>

namespace pq = parquet;
namespace fs = std::filesystem;
namespace {
using Node = pq::schema::NodePtr;

Node stringField(const char* name, bool nullable = false) {
    return pq::schema::PrimitiveNode::Make(name, nullable ? pq::Repetition::OPTIONAL : pq::Repetition::REQUIRED,
                                           pq::Type::BYTE_ARRAY, pq::ConvertedType::UTF8);
}
Node integer(const char* name) {
    return pq::schema::PrimitiveNode::Make(name, pq::Repetition::REQUIRED, pq::Type::INT64);
}
Node dayField() {
    return pq::schema::PrimitiveNode::Make("day", pq::Repetition::REQUIRED, pq::Type::INT32, pq::ConvertedType::DATE);
}
auto writer(const fs::path& path, std::vector<Node> fields) {
    auto schema = std::static_pointer_cast<pq::schema::GroupNode>(
        pq::schema::GroupNode::Make("schema", pq::Repetition::REQUIRED, fields));
    auto stream = arrow::io::FileOutputStream::Open(path.string());
    if (!stream.ok()) throw std::runtime_error(stream.status().ToString());
    pq::WriterProperties::Builder props;
    props.disable_dictionary()->disable_statistics()->compression(pq::Compression::SNAPPY);
    return pq::ParquetFileWriter::Open(*stream, schema, props.build());
}

template<class ParquetType, class Rows, class Field>
void column(pq::RowGroupWriter* group, const Rows& rows, Field field) {
    std::vector<typename ParquetType::c_type> values;
    values.reserve(rows.size());
    for (const auto& r : rows) values.push_back(field(r));
    static_cast<pq::TypedColumnWriter<ParquetType>*>(group->NextColumn())
        ->WriteBatch(static_cast<int64_t>(rows.size()), nullptr, nullptr, values.data());
}
pq::ByteArray bytes(const Key& k, std::vector<Key::Buffer>& scratch) {
    const auto text = k.needsDecoding() ? k.view(scratch.emplace_back()) : k.borrowed();
    return {static_cast<uint32_t>(text.size()), reinterpret_cast<const uint8_t*>(text.data())};
}
class UserWriter {
    std::vector<pq::ByteArray> people, categories, categoryNames;
    std::vector<int16_t> present;
    std::vector<Key::Buffer> decodedPeople, decodedCategories;
public:
    explicit UserWriter(const Context& ctx) {
        decodedCategories.reserve(ctx.categories.size());
        categoryNames.reserve(ctx.categories.size());
        for (const Key& name : ctx.categories) categoryNames.push_back(bytes(name, decodedCategories));
    }

    void write(pq::ParquetFileWriter& file, const UserColumns& rows, size_t first, size_t n) {
        people.resize(n);
        present.resize(n);
        categories.clear();
        categories.reserve(n);
        decodedPeople.clear();
        decodedPeople.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const size_t row = first + i;
            people[i] = bytes(rows.people[row], decodedPeople);
            const uint32_t top = rows.top[row];
            present[i] = top != UINT32_MAX;
            if (present[i]) categories.push_back(categoryNames[top]);
        }
        auto* group = file.AppendRowGroup();
        static_cast<pq::ByteArrayWriter*>(group->NextColumn())->WriteBatch(n, nullptr, nullptr, people.data());
        static_cast<pq::Int32Writer*>(group->NextColumn())->WriteBatch(n, nullptr, nullptr, rows.days.data() + first);
        static_cast<pq::Int64Writer*>(group->NextColumn())->WriteBatch(n, nullptr, nullptr, rows.sessions.data() + first);
        static_cast<pq::Int64Writer*>(group->NextColumn())->WriteBatch(n, nullptr, nullptr, rows.events.data() + first);
        static_cast<pq::Int64Writer*>(group->NextColumn())->WriteBatch(n, nullptr, nullptr, rows.revenue.data() + first);
        static_cast<pq::ByteArrayWriter*>(group->NextColumn())->WriteBatch(n, present.data(), nullptr, categories.data());
        group->Close();
    }
};
struct CampaignRow { uint32_t id; int32_t day; CampaignStats stats; };
}

void writeOutputs(const fs::path& root, const Context& ctx, const std::vector<Result>& results, unsigned workers) {
    auto userDir = root / "user_daily", campaignDir = root / "campaign_daily";
    fs::create_directories(userDir);
    fs::create_directories(campaignDir);
    size_t files = std::min<size_t>(workers, results.size());
    parallel(files, workers, [&](size_t part) {
        auto file = writer(userDir / ("part-" + std::to_string(part) + ".parquet"), {
            stringField("user_id"), dayField(), integer("sessions"), integer("events"),
            integer("revenue_cents"), stringField("top_category", true)});
        UserWriter columns(ctx);
        for (size_t i = part; i < results.size(); i += files) {
            const UserColumns& rows = results[i].users;
            for (size_t start = 0; start < rows.people.size(); start += 65536) {
                const size_t count = std::min<size_t>(65536, rows.people.size() - start);
                columns.write(*file, rows, start, count);
            }
        }
        file->Close();
    });
    std::unordered_map<int32_t, std::vector<CampaignStats>> totals;
    for (const auto& result : results) for (const auto& [day, stats] : result.byDay) {
        auto& daily = totals.try_emplace(day, stats.size()).first->second;
        for (size_t i = 0; i < stats.size(); ++i) {
            daily[i].sessions += stats[i].sessions;
            daily[i].purchasing += stats[i].purchasing;
            daily[i].revenue += stats[i].revenue;
            daily[i].assists += stats[i].assists;
        }
    }
    std::vector<CampaignRow> rows;
    for (const auto& [day, daily] : totals)
        for (uint32_t i = 0; i < daily.size(); ++i)
            if (daily[i].sessions || daily[i].assists) rows.push_back({i, day, daily[i]});

    auto file = writer(campaignDir / "part-0.parquet", {
        stringField("campaign_id"),stringField("channel"),dayField(),integer("sessions"),
        integer("purchasing_sessions"),integer("revenue_cents"),integer("assisted_sessions")});
    if (!rows.empty()) {
        auto* g = file->AppendRowGroup();
        std::vector<Key::Buffer> scratch;
        scratch.reserve(rows.size());
        column<pq::ByteArrayType>(g, rows, [&](const auto& r){return bytes(ctx.campaignNames[r.id],scratch);});
        scratch.clear();
        column<pq::ByteArrayType>(g, rows, [&](const auto& r){return bytes(ctx.channels[r.id],scratch);});
        column<pq::Int32Type>(g, rows, [](const auto& r){return r.day;});
        column<pq::Int64Type>(g, rows, [](const auto& r){return r.stats.sessions;});
        column<pq::Int64Type>(g, rows, [](const auto& r){return r.stats.purchasing;});
        column<pq::Int64Type>(g, rows, [](const auto& r){return r.stats.revenue;});
        column<pq::Int64Type>(g, rows, [](const auto& r){return r.stats.assists;});
        g->Close();
    }
    file->Close();
}
