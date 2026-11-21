# Compatible integer Parquet output

The official scorer rejected commit `62586a9` before timing because its reader accepts BYTE_STREAM_SPLIT only for floating-point columns. The DuckDB starter pins version 1.5.6, which reproduces the failure. This build writes user-day dates and counts using integer dictionaries with packed indices when appropriate. It writes full-width PLAIN values for revenue and for data outside the dictionary limits.

## Intel comparison

A six-core Intel Core i5-8500 ran the reference `05a39b3` and the corrected build against the same warm input, writing fresh output for every run. The native binaries target Haswell. For the first five 20M inputs and the 60M input, each of four builds had eight measured runs after warmup, with the order shuffled each round. Seed 1724859792 was drawn after choosing the packed writer. On that seed the reference and candidate alternated for twelve measured runs after warmup. Times below are median solver process times. The estimate scales the official reference time of 0.84 s by the ratio of the local medians.

| Seed | Events | Reference | Candidate | Runtime reduction | Intel practice estimate |
|---|---:|---:|---:|---:|---:|
| 42 | 20M | 0.7359 s | 0.4962 s | 32.6% | 0.566 s |
| 807154 | 20M | 0.7235 s | 0.4932 s | 31.8% | 0.573 s |
| 1599080644 | 20M | 0.7188 s | 0.4834 s | 32.8% | 0.565 s |
| 1376142809 | 20M | 0.7401 s | 0.4919 s | 33.5% | 0.558 s |
| 1896044346 | 20M | 0.7144 s | 0.4901 s | 31.4% | 0.576 s |
| 1724859792 | 20M | 0.6968 s | 0.4784 s | 31.3% | 0.577 s |
| 73 | 60M | 2.1010 s | 1.4277 s | 32.0% | — |

## Cross-check on the release binary

A Ryzen 9 9950X ran the Cascade Lake binaries extracted from both release images on eight pinned hardware threads (four physical cores), with the release allocator settings. Each pair alternated for ten measured runs after a warmup. The two hosts give materially different projections for the official Xeon.

| Seed | Reference | Candidate | Ryzen-scaled practice estimate |
|---|---:|---:|---:|
| 42 | 0.3537 s | 0.2881 s | 0.684 s |
| 1896044346 | 0.3757 s | 0.3072 s | 0.687 s |

## Verification

DuckDB 1.5.6 compared both full tables to the published CSV answers on all 36 fixtures and public dev. Full sorted table hashes from the timed Intel binaries and the Cascade Lake release container matched independent SQL answers on all six 20M seeds and the 60M seed. The new public holdout also passed a complete bidirectional SQL comparison. The same reader reproduces the archived official public-dev failure. To repeat the published-answer check, install `duckdb==1.5.6` and run `python tools/check_parquet_compatibility.py dev-answer OUTPUT`.

[Individual timings](timings.json) · [Row counts and full hashes](verification.json) · [Official DuckDB starter](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/starter-duckdb.zip)
