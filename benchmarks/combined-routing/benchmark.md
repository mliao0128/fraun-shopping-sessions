# Combined routing and session benchmark

The candidate partitions large inputs into up to 1,024 groups, retains the high bits of common person hashes in event and click records, sorts small sessions with an AVX2 network, and builds the Parquet user dictionary as names are emitted.

The control is the Haswell build of our official 0.64 s submission, `dd510ea`. Each seed ran on six physical Intel i5-8500 cores in nine measured rounds after a warm-up. Within each round the solver variants ran in randomized order against the same warm input. The interval covers native process launch through exit, including both output filesets. The practice estimate applies the within-round median elapsed-time ratio to 0.64 s.

| Generated events / clicks | Seed | Control | Candidate | Paired time saved | Estimated practice |
|---|---:|---:|---:|---:|---:|
| 20M / 5M | 133688819 | 0.476 s | 0.437 s | 8.2% | 0.587 s |
| 20M / 5M | 1765772783 | 0.494 s | 0.445 s | 9.8% | 0.577 s |
| 20M / 5M | 1150123013 | 0.479 s | 0.438 s | 8.4% | 0.586 s |
| 20M / 5M | 42 | 0.496 s | 0.454 s | 9.2% | 0.581 s |
| 60M / 15M | 73 | 1.578 s | 1.467 s | 7.2% | n/a |
| 60M / 15M | 223013281 | 1.561 s | 1.514 s | 3.0% | n/a |

[Raw Intel timings](measurements.json)

## Verification

The release image ran as UID 1000 with the root and input read-only, eight pinned hardware threads, 24 GB of memory, no swap and no network. DuckDB 1.5.6 matched all 36 starter fixture pairs and the complete 1M dev answer (117,038 user rows and 27,400 campaign rows). Both sorted typed output hashes matched at 20M seeds 42, 1150123013, 133688819 and 1765772783 and at 60M seed 223013281. The last two 20M references were independently computed from raw CSVs in SQL. All seven C++ tests passed, including an independent check of the small-session sort and hash retention on timestamp and purchase overflow paths. A separate reader test recovered all 351,069 ordered user IDs across Parquet page boundaries and the long-name fallback.
