# Allocator and compiler profile

The solver uses jemalloc and trains GCC on a synthetic workload with a wider range of identifiers, record layouts and user frequencies. Training runs during the image build.

## Intel comparison

Fraun-01 has six physical Intel Core i5-8500 cores. Both solvers were compiled with GCC 13.3 for Haswell and used all six cores. Each dataset had one warmup followed by seven alternating timed runs per build. Every invocation read the input and wrote both output tables to a fresh directory. The reference was the source of the 0.84 s official submission, commit `05a39b3`. Practice estimates multiply 0.84 s by the ratio of local median times.

| Seed | Events | Reference | Candidate | Reduction | Estimated practice score |
|---|---:|---:|---:|---:|---:|
| 42 | 20M | 0.7629 s | 0.5453 s | 28.5% | 0.600 s |
| 807154 | 20M | 0.7415 s | 0.5409 s | 27.1% | 0.613 s |
| 1599080644 | 20M | 0.7337 s | 0.5355 s | 27.0% | 0.613 s |
| 1376142809 | 20M | 0.7233 s | 0.5318 s | 26.5% | 0.618 s |
| 73 | 60M | 2.1988 s | 1.6090 s | 26.8% | — |

## Container and correctness checks

The GCC 12 Cascade Lake container passed 36 fixture pairs. Both directions of the public dev comparison matched, with 117,038 user-day rows and 27,400 campaign-day rows. On all five generated inputs, both complete result tables matched the sorted independent SQL references by row count and SHA-256.

The raw file also contains the eight-thread, 24 GiB Ryzen 9 9950X container timings. Concurrent workstation use produced substantial timing variability, including runs exceeding twice the median. The estimates above use the separate Intel machine. Seed 1376142809 was reserved before its first Intel timing.

[Raw timings and full table hashes](measurements.json)
