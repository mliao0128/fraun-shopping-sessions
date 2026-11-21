// Build standalone with -ffunction-sections -Wl,--gc-sections.
#include "../ingest.cpp"
#include <cassert>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    assert(timestamp("2000-02-29T23:59:59Z") == 951868799000LL);
    assert(timestamp("1900-02-29T12:00:00Z") == invalidTime);
    assert(timestamp("1969-12-31T23:59:59Z") == -1000);
    assert(timestamp("1970-01-01T06:15:00+06:15") == 0);
    assert(timestamp("1970-01-01T06:15:00+06:60") == invalidTime);

    const auto page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    char* pages = static_cast<char*>(mmap(nullptr, 2*page, PROT_READ|PROT_WRITE,
                                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0));
    assert(pages != MAP_FAILED && mprotect(pages+page,page,PROT_NONE) == 0);
    char* digits = pages+page-13;
    std::memcpy(digits,"1234567890123",13);
    assert(timestamp({digits,13}) == 1234567890123LL);
    digits[12]='x';
    assert(timestamp({digits,13}) == invalidTime);
    assert(munmap(pages,2*page) == 0);

    Context ctx{};
    ctx.start = timestamp("2000-01-01T00:00:00Z");
    ctx.end = timestamp("2001-01-01T00:00:00Z");
    const std::string file = "header\n"
        "a,u,2000-02-29T23:59:59Z,2000-03-01T23:59:59Z,view,p,\n"
        "b,u,951868798999,2000-03-01T23:59:59Z,view,p,\n"
        "bad,un\"quoted,951868799000,2000-03-01T23:59:59Z,view,p,\n"
        "\"c\",\"u\",951868799001,2000-03-01T23:59:59Z,view,p,";
    for (size_t width : {1, 17, 64, 128, 4096}) {
        std::string observed;
        for (size_t begin=0; begin<file.size(); begin+=width) {
            chunkFields<7>({file,begin,std::min(begin+width,file.size())},
                [&](std::string_view, const auto& fields) {
                    ParsedEvent event;
                    assert(parseEvent(fields,ctx,event));
                    observed += fields[0];
                    observed += event.valid ? '+' : '-';
                });
        }
        assert(observed == "a+b-c+");
    }
}
