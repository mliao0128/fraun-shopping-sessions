#!/bin/sh
set -eu
arrow_dir=${ARROW_DIR:-$(python3 -c 'import os, pyarrow; print(os.path.dirname(pyarrow.__file__))')}
arch_flags=${ARCH_FLAGS:--march=cascadelake -mtune=cascadelake}
output=${1:-shopping-sessions}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
compile() {
  ${CXX:-g++} -std=c++20 -O3 -DNDEBUG $arch_flags -flto=auto -pthread "$@" \
    -I"$arrow_dir/include" main.cpp links.cpp ingest.cpp aggregate.cpp output.cpp \
    -L"$arrow_dir" -Wl,-rpath,"$arrow_dir" -l:libparquet.so.2000 -l:libarrow.so.2000 -o "$output"
}
# A build host with an older CPU can still cross-compile the scorer binary.
case "$arch_flags" in
    *cascadelake*)
        for flag in avx512f avx512bw avx512vl avx512dq avx512cd avx512_vnni; do
            if ! grep -qw "$flag" /proc/cpuinfo; then compile; exit 0; fi
        done ;;
esac
# The normal executable exits immediately after closing Parquet. Flush GCC's
# counters at precisely that point in the temporary instrumented executable.
cat >"$work/dump.c" <<'C'
extern void __gcov_dump(void);
extern void __real__exit(int);
void __wrap__exit(int status) { __gcov_dump(); __real__exit(status); }
C
cc -c "$work/dump.c" -o "$work/dump.o"
compile "-fprofile-generate=$work/counters" "$work/dump.o" -Wl,--wrap=_exit
python3 training.py "$work/input"
NCPU=2 "$output" "$work/input" "$work/output"
compile "-fprofile-use=$work/counters" -fprofile-correction -Werror=missing-profile
