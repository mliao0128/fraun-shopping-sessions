# Shopping Sessions against the practice leaderboard

On November 20, 2026, the updated pipeline completed two locally generated practice inputs in **1.00 s** and **0.95 s** with a warm Linux input cache. With that cache cleared, the second seed took **1.31 s**. At 6:39 p.m. Eastern, the official board showed martin-brooke at **7.14 s**, improved from the **16.38 s** that prompted this comparison. Bob the builder led at **2.37 s**. The local machine uses an AMD Ryzen 9 9950X and different input seeds from the official Intel Xeon scorer. [Live leaderboard](https://www.joinbuilders.org/buildathon/leaderboard)

## Paired results

Each cell is the median of three complete runs. The previous commit and the updated image ran in interleaved order on the same eight logical CPUs. The inputs and correctness checks were identical for each pair.

| Dataset | Previous commit `b78bed7` | Updated | Time reduction |
|---|---:|---:|---:|
| 20M events + 5M clicks, seed 42 | 1.349 s | 0.999 s | 26% |
| 20M events + 5M clicks, fresh seed 314159 | 1.239 s | 0.950 s | 23% |
| 60M events + 15M clicks, seed 73 | 4.947 s | 3.648 s | 26% |

The CSV parser extracts delimiters and validates quote boundaries in parallel for common record lengths. Each reader prefetches up to 32 identity lookups, obtains temporary blocks from a per-reader allocation pool, and locates its own line boundaries. Snappy reduced final-size output and scratch from approximately **272 MiB to 65 MiB**. Peak process RSS at final size was **9.78 GiB**, down from **10.43 GiB**. [Per-run timings, disk counters and row counts](leaderboard-local.json)

## Cold-input storage tests

Each row contains three new containers. The 1.92 GB seed-42 input was a distinct physical copy on ext4. The container's block-device read cap used decimal MB/s. Targeted eviction and Linux `mincore` confirmed zero resident input pages before each run. The block-device counters recorded 1,915,793,408 bytes read on each capped run. The underlying Windows and device caches were uncontrolled.

| Input and storage condition | Median elapsed time |
|---|---:|
| Seed 314159, local storage at full speed | 1.31 s |
| Seed 42, 1,000 MB/s read cap | 2.43 s |
| Seed 42, 500 MB/s read cap | 4.30 s |
| Seed 42, 250 MB/s read cap | 8.07 s |
| Seed 42, 125 MB/s read cap | 15.72 s |

The 500 MB/s and 250 MB/s trials fell on opposite sides of martin-brooke's updated 7.14 s. The 1,000 MB/s trial was close to the 2.37 s lead. Actual input residency and storage throughput on the hidden scorer are unknown.

## Verification and timing

The submission image passed all **36 official fixtures** and matched the published dev answer across **117,038 user-day** and **27,400 campaign-day** rows. The independent Python and DuckDB checker matched both tables in all **33** reported runs. The fresh practice seed contained 2,218,865 user-day rows and 28,014 campaign-day rows. The final-size seed contained 6,612,082 and 28,014 respectively. An AddressSanitizer and UndefinedBehaviorSanitizer build passed the fixtures and a multi-megabyte record test spanning reader boundaries.

The reported time uses Linux `CLOCK_MONOTONIC` around the complete executable, including startup, reads, output writes and exit. The WSL wall clock stepped during several repetitions, so the engine's start and end timestamps could understate duration. The raw file retains the engine times and both monotonic clocks. Runs pinned four physical cores with both SMT threads, enforced 24 GiB with zero swap, disabled networking, used UID 1000, and used fresh writable output and scratch directories.

The README includes commands for the official generator, exact checker and benchmark runner. Use seed `314159` with `--events 20000000 --clicks 5000000` for the fresh holdout. For a cold, read-limited trial, pass `--clock ./time-exec --evict-input --device-read-bps DEVICE:RATE --runs 3` to the benchmark runner. The reported values are in `median_monotonic_seconds`. [Reproduction instructions](../README.md)
