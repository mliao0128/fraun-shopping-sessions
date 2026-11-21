# Shopping Sessions release benchmark

Candidate: `fraun-fast-final`, image `b3d3d09065bed16aef89ceb12f9ef8afc1781b9f779b6d940eea5946078adb00`. Reference: Jerry Hu, image `a6bb5c293fa15f184ef4fbae457eda03cbd740c1820a21d76b1f8f5a9ed37ead`. Projections scale his official 1.19 s practice score by the ratio of the local medians.

| Input | Jerry | Candidate | Time saved | Faster pairs | Projected practice |
|---|---:|---:|---:|---:|---:|
| practice | 1.084 s | 0.805 s | 25.7% | 6/6 | 0.884 s |
| followup-holdout | 0.827 s | 0.529 s | 36.0% | 6/6 | 0.761 s |
| final | 2.840 s | 2.021 s | 28.8% | 6/6 | — |

Six fresh-container runs per build per input, in six paired blocks alternating which build ran first. Input files were warmed before each dataset. Elapsed time uses Linux CLOCK_MONOTONIC around executable startup through exit. Each container has eight logical CPUs pinned to 0–7, 24 GiB memory with no swap and networking disabled.

The 20M inputs use seeds 42 and 314159. The 60M input uses seed 73. All 36 official fixtures and the public dev reference matched. Both tables from all 36 timed runs matched independent DuckDB reference outputs, using EXCEPT ALL in both directions including duplicate multiplicities.

The same samples provide the container engine start-to-exit times below. All runs used a Ryzen 9 9950X workstation with WSL2 and warm ext4 inputs. Other jobs were active on the workstation.

| Input | Jerry, container | Release, container | Practice projection |
|---|---:|---:|---:|
| practice | 1.084 s | 0.802 s | 0.881 s |
| followup-holdout | 0.823 s | 0.526 s | 0.760 s |
| final | 2.836 s | 1.888 s | n/a |

## Changes from Jerry's submission

The reference was [Jerry Hu's 1.19-second entry](https://github.com/Jerryh127/fraun-shopping-sessions/commit/e315a0296ecc6f9c05c31d57309dc308f6a549b9).

The release encodes common identifiers inline, retains short event IDs and ingest times in staged rows, and groups potential duplicates by hash before exact comparisons. A fingerprint table skips negative alias probes. Routing packs common events into 16-byte rows with full-width overflow storage. Aggregation writes numeric columns directly, and GCC profiles a synthetic workload during the image build.

The fixture and dev checks also passed on a Haswell-targeted build. Additional differential checks covered 1.44 million key comparisons, timestamp parsing, large inline overflows and duplicate selection.
