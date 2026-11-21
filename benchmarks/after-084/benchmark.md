# Comparison with the 0.84 s submission

Reference [05a39b3](https://github.com/mliao0128/fraun-shopping-sessions/commit/05a39b3639b8fbc1cf6c27072c3071d9de7fa81c) scored 0.84 s on the official practice set. These measurements use the archived image of that submission.

| Generated input | Reference | Candidate | Time saved | Faster pairs |
|---|---:|---:|---:|---:|
| 20M events + 5M clicks, seed 42 | 0.5639 s | 0.5224 s | 7.3% | 6/6 |
| 20M events + 5M clicks, seed 314159 | 0.5478 s | 0.5269 s | 3.8% | 4/6 |
| 20M events + 5M clicks, seed 807154 (held out) | 0.7242 s | 0.6888 s | 4.9% | 5/6 |
| 60M events + 15M clicks, seed 73 | 1.6396 s | 1.6279 s | 0.7% | 3/6 |

Each time is the median of six fresh container runs on warm inputs. Within each pair the reference and candidate used the same input and eight logical CPUs pinned to four physical cores on a Ryzen 9 9950X under WSL2. Build order alternated by pair and dataset. Containers had a 24 GiB memory limit, no swap, and networking disabled. The primary timer covers the complete process through exit. The raw records include both monotonic clocks and engine times. Seed 807154 was generated before final selection and used for candidate timing only during confirmation.

| Input seed | Reference aggregation and output | Candidate aggregation and output | Reference peak RSS | Candidate peak RSS |
|---|---:|---:|---:|---:|
| 42 | 0.109 s | 0.093 s | 2.78 GiB | 2.77 GiB |
| 314159 | 0.108 s | 0.100 s | 2.74 GiB | 2.72 GiB |
| 807154 | 0.104 s | 0.096 s | 2.76 GiB | 2.75 GiB |
| 73 | 0.428 s | 0.352 s | 7.89 GiB | 7.85 GiB |

The candidate passed all 36 organizer fixtures and matched the public dev set across 117,038 user-day rows and 27,400 campaign-day rows. All 48 timed outputs matched both independent DuckDB reference tables in both directions, including duplicate multiplicities. Unit checks compared the time-cell result with full timestamp sorting on 12,000 shuffled inputs. Separate end-to-end cases covered session and attribution boundaries and the wide timestamp fallback.

The container engine reported a zero interval for 1 completed runs on this WSL host. Every run was retained in the monotonic-clock comparison.

Reference image `ac132414793cc84a21b6277a303bf7b74d89ae16197f77e8e7ae4b02ee00bdf2`. Candidate image `23d3984b200f70e9325f98ef08d0965a50dad8b24dcde18e9d0912563af009b9`. [Timings and oracle checks](measurements.json)
