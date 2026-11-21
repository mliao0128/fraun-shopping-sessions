# Parser and timestamp benchmark

Compared with the [0.87 s official practice entry](https://github.com/mliao0128/fraun-shopping-sessions/commit/53f036fc19dd1a206824b29ca2711dd0308fe1d8). Both binaries ran in fresh offline containers on a Ryzen 9 9950X with WSL2, eight pinned logical CPUs, 24 GiB RAM and no container swap. Inputs were warm. Each dataset had six paired runs, alternating which build ran first. The primary measurement is container start to exit. Other work on the host can affect individual timings.

| Generated input | Baseline median | Candidate median | Time saved | Candidate won |
|---|---:|---:|---:|---:|
| 20M events, seed 42 | 0.5691 s | 0.5705 s | -0.3% | 3/6 |
| 20M events, seed 314159 | 0.5425 s | 0.5658 s | -4.3% | 3/6 |
| 60M events, seed 73 | 1.7693 s | 1.5884 s | 10.2% | 4/6 |

## Stage timings

Medians in seconds. The parse stage includes reading event and click CSVs, field validation, timestamp conversion and staging rows. Identity linking runs concurrently. The wait column measures how long routing waited for it after deduplication.

| Input | Build | Parse | Wait for links | Route | Whole executable |
|---|---|---:|---:|---:|---:|
| 20M events, seed 42 | Submitted | 0.3111 | 0.0001 | 0.0502 | 0.5724 |
| 20M events, seed 42 | Candidate | 0.3029 | 0.0001 | 0.0484 | 0.5734 |
| 20M events, seed 314159 | Submitted | 0.3487 | 0.0001 | 0.0501 | 0.5826 |
| 20M events, seed 314159 | Candidate | 0.3257 | 0.0001 | 0.0446 | 0.5688 |
| 60M events, seed 73 | Submitted | 1.0115 | 0.0001 | 0.1388 | 1.7729 |
| 60M events, seed 73 | Candidate | 0.9084 | 0.0001 | 0.1333 | 1.5905 |

## Correctness

All 36 official fixtures matched. On the public development set, both the 117,038 user rows and the 27,400 campaign rows matched exactly. Both Parquet tables from all 36 timed runs matched independent DuckDB reference results in both directions, including multiplicity.

The row scanner was compared with the generic parser on 256,000 generated records at five chunk sizes on AVX2 and AVX-512. Timestamp tests covered 930,982 values and 300,000 valid arrival times on each target, including date validity, UTC offsets, exact lateness boundaries and duplicate ordering.

[Per-run timings and oracle checks](measurements.json)
