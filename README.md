# Shopping Sessions

A C++20 entry for the Databricks Shopping Sessions Buildathon. It reads the mounted CSV files, resolves identities, and writes `user_daily` and `campaign_daily` as Parquet. See [benchmark results](BENCHMARK.md) for the tested environment, correctness checks, and timings.

## Build and run

Use a Linux x86-64 machine with AVX2, eight available logical CPUs and 24 GiB of memory. Run from the repository root.

```sh
docker build --platform linux/amd64 -t shopping-sessions:latest .
python3 tools/benchmark.py shopping-sessions:latest /absolute/path/to/data ./sample --runs 1
```

The runner creates a new output directory for every sample. Its default CPU set is `0-7`. Use `--cpuset` to select eight available hardware threads on a different host. `--profile` prints time spent on the four pipeline stages. An ordinary scoring container takes no arguments and uses `DATA_DIR`, `OUTPUT_DIR`, and `NCPU` from the environment.

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

From the tooling directory, set `SOLUTION` to the absolute path of this repository. The two seeds below reproduce the local benchmark inputs.

```sh
generator/target/release/generate --seed 42 --events 20000000 --clicks 5000000 --output practice
generator/target/release/generate --seed 73 --events 60000000 --clicks 15000000 --output final
cc -O2 -static "$SOLUTION/tools/time_exec.c" -o time-exec
python3 "$SOLUTION/tools/benchmark.py" "$IMAGE" practice measured-practice --clock ./time-exec --runs 3
python3 "$SOLUTION/tools/benchmark.py" "$IMAGE" final measured-final --clock ./time-exec --runs 5
```

The optional Linux x86-64 timer measures the full child process with `CLOCK_MONOTONIC_RAW` and records peak resident memory. The report also saves the container engine's start-to-exit timestamps and the elapsed client command time. Omitting `--clock` uses the engine timestamps.

To check a generated seed with an independent implementation, install DuckDB 2.0.1 and run the command below. The checker builds its own expected tables in the new scratch directory and compares every output row in both directions. Allow additional disk space for its intermediate SQL tables.

```sh
.venv/bin/python -m pip install duckdb==2.0.1
.venv/bin/python "$SOLUTION/tools/check_generated.py" practice measured-practice/run-1/output oracle-practice
```

## How the pipeline works

- CSV files are memory-mapped. Parallel readers split large files at line boundaries. AVX2 delimiter scans validate quote placement and retain field bytes exactly, including spaces and carriage returns.
- A union-find over valid link edges computes connected components. A read-only hash table maps aliases to the bytewise smallest ID in each component, including IDs appearing only in link files.
- Events are partitioned by event ID. Exact comparisons select the earliest ingestion timestamp even when that record is invalid or names a different person. Workers route surviving events to the canonical person's bucket.
- Each person’s events and valid clicks are sorted by time. Event gaps determine session boundaries. A moving seven-day window tracks the last eligible click and the distinct campaigns present. Identical click copies enter and expire together, so the winner and the set of assists are invariant to their multiplicity.
- Workers aggregate user rows and campaign totals in memory. Apache Parquet writes user files in parallel and emits the final campaign table. Every run derives its time window, identities, product categories and campaign metadata from the mounted data.

## Dependencies and license

The pipeline is original C++ code. It uses GCC, the C++ standard library, Linux memory mapping, and the Apache Arrow and Parquet C++ libraries distributed in the PyArrow 20.0.0 wheel. The Python benchmark runner uses the standard library. The official generator, checker, fixtures and expected outputs are external test inputs. The submission code is released under the [MIT License](LICENSE).
