#include "shared.hpp"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <numeric>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using SV = std::string_view;

void DimensionIndex::finish() {
    families = {};
    std::array<size_t, 8> counts{};
    visit([&](Key k, uint32_t) {
        unsigned n = group(k);
        if (n >= families.size())
            return;
        auto &f = families[n];
        if (!counts[n]++)
            f.fixed = k.bits;
        f.variable |= f.fixed ^ k.bits;
    });
    size_t total = 0;
    for (unsigned n = 0; n < families.size(); n++) {
        auto &f = families[n];
        unsigned bits = std::popcount(f.variable);
        if (!counts[n] || bits > 20)
            continue;
        size_t slots = size_t{1} << bits;
        if (slots > counts[n] * 32)
            continue;
        f.fixedMask = _bzhi_u64(UINT64_MAX, n * 8) & ~f.variable;
        f.fixed &= f.fixedMask;
        f.base = static_cast<uint32_t>(total);
        total += slots;
    }
    values.assign(total, UINT32_MAX);
    visit([&](Key k, uint32_t value) {
        unsigned n = group(k);
        if (n >= families.size())
            return;
        auto &f = families[n];
        if (f.base != UINT32_MAX)
            values[f.base + _pext_u64(k.bits, f.variable)] = value;
    });
    ready = true;
}
void Index::reserve(size_t needed) {
    size_t capacity = std::bit_ceil(std::max<size_t>(16, needed + needed / 2 + 1));
    if (capacity <= slots.size()) return;
    std::vector<Slot> old;
    old.swap(slots);
    slots.resize(capacity);
    for (const Slot& entry : old)
        if (entry.value != UINT32_MAX) slots[locate(entry.key)] = entry;
}

uint32_t Index::insert(Key key, uint32_t value) {
    if ((occupied + 1) * 3 >= slots.size() * 2) reserve(occupied + 1);
    Slot& entry = slots[locate(key)];
    if (entry.value == UINT32_MAX) {
        entry = {key, value};
        ++occupied;
    }
    return entry.value;
}

namespace {
struct Mapping {
    const char* data = nullptr;
    size_t size = 0;
    explicit Mapping(const fs::path& path) {
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("Cannot open " + path.string());
        struct stat info{};
        if (fstat(fd, &info) < 0 || info.st_size < 0) {
            close(fd);
            throw std::runtime_error("Cannot stat " + path.string());
        }
        size = static_cast<size_t>(info.st_size);
        if (size) data = static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
        close(fd);
        if (data == MAP_FAILED) throw std::runtime_error("Cannot map " + path.string());
    }
    ~Mapping() { if (size) munmap(const_cast<char*>(data), size); }
    Mapping(const Mapping&) = delete;
};
struct Inputs {
    std::vector<std::unique_ptr<Mapping>> maps;
    SV file(const fs::path& path) {
        auto& m = maps.emplace_back(std::make_unique<Mapping>(path));
        return m->size ? SV(m->data, m->size) : SV{};
    }
    std::vector<SV> folder(const fs::path& path) {
        std::vector<SV> files;
        for (auto& entry : fs::directory_iterator(path))
            if (entry.is_regular_file() && entry.path().extension() == ".csv")
                files.push_back(file(entry.path()));
        return files;
    }
};

template<class F> void rows(SV file, F&& fn) {
    size_t pos = file.find('\n');
    if (pos == SV::npos) return;
    for (++pos; pos < file.size();) {
        size_t end = file.find('\n', pos);
        if (end == SV::npos) end = file.size();
        fn(file.substr(pos, end - pos));
        pos = end + 1;
    }
}
int64_t manifestTime(SV text, SV key) {
    std::string label = "\"" + std::string(key) + "\"";
    size_t field = text.find(label);
    if (field != SV::npos) {
        size_t colon = text.find(':', field + label.size());
        size_t first = colon == SV::npos ? SV::npos : text.find('"', colon + 1);
        size_t last = first == SV::npos ? SV::npos : text.find('"', first + 1);
        if (last != SV::npos) {
            int64_t t = timestamp(text.substr(first + 1, last - first - 1));
            if (t != INT64_MIN) return t;
        }
    }
    throw std::runtime_error("Invalid manifest field " + std::string(key));
}
void dimensions(const fs::path& data, Inputs& inputs, Context& ctx, unsigned workers) {
    auto manifest = inputs.file(data / "manifest.json");
    ctx.start = manifestTime(manifest, "window_start");
    ctx.end = manifestTime(manifest, "window_end");
    const Key unknown = Key::from("unknown");
    ctx.categories.push_back(unknown);
    SV products = inputs.file(data / "products.csv");
    Index seen;
    rows(products, [&](SV line) {
        std::array<SV,2> f;
        if (!csv(line,f)) throw std::runtime_error("Invalid product row");
        Key category = Key::from(f[1]);
        uint32_t next = static_cast<uint32_t>(ctx.categories.size());
        if (seen.insert(category, next) == next) ctx.categories.push_back(category);
    });
    std::sort(ctx.categories.begin(), ctx.categories.end());
    Index categoryIds;
    for (uint32_t i = 0; i < ctx.categories.size(); ++i) categoryIds.insert(ctx.categories[i], i);
    ctx.unknown = categoryIds.find(unknown);
    ctx.products.reserve(products.size() / 14);
    rows(products, [&](SV line) {
        std::array<SV,2> f;
        if (!csv(line,f)) throw std::runtime_error("Invalid product row");
        ctx.products.insert(Key::from(f[0]), categoryIds.find(Key::from(f[1])));
    });
    ctx.campaignNames.push_back(Key::from("organic"));
    ctx.channels.push_back(Key::from("organic"));
    rows(inputs.file(data / "campaigns.csv"), [&](SV line) {
        std::array<SV,2> f;
        if (!csv(line,f)) throw std::runtime_error("Invalid campaign row");
        uint32_t i = static_cast<uint32_t>(ctx.campaignNames.size());
        ctx.campaignNames.push_back(Key::from(f[0]));
        ctx.channels.push_back(Key::from(f[1]));
        ctx.campaigns.insert(ctx.campaignNames.back(), i);
    });
    ctx.products.finish();
    ctx.campaigns.finish();
}
const char* env(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}
}

