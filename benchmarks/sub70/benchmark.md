# Shopping Sessions benchmark against the 0.87 s build

The reference is the [submitted 0.87 s build](https://github.com/mliao0128/fraun-shopping-sessions/commit/53f036fc19dd1a206824b29ca2711dd0308fe1d8). Times are elapsed seconds from starting the executable through its exit, including Parquet writes and process cleanup.

| Generated input | Reference median | Candidate median | Reduction | Faster pairs | Projected practice score |
|---|---:|---:|---:|---:|---:|
| 20M events + 5M clicks, seed 42 | 0.6367 s | 0.5285 s | 17.0% | 6/6 | 0.722 s |
| 20M events + 5M clicks, seed 314159 | 0.6333 s | 0.5517 s | 12.9% | 6/6 | 0.758 s |
| 60M events + 15M clicks, seed 73 | 2.0029 s | 1.5840 s | 20.9% | 6/6 | n/a |

Each projection multiplies 0.87 s by the local candidate-to-reference median ratio on a 20M input. The two practice seeds were used during development. Only a hidden-seed official run can establish a leaderboard time.

## Setup and verification

Six paired blocks per input alternated build order. Each run used a fresh offline, read-only container, eight logical CPUs pinned to cores 0–7, a 24 GiB limit with no swap, and warmed input on a Ryzen 9 9950X under WSL2. The primary elapsed time comes from CLOCK_MONOTONIC. Data for CLOCK_MONOTONIC_RAW, container engine timing, peak memory and stage timing are also retained. Both clock-based comparisons below use all observations.

| Input | Reduction on second clock | Reference peak RSS¹ | Candidate peak RSS¹ |
|---|---:|---:|---:|
| 20M events + 5M clicks, seed 42 | 17.0% | 3.17 GiB | 2.78 GiB |
| 20M events + 5M clicks, seed 314159 | 12.9% | 3.12 GiB | 2.73 GiB |
| 60M events + 15M clicks, seed 73 | 20.9% | 9.05 GiB | 7.89 GiB |

¹ Median peak resident set of the executable. It includes mapped input pages.

All 36 organizer fixtures and every field of the public dev answer matched (117,038 user-day rows and 27,400 campaign-day rows). For every timed output, an independent DuckDB reference checked both Parquet tables with EXCEPT ALL in both directions, including row multiplicities.

Candidate image `ac132414793cc84a21b6277a303bf7b74d89ae16197f77e8e7ae4b02ee00bdf2`. Reference image `b3d3d09065bed16aef89ceb12f9ef8afc1781b9f779b6d940eea5946078adb00`. See [all 36 timing samples and verification results](measurements.json).
