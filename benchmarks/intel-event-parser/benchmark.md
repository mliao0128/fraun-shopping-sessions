# Intel event parser comparison

Fraun-01, Core i5-8500, six physical cores, GCC 13.3, native Linux AVX2. Both revisions used the same profile-guided Haswell build, six workers, and warm inputs. Elapsed time includes native process launch through exit. An initial three-way screen rotated the order. The head-to-head then alternated order, with one warm-up and six measured pairs per dataset.

| Generated input | 64e6b93 median | Split event parser median | Time saved | Faster pairs | Median parse, baseline / candidate |
|---|---:|---:|---:|---:|---:|
| 20M events + 5M clicks, seed 42 | 0.6916 s | 0.6856 s | 0.86% | 5/6 | 315.7 / 310.8 ms |
| 20M events + 5M clicks, seed 807154 | 0.6599 s | 0.6551 s | 0.74% | 6/6 | 305.7 / 300.5 ms |
| 60M events + 15M clicks, seed 73 | 1.9053 s | 1.8825 s | 1.20% | 5/6 | 923.6 / 919.3 ms |

## Parser diagnostics

Each value is the median of three diagnostic runs of the baseline. Worker CPU is summed over the reader threads, including row scanning, parsing and scatter writes. Timestamp replay used every eighth chunk, with matched traversal controls. Event timestamp measurements include both event and arrival handling per eligible event. CSV and complete event parsing were timed in separate replays from the same sampled lines. The complete parser includes CSV, timestamps, ID packing, and validation. These overlapping and separately measured costs cannot be added.

| Seed | Event / click worker CPU (ms) | Timestamp per event / click (ns) | CSV / complete event parse per row (ns) |
|---|---:|---:|---:|
| 42 | 1370.1 / 278.0 | 12.1 / 10.3 | 19.2 / 38.9 |
| 807154 | 1345.3 / 245.0 | 12.1 / 10.3 | 19.2 / 39.0 |
| 73 | 3994.5 / 1070.1 | 12.1 / 10.2 | 19.2 / 38.7 |

The selected candidate splits common 65-96 byte event rows at byte 32 and extracts the two groups of field delimiters independently. Other layouts retain the existing parser. A second candidate that found three delimiters from each end had no consistent screening advantage.

Both parser variants matched the baseline on an 84,661-row differential corpus compiled for Haswell and Cascade Lake. The selected split parser passed all 36 official fixtures and exactly matched the public dev tables (117,038 user-day and 27,400 campaign-day rows). The final measured output from each build at each dataset matched both independent reference tables by row counts and sorted-row SHA-256.