int main(int argc, char** argv) try {
    fs::path data = argc > 1 ? argv[1] : env("DATA_DIR","/data");
    fs::path output = argc > 2 ? argv[2] : env("OUTPUT_DIR","/output");
    unsigned workers = static_cast<unsigned>(std::clamp(std::stoi(env("NCPU","8")), 1, 64));
    bool profiling = std::getenv("SHOP_PROFILE") != nullptr;
    auto start = std::chrono::steady_clock::now(), previous = start;
    auto stage = [&](const char* name) {
        auto now = std::chrono::steady_clock::now();
        if (profiling) std::cerr << name << " " << std::chrono::duration<double>(now - previous).count() << " s\n";
        previous = now;
    };
    Inputs inputs;
    Context ctx;
    dimensions(data, inputs, ctx, workers);
    // Identity linking overlaps with parsing, which needs only the small tables.
    auto links = inputs.folder(data / "links");
    std::exception_ptr linkError;
    std::thread linker([&] {
        try { linkPeople(links, ctx, std::max(1u, workers / 2)); }
        catch (...) { linkError = std::current_exception(); }
    });
    auto awaitIdentities = [&] {
        if (linker.joinable()) linker.join();
        if (linkError) std::rethrow_exception(linkError);
    };
    auto events = inputs.folder(data / "events");
    auto clicks = inputs.folder(data / "clicks");
    size_t bytes = 0;
    for (auto e : events) bytes += e.size();
    // More buckets balance large people across workers. Keep small datasets
    // compact, since each active bucket accumulates its own campaign totals.
    size_t bucketCount = std::clamp(std::bit_ceil(bytes / (1u << 20)), size_t{1}, size_t{512});
    std::vector<Bucket> buckets(bucketCount);
    stage("dimensions");
    try {
        ingest(ctx, events, clicks, buckets, workers, awaitIdentities);
    } catch (...) {
        if (linker.joinable()) linker.join();
        throw;
    }
    stage("ingest");
    // One result per worker: campaign totals are a dense table per day, far
    // too large to keep once per bucket.
    std::vector<size_t> order(bucketCount);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](auto a,auto b) {
        return buckets[a].events.size > buckets[b].events.size;
    });
    const unsigned aggregators = static_cast<unsigned>(std::min<size_t>(workers, bucketCount));
    std::vector<Result> results(aggregators);
    for (auto& r : results) r.users.reserve(32768);
    std::atomic<size_t> nextBucket{0};
    writeOutputs(output,ctx,results,workers,[&](size_t w) {
        const size_t i=nextBucket.fetch_add(1,std::memory_order_relaxed);
        if (i >= bucketCount) return false;
        aggregate(buckets[order[i]],results[w],ctx.categories.size(),ctx.campaignNames.size(),ctx.start);
        return true;
    });
    stage("aggregate-output");
    // Everything is written and closed. Skip unmapping gigabytes on the way out.
    std::cerr.flush();
    _exit(0);
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
