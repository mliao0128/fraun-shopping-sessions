#!/usr/bin/env python3
"""Run a fresh Linux container per sample, timing its recorded start and finish."""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile
import time


def timestamp_ns(value):
    # Both Docker (RFC 3339 Nano) and Podman preserve nanosecond fractions.
    match = re.fullmatch(r"(.+?)(?:\.(\d+))?(Z|[+-]\d\d:\d\d)", value)
    if not match:
        raise ValueError(f"Unrecognized container timestamp {value!r}")
    date, fraction, zone = match.groups()
    base = datetime.datetime.fromisoformat(date + zone.replace("Z", "+00:00"))
    return int(base.timestamp()) * 1_000_000_000 + int((fraction or "").ljust(9, "0"))


def run(args, index):
    destination = args.results / f"run-{index}"
    destination.mkdir()
    output = destination / "output"
    output.mkdir()
    output.chmod(0o777)
    with tempfile.TemporaryDirectory(dir=destination, prefix="scratch-") as tmp:
        scratch = Path(tmp).resolve()
        scratch.chmod(0o777)
        command = [args.engine, "create", "--platform", "linux/amd64", "--network", "none",
                   "--read-only", "--tmpfs", "/tmp:rw,size=64m,mode=1777", "--cap-drop", "ALL",
                   "--security-opt", "no-new-privileges", "--pids-limit", "4096", "--cpus", "8",
                   "--cpuset-cpus", args.cpuset, "--memory", "24g", "--memory-swap", "24g",
                   "--user", "1000:1000", "-v", f"{args.data.resolve()}:/data:ro",
                   "-v", f"{output.resolve()}:/output", "-v", f"{scratch}:/scratch"]
        if Path(args.engine).name == "podman":
            command.append("--read-only-tmpfs=false")
        for key, value in {"DATA_DIR": "/data", "OUTPUT_DIR": "/output",
                           "SCRATCH_DIR": "/scratch", "HOME": "/scratch",
                           "NCPU": "8", "MEM_GB": "24"}.items():
            command.extend(["-e", f"{key}={value}"])
        if args.profile:
            command.extend(["-e", "SHOP_PROFILE=1"])
        if args.clock:
            command.extend(["-v", f"{args.clock.resolve()}:/benchmark-clock:ro",
                            "--entrypoint", "/benchmark-clock"])
        command.append(args.image)
        if args.clock:
            command.append("/usr/local/bin/shopping-sessions")
        cid = subprocess.check_output(command, text=True).strip()
        try:
            before = time.perf_counter()
            result = subprocess.run([args.engine, "start", "-a", cid], stderr=subprocess.PIPE, text=True)
            cli_seconds = time.perf_counter() - before
            sys.stderr.write(result.stderr)
            result.check_returncode()
            state = json.loads(subprocess.check_output([args.engine, "inspect", cid]))[0]["State"]
            if state["ExitCode"] != 0:
                raise RuntimeError(f"Container exited with code {state['ExitCode']}")
            elapsed = timestamp_ns(state["FinishedAt"]) - timestamp_ns(state["StartedAt"])
            if elapsed <= 0 and not args.clock:
                raise RuntimeError("Container engine reported an invalid timing interval")
            disk = sum(p.stat().st_size for d in (output, scratch) for p in d.rglob("*") if p.is_file())
            if disk > 50_000_000_000:
                raise RuntimeError("Combined output and scratch exceed 50 GB")
            sample = {"run": index, "seconds": elapsed / 1e9, "oci_seconds": elapsed / 1e9,
                      "cli_start_seconds": cli_seconds, "output_and_scratch_bytes": disk}
            if args.clock:
                line = next(line for line in result.stderr.splitlines() if line.startswith("SHOPPING_TIMER "))
                observation = json.loads(line.removeprefix("SHOPPING_TIMER "))
                sample.update(seconds=observation["process_ns"] / 1e9,
                              peak_rss_kib=observation["peak_rss_kib"])
            return sample
        finally:
            subprocess.run([args.engine, "rm", "-f", cid], check=True, stdout=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image")
    parser.add_argument("data", type=Path)
    parser.add_argument("results", type=Path, help="A new directory for outputs and timings")
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--cpuset", default="0-7")
    parser.add_argument("--clock", type=Path, help="Static Linux tools/time_exec.c executable for monotonic timings")
    parser.add_argument("--profile", action="store_true")
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    args.results.mkdir(parents=True)
    samples = []
    for i in range(1, args.runs + 1):
        sample = run(args, i)
        samples.append(sample)
        print(json.dumps(sample), flush=True)
    report = {"image": args.image, "engine": args.engine, "cpuset": args.cpuset,
              "clock": "process_monotonic_raw" if args.clock else "container_timestamps",
              "memory_gib": 24, "runs": samples,
              "median_seconds": statistics.median(s["seconds"] for s in samples)}
    (args.results / "timings.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Median ({report['clock']}): {report['median_seconds']:.6f} seconds")


if __name__ == "__main__":
    main()
