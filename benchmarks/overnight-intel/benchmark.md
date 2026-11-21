# Exact event hashes and identity resolution

On four 20M-event inputs, this build used 6.6% to 8.2% less elapsed time than the 0.64 s submission. On two 60M inputs the savings were 2.3% and 3.7%.

Both executables were built for Haswell and ran on the same six physical Intel Core i5-8500 cores with jemalloc and a warm input cache. Nine measured rounds followed a warm-up on each input, with execution order varied within each round. Times cover process launch, reading every input CSV, writing both Parquet tables, and process exit. The official runner has eight pinned hardware threads.

| Events / clicks | Seed | Official build, local median | Candidate, local median | Paired time saved | Estimated practice |
|---|---:|---:|---:|---:|---:|
| 20M / 5M | 133688819 | 0.471 s | 0.440 s | 6.6% | 0.598 s |
| 20M / 5M | 1765772783 | 0.485 s | 0.445 s | 7.9% | 0.590 s |
| 20M / 5M | 1376142809 | 0.486 s | 0.449 s | 7.6% | 0.591 s |
| 20M / 5M | 807154 | 0.490 s | 0.451 s | 8.2% | 0.587 s |
| 60M / 15M | 73 | 1.532 s | 1.460 s | 3.7% | n/a |
| 60M / 15M | 223013281 | 1.558 s | 1.522 s | 2.3% | n/a |

The practice estimate multiplies 0.64 s by the median within-round candidate/control ratio for that seed. Only a run through the organizer can establish an official score. The two new practice seeds were first generated after selecting the candidate. [Raw process measurements](measurements.json)

## Correctness

The release container ran as UID 1000 with eight pinned hardware threads, a read-only root and input, networking disabled, 24 GB of memory and no swap. The starter reader (DuckDB 1.5.6) matched all 36 fixtures and both full public dev tables (117,038 user rows and 27,400 campaign rows). It also matched both sorted typed table hashes on four 20M inputs (42, 1150123013, 133688819, 1765772783) and one 60M input (223013281). For the two new seeds a separate SQL implementation computed the expected tables from raw CSVs. The seven repository C++ test programs passed, including the final unterminated row and newline-terminated row at the scanner width boundary.
