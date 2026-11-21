# Fused CSV scanner

The scanner finds the next record, commas and quotes with the same AVX2 loads. The generic CSV path handles longer records and unusual field layouts. Both benchmark builds read the complete input and write both Parquet tables.

## Intel comparison

Six physical Intel Core i5-8500 cores ran GCC 13.3 profile-guided Haswell builds. Each dataset had one discarded warmup, randomized build order on each lap, warm inputs and a fresh output directory. The reference is commit `05a39b3`, which scored 0.84 s in the official 20M-event practice run. The estimate multiplies that score by the ratio of local median times.

| Seed | Events | Measured laps | Reference | Fused | Practice estimate |
|---|---:|---:|---:|---:|---:|
| 42 | 20M | 5 | 0.7508 s | 0.5473 s | 0.612 s |
| 807154 | 20M | 5 | 0.7268 s | 0.5408 s | 0.625 s |
| 1599080644 | 20M | 5 | 0.7342 s | 0.5467 s | 0.625 s |
| 73 | 60M | 4 | 2.2027 s | 1.6128 s |  |

## Eight-thread container

Four fresh container runs per build per dataset used eight pinned Ryzen 9 9950X hardware threads and 24 GiB under WSL2. The workstation was shared with development. Times cover the solver process through exit.

| Seed | Reference | Fused |
|---|---:|---:|
| 42 | 0.4614 s | 0.4130 s |
| 807154 | 0.4844 s | 0.3493 s |
| 1599080644 | 0.4592 s | 0.3336 s |
| 73 | 1.4369 s | 1.0323 s |

The exact release image passed all 36 fixtures and matched all 117,038 user-day and 27,400 campaign-day rows on public dev with zero differing or duplicate keys. Both complete output tables on each benchmark seed matched sorted DuckDB reference rows by count and SHA-256. The scanner regression checks byte boundaries, delimiter masks, and files with and without a trailing newline. [Raw measurements](measurements.json)
