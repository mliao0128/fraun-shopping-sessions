// Optional Linux benchmark observer. Build statically and mount read-only.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long clock_ns(clockid_t clock_id) {
    struct timespec ts;
    if (clock_gettime(clock_id, &ts) != 0) {
        perror("clock_gettime");
        _exit(125);
    }
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc < 2) return 125;
    long long start = clock_ns(CLOCK_MONOTONIC_RAW);
    long long monotonic_start = clock_ns(CLOCK_MONOTONIC);
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 125; }
    if (child == 0) {
        execv(argv[1], argv + 1);
        perror("execv");
        _exit(127);
    }
    int status;
    struct rusage usage;
    while (wait4(child, &status, 0, &usage) < 0) {
        if (errno == EINTR) continue;
        perror("wait4");
        return 125;
    }
    long long elapsed = clock_ns(CLOCK_MONOTONIC_RAW) - start;
    long long monotonic_elapsed = clock_ns(CLOCK_MONOTONIC) - monotonic_start;
    fprintf(stderr, "SHOPPING_TIMER {\"process_ns\":%lld,\"monotonic_ns\":%lld,\"peak_rss_kib\":%ld}\n",
            elapsed, monotonic_elapsed, usage.ru_maxrss);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
