#!/usr/bin/env python3
"""
plot_results.py - Generate static performance plots from metrics CSV.

This script overwrites output files by design.
"""

import argparse
import csv
import importlib
import math
import os
import site
import sys
from collections import defaultdict


def parse_args():
    parser = argparse.ArgumentParser(description="Generate performance plot from results/metrics.csv")
    parser.add_argument("--csv", required=True, help="Input CSV path (columns: np,run,elapsed_s)")
    parser.add_argument("--png", required=True, help="Output PNG path (overwritten)")
    parser.add_argument("--pdf", required=True, help="Output PDF path (overwritten)")
    return parser.parse_args()


def load_runs(csv_path):
    mpi_groups = defaultdict(list)
    serial_runs = []
    with open(csv_path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                elapsed = float(row["elapsed_s"])
            except (ValueError, KeyError):
                continue

            impl = row.get("impl", "mpi").strip().lower()
            try:
                np_val = int(row["np"])
            except (ValueError, KeyError):
                np_val = 1

            if impl == "serial":
                serial_runs.append(elapsed)
            else:
                mpi_groups[np_val].append(elapsed)
    return mpi_groups, serial_runs


def compute_stats(groups):
    stats = {}
    for np_val, vals in groups.items():
        if not vals:
            continue
        mean = sum(vals) / len(vals)
        var = 0.0
        if len(vals) > 1:
            var = sum((v - mean) ** 2 for v in vals) / (len(vals) - 1)
        stats[np_val] = (mean, math.sqrt(var))
    return stats


def _disable_user_site_packages():
    """
    Remove user site-packages from sys.path to avoid mixed binary stacks
    (e.g., system matplotlib + user-installed numpy ABI mismatch).
    """
    candidates = set()
    try:
        candidates.add(site.getusersitepackages())
    except Exception:
        pass

    home = os.path.expanduser("~")
    candidates.add(os.path.join(home, ".local", "lib"))

    new_path = []
    for path in sys.path:
        lowered = path.lower()
        if any(c and c.lower() in lowered for c in candidates):
            continue
        new_path.append(path)
    sys.path = new_path


def _import_matplotlib():
    """
    Import matplotlib in a robust way by first removing user site-packages.
    This prevents ABI conflicts between user-installed numpy and system matplotlib.
    """
    _disable_user_site_packages()

    # Clear partially imported modules before importing cleanly.
    for name in list(sys.modules.keys()):
        if name == "numpy" or name.startswith("numpy.") or name == "matplotlib" or name.startswith("matplotlib."):
            del sys.modules[name]

    matplotlib = importlib.import_module("matplotlib")
    matplotlib.use("Agg")
    plt = importlib.import_module("matplotlib.pyplot")
    return matplotlib, plt


def main():
    args = parse_args()
    os.makedirs(os.path.dirname(args.png) or ".", exist_ok=True)
    os.makedirs(os.path.dirname(args.pdf) or ".", exist_ok=True)

    mpi_groups, serial_runs = load_runs(args.csv)
    stats = compute_stats(mpi_groups)
    serial_mean = (sum(serial_runs) / len(serial_runs)) if serial_runs else None

    if not stats and serial_mean is None:
        print(f"No valid metrics found in {args.csv}", file=sys.stderr)
        sys.exit(1)

    try:
        _, plt = _import_matplotlib()
    except Exception as exc:
        print(f"matplotlib unavailable; skipping plot generation: {exc}")
        return

    nps = sorted(stats.keys())
    means = [stats[n][0] for n in nps]
    stds = [stats[n][1] for n in nps]

    fig, axes = plt.subplots(1, 2, figsize=(12, 5))

    axes[0].errorbar(nps, means, yerr=stds, marker="o", linewidth=2, capsize=4)
    if serial_mean is not None:
        axes[0].axhline(
            y=serial_mean,
            color="tab:red",
            linestyle="--",
            linewidth=1.8,
            label=f"Serial baseline ({serial_mean:.3f}s)",
        )
        axes[0].legend()
    axes[0].set_title("Time vs MPI Processes")
    axes[0].set_xlabel("MPI Processes (np)")
    axes[0].set_ylabel("Elapsed Time (s)")
    axes[0].grid(True, linestyle="--", alpha=0.4)

    if serial_mean is not None:
        baseline = serial_mean
        speedup = [baseline / m if m > 0 else 0.0 for m in means]
        ideal = [n for n in nps]
        axes[1].plot(nps, speedup, marker="o", linewidth=2, label="Measured")
        axes[1].plot(nps, ideal, linestyle="--", label="Ideal")
        axes[1].legend()
    elif 1 in stats:
        baseline = stats[1][0]
        speedup = [baseline / m if m > 0 else 0.0 for m in means]
        ideal = [n for n in nps]
        axes[1].plot(nps, speedup, marker="o", linewidth=2, label="Measured")
        axes[1].plot(nps, ideal, linestyle="--", label="Ideal")
        axes[1].legend()
    else:
        axes[1].plot(nps, [0 for _ in nps], marker="o", linewidth=2, label="Measured")
        axes[1].legend()

    axes[1].set_title("Speedup vs MPI Processes")
    axes[1].set_xlabel("MPI Processes (np)")
    axes[1].set_ylabel("Speedup")
    axes[1].grid(True, linestyle="--", alpha=0.4)

    fig.tight_layout()
    fig.savefig(args.png, dpi=160)  # overwrites existing file
    fig.savefig(args.pdf)           # overwrites existing file
    plt.close(fig)

    print(f"Saved plot files: {args.png}, {args.pdf}")


if __name__ == "__main__":
    main()
