#include "../ingest.cpp"
#include <cassert>

int main() {
    Context ctx;
    ctx.start = timestamp("1969-01-01T00:00:00Z");
    ctx.end = timestamp("2030-01-01T00:00:00Z");

    // The arrival on a well-formed but late event is still needed for deduplication.
    auto check = [&](std::string_view event, std::string_view arrival, bool valid) {
        int64_t eventMs = invalidTime, arrivalMs = invalidTime;
        const auto expectedEvent = timestamp(event);
        const bool accepted = eventTimes(event, arrival, ctx, eventMs, arrivalMs);
        assert(accepted == valid);
        if (expectedEvent == invalidTime || expectedEvent < ctx.start || expectedEvent >= ctx.end) {
            assert(arrivalMs == invalidTime);
        } else {
            assert(eventMs == expectedEvent);
            assert(arrivalMs == timestamp(arrival));
        }
    };

    check("2024-02-29T23:59:59Z", "2024-02-29T23:59:59Z", true);
    check("2024-03-01T00:15:00+05:30", "2024-02-29T21:45:01Z", true);
    check("1969-12-31T21:00:00-03:00", "1970-01-02T00:00:00Z", true);
    check("1969-12-31T21:00:00-03:00", "1970-01-02T00:00:01Z", false);
    check("2030-01-01T00:00:00Z", "2029-12-31T23:59:59Z", false);
    check("2023-02-29T00:00:00Z", "2024-02-29T00:00:00Z", false);
    check("2024-02-29T22:59:59+24:00", "2024-02-29T23:59:59Z", false);
    for (unsigned i : {4, 7, 10, 11, 13, 16, 19}) {
        std::string malformed = "2024-02-29T23:59:59Z";
        malformed[i] = 'X';
        check(malformed, "2024-02-29T23:59:59Z", false);
    }
}
