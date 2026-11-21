#!/bin/sh
set -eu
default_arch='-march=cascadelake -mtune=cascadelake'
arch_flags=${ARCH_FLAGS:-$default_arch}
output=${1:-shopping-sessions}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
compile() {
  ${CXX:-g++} -std=c++20 -O3 -DNDEBUG $arch_flags -static-libstdc++ -static-libgcc -Wa,-mbranches-within-32B-boundaries -flto=auto -pthread "$@" \
    main.cpp links.cpp ingest.cpp aggregate.cpp output.cpp -l:liblz4.so.1 -o "$output"
}
# Keep training and release flags identical so GCC can use every counter.
case "$arch_flags" in
    *cascadelake*)
        for flag in avx512f avx512bw avx512vl avx512dq avx512cd avx512_vnni; do
            if grep -qw "$flag" /proc/cpuinfo; then continue; fi
            if [ "$arch_flags" = "$default_arch" ]; then
                echo 'int main(void) { return !__builtin_cpu_supports("x86-64-v3"); }' > "$work/probe.c"
                cc -march=x86-64 "$work/probe.c" -o "$work/probe"
                if "$work/probe"; then
                    arch_flags='-march=x86-64-v3 -mtune=haswell'
                    break
                fi
            fi
            echo "Cross-compiling without training: $arch_flags" >&2
            compile
            exit 0
        done ;;
esac
echo "Profiling and compiling: $arch_flags" >&2
# The normal executable exits immediately after closing Parquet. Flush GCC's
# counters at precisely that point in the temporary instrumented executable.
cat >"$work/dump.c" <<'C'
extern void __gcov_dump(void);
extern void __real__exit(int);
void __wrap__exit(int status) { __gcov_dump(); __real__exit(status); }
C
cc -c "$work/dump.c" -o "$work/dump.o"
compile "-fprofile-generate=$work/counters" "$work/dump.o" -Wl,--wrap=_exit
if command -v shopping-profile-generator >/dev/null 2>&1; then
  shopping-profile-generator --seed 18323567 --events 2500000 --clicks 500000 --output "$work/input"
else
  python3 training.py "$work/input"
fi
NCPU=8 "$output" "$work/input" "$work/output"
compile "-fprofile-use=$work/counters" -fprofile-correction -Werror=missing-profile
