# Shopping Sessions benchmark

The [current benchmark](benchmarks/sub70/benchmark.md) compares this candidate with our 0.87 s submission.

The [previous release benchmark](benchmarks/subsecond/benchmark.md) compares the submitted 0.87 s build with Jerry Hu's official 1.19 s entry.

The [earlier leaderboard comparison](benchmarks/leaderboard-comparison.md) tests the updated pipeline on two practice seeds, a final-size dataset, and cold inputs at four storage speeds. The measurements below document commit `b78bed7`.

On the generated final-size input, the optimized container finished in **4.68 seconds median** over five warm runs. The executable itself took **4.30 seconds median**. With all input pages evicted from the Linux page cache, one run took **6.18 seconds container time** and **5.72 seconds executable time**. These are local measurements on an AMD Ryzen 9 9950X. The official scorer runs on Intel Xeon hardware and hidden seeds.

## Timings

| Generated input | Warm runs | Initial executable median | Optimized executable median | Optimized container median | Fastest optimized container | Peak optimized process RSS |
|---|---:|---:|---:|---:|---:|---:|
| 20M events + 5M clicks, seed 42 | 3 | 2.224 s | 1.192 s | 1.271 s | 1.257 s | 3.61 GiB |
| 60M events + 15M clicks, seed 73 | 5 | 6.980 s | 4.296 s | 4.683 s | 4.307 s | 10.43 GiB |

The initial correct implementation ran three times at each size. Tuning reduced median executable time by **46.4% on practice** and **38.5% at final size**. The main changes were to assign identity IDs in parallel before union-find, allocate destination buckets in parallel, route valid clicks directly to their person, and keep campaign totals in contiguous daily arrays. The final-size warm container times in execution order were **4.774, 4.593, 4.683, 4.307 and 5.514 seconds**. [Raw observations and comparison results](benchmarks/local.json)

## Correctness

| Input | User-day rows | Campaign-day rows | Verification |
|---|---:|---:|---|
| Official fixtures | — | — | Both tables passed on all 36 cases using the submission image |
| Official dev, seed 1 | 117,038 | 27,400 | Official checker found zero differences and zero duplicate keys |
| Generated practice, seed 42 | 2,246,306 | 28,014 | Every timed output matched the independent reference |
| Generated final, seed 73 | 6,612,082 | 28,014 | Every timed output matched the independent reference |

The independent checker builds the identity graph in Python and computes both tables in DuckDB. It uses an as-of join for last-click credit and merged click-coverage intervals for assists. Comparisons used two-way `EXCEPT ALL`, including duplicate multiplicities and null values. All 15 timed runs of the initial and optimized images passed. A separate AddressSanitizer and UndefinedBehaviorSanitizer build also passed all 36 official fixtures. [Independent checker](tools/check_generated.py)

## Measurement conditions

- Ubuntu 24.04 on WSL2, native ext4 data, eight pinned logical CPUs spanning four physical cores with SMT, a 24 GiB cgroup limit and zero swap. Runs used UID 1000, disabled networking, a read-only root, a 64 MiB temporary filesystem and fresh writable output and scratch directories.
- Each repetition used a fresh container. Input generation, image building, container creation, and correctness comparison were outside the timed executable. CSV reads, process startup and teardown, and Parquet writes were inside. The raw observations also contain the container engine’s start/exit interval and the client command duration.
- An anomalous start/exit interval appeared in an exploratory run. For the reported series, a statically linked observer used `CLOCK_MONOTONIC_RAW` around the executable and recorded peak RSS with `wait4`. The initial and optimized images ran in interleaved order. [Timer source](tools/time_exec.c)
- All 1,368,651 Linux input pages were resident before the warm final-size series. Before the cold trial, targeted file eviction and `mincore` verified that zero pages were resident. Windows and storage-device caches were uncontrolled. Peak cgroup usage on the cold trial was 10.45 GiB. Final-size output and scratch together occupied about 272 MiB.
- The release image is 155,473,314 bytes. Rebuilding the release produced the tested image ID and matching executable and shared-library digests. [Reproduction commands](README.md)
