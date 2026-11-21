# Intel branch alignment

The build uses GCC 15 and asks the assembler to keep branches within 32-byte boundaries. The runtime uses one jemalloc arena with transparent huge pages enabled for its allocations. The solver reads the supplied event files and writes both daily Parquet tables on every invocation.

## Paired Intel measurements

An Intel Core i5-8500 ran both programs with six physical cores and warm input files. Each invocation wrote to a fresh output directory. After one warmup, eight alternating runs per build were measured. The reference was the source of the official 0.84 s submission, `05a39b3`. The candidate used the GCC 15 Haswell target supported by this machine. The release container defaults to Cascade Lake.

The practice estimate multiplies 0.84 s by the ratio of median solver process times. The 60M input measures finalist-scale throughput. The fifth practice seed was drawn before this compiler change qualified and was timed first in the confirmation run.

| Seed | Events | Reference | Candidate | Reduction | Practice estimate |
|---|---:|---:|---:|---:|---:|
| 42 | 20M | 0.7585 s | 0.5002 s | 34.0% | 0.554 s |
| 807154 | 20M | 0.7526 s | 0.4986 s | 33.7% | 0.557 s |
| 1599080644 | 20M | 0.7244 s | 0.4860 s | 32.9% | 0.564 s |
| 1376142809 | 20M | 0.7209 s | 0.4868 s | 32.5% | 0.567 s |
| 1896044346 | 20M | 0.7074 s | 0.4856 s | 31.4% | 0.577 s |
| 73 | 60M | 2.1415 s | 1.4539 s | 32.1% | N/A |

## Release checks

The Cascade Lake release container passed all 36 fixtures and matched all 117,038 user-day rows and 27,400 campaign-day rows on the public dev set, with zero differing or duplicate keys. Each generated input matched the independent DuckDB answer in both complete output tables after schema checks, sorting and SHA-256 comparison. The fifth seed also passed a full bidirectional SQL comparison.

All seven targeted C++ checks passed under the GCC 15 Cascade Lake settings. They cover CSV parsing, mixed quoting, binary keys, deduplication locators, session boundaries, the fused scanner and dimension dictionaries.

[Raw timings and complete output hashes](measurements.json)
