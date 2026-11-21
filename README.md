# Shopping Sessions

A C++20 entry for the Databricks Shopping Sessions Buildathon. It reads the mounted CSV files, resolves identities, and writes `user_daily` and `campaign_daily` as Parquet. The [latest paired benchmark](benchmarks/campaign-counters/benchmark.md) compares this build with our official 0.64 s submission and records complete output checks.

## Build and run

The default build targets Intel Cascade Lake on Linux x86-64. Run it on a compatible CPU with eight available logical CPUs and 24 GiB of memory. For an AVX2/BMI2 machine without AVX-512, build with the Docker build argument ARCH_FLAGS=-march=haswell. Run from the repository root. On a compatible build machine, GCC collects an execution profile on synthetic CSVs and uses it for the final compilation. A builder without the default target CPU features skips profiling and cross-compiles the scoring binary. The runtime image contains the executable and its shared libraries.

```sh
docker build --platform linux/amd64 -t shopping-sessions:latest .
python3 tools/benchmark.py shopping-sessions:latest /absolute/path/to/data ./sample --runs 1
```

The runner creates a new output directory for every sample. Its default CPU set is `0-7`. Use `--cpuset` to select eight available hardware threads on a different host. `--profile` prints time spent on each pipeline stage. An ordinary scoring container takes no arguments and uses `DATA_DIR`, `OUTPUT_DIR`, and `NCPU` from the environment.

The Docker build verifies the organizer's generator archive by SHA-256, builds the generator offline from that source, and creates a temporary synthetic input for GCC's profile-guided optimization. Only the solver and runtime libraries enter the final image. [Organizer's generator](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/generator.zip)

## Verify with the organizer's files

Download and unpack the Polars starter, generator and dev answer into one tooling directory outside the repository. The starter contains the official checker and 36 fixtures. Build the unmodified generator with Rust 1.85 or newer and use the starter's pinned Python dependencies. The commands below assume the current directory is the tooling directory and the submission image has already been built. [Official track downloads](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions)

```sh
cargo build --release --locked --manifest-path generator/Cargo.toml
python3 -m venv .venv
.venv/bin/python -m pip install -r starter-polars/requirements.txt
export IMAGE=shopping-sessions:latest
.venv/bin/python starter-polars/scripts/run_fixtures.py starter-polars/fixtures -- \
    sh starter-polars/scripts/docker_run.sh
generator/target/release/generate --seed 1 --events 1000000 --clicks 250000 --output dev
sh starter-polars/scripts/docker_run.sh dev dev-output
.venv/bin/python starter-polars/scripts/compare.py dev-output dev-answer
```

## Benchmark other sizes

From the tooling directory, set `SOLUTION` to the absolute path of this repository. The following seeds reproduce the local benchmark inputs.

```sh
generator/target/release/generate --seed 42 --events 20000000 --clicks 5000000 --output practice
generator/target/release/generate --seed 314159 --events 20000000 --clicks 5000000 --output holdout
generator/target/release/generate --seed 807154 --events 20000000 --clicks 5000000 --output independent-practice
generator/target/release/generate --seed 73 --events 60000000 --clicks 15000000 --output final
cc -O2 -static "$SOLUTION/tools/time_exec.c" -o time-exec
python3 "$SOLUTION/tools/benchmark.py" "$IMAGE" practice measured-practice --clock ./time-exec --runs 3
python3 "$SOLUTION/tools/benchmark.py" "$IMAGE" final measured-final --clock ./time-exec --runs 5
```

The optional Linux x86-64 timer measures the full child process with both `CLOCK_MONOTONIC_RAW` and `CLOCK_MONOTONIC` and records peak resident memory. The report also saves the container engine's start-to-exit timestamps and the elapsed client command time. Omitting `--clock` uses the engine timestamps.

For a cold-input storage test, add `--evict-input`. The runner checks that every input page was evicted from the local Linux page cache before starting each container. To limit reads as well, pass `--device-read-bps DEVICE:RATE` with the dataset's backing block device and a byte-per-second rate. For example, use `/dev/sdd:125000000` when the dataset resides on `/dev/sdd`.

To check a generated seed with an independent implementation, install DuckDB 2.0.1 and run the command below. The checker builds its own expected tables in the new scratch directory and compares every output row in both directions. Allow additional disk space for its intermediate SQL tables.

```sh
.venv/bin/python -m pip install duckdb==2.0.1
.venv/bin/python "$SOLUTION/tools/check_generated.py" practice measured-practice/run-1/output oracle-practice
```

## How the pipeline works

- CSV files are memory-mapped. Each reader scans a byte range and consumes the lines that start in it. AVX2 delimiter scans and BMI2 bit extraction check quoting and locate fields, preserving spaces and carriage returns. Long records use the general parser.
- A union-find over valid link edges computes connected components. A read-only hash table maps aliases to the bytewise smallest ID in each component, including IDs appearing only in link files.
- Parsing overlaps identity linking. Readers pack events into buckets using a hash of the raw user ID and group event IDs into separate shards. A bit filter identifies possible duplicate IDs in each shard. Exact ID and arrival-time comparisons decide which copy survives, including when an earlier invalid copy suppresses a later valid one.
- An ordinary duplicate record stores the arrival time and the staged event location in 16 bytes. Long time spans and large bucket indices take a full-width path. Losing copies are marked directly. After identity linking, workers keep unaliased events in place and move aliases to the bucket for their canonical user.
- The timestamp parser decodes paired event and arrival clocks together when possible. A shared date is converted once. Other supported timestamp forms retain their own conversion paths.
- Workers collect event times per shopper. For shoppers with many events in a window that fits 32-bit millisecond offsets, a bitmap enumerates occupied time intervals. Each interval is shorter than the 30-minute session gap, so its first event, last event and count are sufficient to recover exact session boundaries. Small groups and wide time windows use a full timestamp sort. Workers group purchases by shopper and sort within each shopper's span. Clicks are also sorted per shopper. A moving seven-day window tracks the last eligible click and the distinct campaigns present. Identical click copies enter and expire together, so the winner and the set of assists are invariant to their multiplicity.
- Workers write completed user columns in batches and reuse their buffers for subsequent buckets. The Parquet writer uses compact dictionaries for short date ranges, counts and repeated strings. It uses full-width PLAIN integer values for revenue and as a fallback, and writes definition levels for nullable categories. Workers merge campaign totals before writing the campaign table. Every run derives its time window, identities, product categories and campaign metadata from the mounted data.

## Dependencies and license

This build extends the Fraun pipeline and [Jerry Hu's staged-scatter and radix implementation](https://github.com/Jerryh127/fraun-shopping-sessions/commit/e315a0296ecc6f9c05c31d57309dc308f6a549b9). It uses GCC, the C++ standard library, Linux memory mapping, and the LZ4 runtime library supplied by Debian bookworm. The Python benchmark runner uses the standard library. The organizer provides the generator, checker, fixtures and expected outputs. The submission code is released under the [MIT License](LICENSE).
