#!/usr/bin/env python3
"""
run_benchmark.py - convenience wrapper: runs each of the three example
benchmarks under gem5 (via accel_se_system.py) and prints a short summary
table pulled from each run's m5out/stats.txt.

Usage (from your gem5 checkout root, after building and cross-compiling):

    python3 configs/run_benchmark.py \
        --gem5 build/RISCV/gem5.opt \
        --config configs/accel_se_system.py \
        --workloads-dir workloads

This just shells out to gem5 once per binary (each run gets its own
m5out-<name>/ directory) and greps the stats you likely care about for
this platform: bus traffic, memory bandwidth, and each accelerator's
ops/bytes/cycles counters.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

BENCHMARKS = ["npu_matmul_bench", "lpu_stream_bench", "isp_pipeline_bench"]

# Stat name -> human label. Regexes are matched against m5out/stats.txt
# lines; adjust if your gem5 version names things slightly differently
# (stat names do shift across releases - check stats.txt directly if a
# row prints as "n/a").
STATS_OF_INTEREST = [
    (r"^system\.membus\.pkt_count_system.*\s+(\S+)", "membus total packets"),
    (r"^system\.mem_ctrl\.dram\.bw_total::total\s+(\S+)", "DRAM bw_total"),
    (r"^system\.accel_npu\.opsCompleted\s+(\S+)", "NPU opsCompleted"),
    (r"^system\.accel_npu\.bytesTransferred\s+(\S+)", "NPU bytesTransferred"),
    (r"^system\.accel_npu\.busyCycles\s+(\S+)", "NPU busyCycles"),
    (r"^system\.accel_lpu\.opsCompleted\s+(\S+)", "LPU opsCompleted"),
    (r"^system\.accel_lpu\.bytesTransferred\s+(\S+)", "LPU bytesTransferred"),
    (r"^system\.accel_lpu\.busyCycles\s+(\S+)", "LPU busyCycles"),
    (r"^system\.accel_isp\.opsCompleted\s+(\S+)", "ISP opsCompleted"),
    (r"^system\.accel_isp\.bytesTransferred\s+(\S+)", "ISP bytesTransferred"),
    (r"^system\.accel_isp\.busyCycles\s+(\S+)", "ISP busyCycles"),
    (r"^system\.cpu\.numCycles\s+(\S+)", "CPU numCycles"),
]


def run_one(gem5, config, binary, outdir):
    outdir.mkdir(parents=True, exist_ok=True)
    cmd = [gem5, f"--outdir={outdir}", config, f"--binary={binary}"]
    print(f"$ {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout[-2000:])
        print(result.stderr[-2000:])
        print(f"!! gem5 exited with code {result.returncode} for {binary}")
    return outdir / "stats.txt"


def summarize(stats_path):
    if not stats_path.exists():
        return {}
    text = stats_path.read_text()
    found = {}
    for pattern, label in STATS_OF_INTEREST:
        m = re.search(pattern, text, re.MULTILINE)
        if m:
            found[label] = m.group(1)
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gem5", required=True)
    ap.add_argument("--config", required=True)
    ap.add_argument("--workloads-dir", default="workloads")
    ap.add_argument("--out-root", default="m5out-accel")
    args = ap.parse_args()

    wdir = Path(args.workloads_dir)
    outroot = Path(args.out_root)

    for name in BENCHMARKS:
        binary = wdir / name
        if not binary.exists():
            print(f"skip {name}: not found at {binary} (build it first "
                  f"with workloads/Makefile)")
            continue

        stats_path = run_one(args.gem5, args.config, str(binary),
                              outroot / name)
        summary = summarize(stats_path)

        print(f"\n=== {name} ===")
        if not summary:
            print("  (no matching stats found - check "
                  f"{stats_path} directly)")
        for label, value in summary.items():
            print(f"  {label:28s} {value}")
        print()


if __name__ == "__main__":
    sys.exit(main())
