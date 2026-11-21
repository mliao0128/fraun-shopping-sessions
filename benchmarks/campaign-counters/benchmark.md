# Campaign counter benchmark

Campaign assists now accumulate in a compact per-day array. A separate record contains session credits, purchasing-session counts, and 64-bit revenue. Each worker carries 32-bit counts forward into 64-bit totals before the next bucket could overflow them.

The control is the Haswell build of Fraun’s official 0.64 s submission, `dd510ea`. The previous candidate is `0acae3b`. Each seed ran on six physical Intel i5-8500 cores in nine measured rounds after a warm-up. The order varied within each round. The native process timer includes startup, writing both Parquet tables, and exit. Practice estimates multiply 0.64 s by the median candidate/control ratio in matched rounds.

| Events / clicks | Generator seed | Official build locally | Previous candidate | New candidate | Paired time saved vs official build | Practice estimate |
|---|---:|---:|---:|---:|---:|---:|
| 20M / 5M | 133688819 | 0.478 s | 0.438 s | 0.425 s | 10.6% | 0.572 s |
| 20M / 5M | 1765772783 | 0.491 s | 0.448 s | 0.432 s | 11.3% | 0.568 s |
| 20M / 5M | 1150123013 | 0.478 s | 0.430 s | 0.413 s | 13.3% | 0.555 s |
| 20M / 5M | 42 | 0.489 s | 0.442 s | 0.429 s | 12.2% | 0.562 s |
| 60M / 15M | 73 | 1.504 s | 1.438 s | 1.374 s | 8.0% | — |
| 60M / 15M | 223013281 | 1.595 s | 1.507 s | 1.487 s | 6.2% | — |

[Raw Intel timings](measurements.json)

## Correctness

The Linux amd64 release image ran as UID 1000, with eight pinned hardware threads, 24 GB of memory, no swap, read-only root and input, and networking disabled. The starter reader using DuckDB 1.5.6 matched all 36 fixture pairs and all public dev rows, including 117,038 user rows and 27,400 campaign rows. Both complete sorted output hashes matched at 20M seeds 42, 1150123013, 133688819 and 1765772783 and 60M seed 223013281. Independent SQL over the raw inputs supplied the answers for seeds 133688819 and 1765772783. All eight C++ executables passed. The new regression covers two 32-bit counter carries, zero-revenue sessions, and an untouched campaign.
