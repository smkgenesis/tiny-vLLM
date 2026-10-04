"""Render static research figures from saved analysis tables (no simulations)."""
import argparse
import csv
import json
from pathlib import Path
import platform

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

BLUE = "#2563a6"
ORANGE = "#d86b27"
TEAL = "#238776"


def load(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def decorate(ax, title, xlabel, ylabel):
    ax.set_title(title, loc="left", fontweight="bold", pad=12)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_ylim(bottom=0)
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", alpha=.18)
    ax.set_axisbelow(True)


def grouped(ax, data, fields, labels, colors, value_fmt=".0f"):
    x = np.arange(len(data))
    width = .7 / len(fields)
    for i, (field, label, color) in enumerate(zip(fields, labels, colors)):
        values = [float(r[field]) for r in data]
        bars = ax.bar(x + (i - (len(fields) - 1) / 2) * width, values, width, label=label, color=color)
        ax.bar_label(bars, labels=[format(v, value_fmt) for v in values], padding=3, fontsize=9)
    ax.set_xticks(x, [str(r["value"]) for r in data])
    ax.margins(y=.22)
    if len(fields) > 1:
        ax.legend(frameon=False, loc="upper left", bbox_to_anchor=(0, 1.04), ncols=len(fields), fontsize=8)


def save(fig, directory, name):
    fig.savefig(directory / f"{name}.png", dpi=170)
    fig.savefig(directory / f"{name}.svg")
    plt.close(fig)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("analysis", type=Path)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    report = json.loads((args.analysis / "analysis.json").read_text(encoding="utf-8"))
    if report["status"] != "passed":
        p.error("Analysis is failed/incomplete; refusing success-only figures")
    data, steps = load(args.analysis / "metrics.csv"), load(args.analysis / "steps.csv")
    args.output.mkdir(parents=True, exist_ok=False)
    plt.rcParams.update({"font.size": 10, "axes.titlesize": 11, "figure.facecolor": "white", "svg.fonttype": "none"})
    groups = {name: [r for r in data if r["group"] == name] for name in ("prompt", "output", "active", "chunk", "block", "service", "fifo")}
    fig, axes = plt.subplots(2, 3, figsize=(13.5, 8.2), layout="constrained")
    fig.suptitle("tiny-vLLM | Workload effects under the declared access policy", fontsize=16, fontweight="bold")
    configs = [
        ("prompt", ["write_bytes", "read_bytes"], ["Write", "Read"], [BLUE, ORANGE], "Prompt length (O=3)", "Prompt tokens", "Logical bytes"),
        ("output", ["write_bytes", "read_bytes"], ["Write", "Read"], [BLUE, ORANGE], "Output length (P=5)", "Output tokens", "Logical bytes"),
        ("active", ["peak_allocated_bytes", "peak_reserved_bytes"], ["Allocated", "Reserved"], [BLUE, TEAL], "Concurrency (same requests)", "Maximum active requests", "Peak bytes"),
        ("chunk", ["transactions"], ["Transactions"], [BLUE], "Chunk splitting (budget=3)", "Prefill chunk cap (tokens)", "Completed transactions"),
        ("chunk", ["max_prefill_step_bytes"], ["Prefill"], [TEAL], "Prefill concentration", "Prefill chunk cap (tokens)", "Maximum prefill bytes / logical step"),
        ("block", ["peak_allocated_bytes", "peak_valid_kv_bytes"], ["Allocated", "Valid KV"], [BLUE, TEAL], "Block rounding (128 B pool)", "Tokens per block (P=5, O=5)", "Peak bytes"),
    ]
    for ax, (group, fields, labels, colors, title, xlabel, ylabel) in zip(axes.flat, configs):
        grouped(ax, groups[group], fields, labels, colors)
        decorate(ax, title, xlabel, ylabel)
    fig.supxlabel("Each panel varies one axis within its group. Logical bytes are synthetic KV accesses, not GPU traffic.", fontsize=10)
    save(fig, args.output, "workload")

    fig, axes = plt.subplots(2, 2, figsize=(11.5, 8), layout="constrained")
    fig.suptitle("tiny-vLLM | Timing of the example FIFO and serial service RTL", fontsize=15, fontweight="bold")
    grouped(axes[0, 0], groups["service"], ["completion_span_cycles"], ["Observed"], [BLUE])
    predictions = [18 * (int(r["value"]) + 2) - 1 for r in groups["service"]]
    axes[0, 0].scatter(np.arange(3), predictions, marker="x", s=65, color="#20242c", label="Declared prediction", zorder=3)
    axes[0, 0].legend(frameon=False, fontsize=9, loc="upper left")
    decorate(axes[0, 0], "Service delay | small, outstanding=1", "Service cycles", "First request to last completion (cycles)")
    grouped(axes[0, 1], groups["fifo"], ["completion_span_cycles"], ["Span"], [BLUE])
    decorate(axes[0, 1], "FIFO depth | mixed, service=7, outstanding=8", "FIFO depth (requests)", "First request to last completion (cycles)")
    grouped(axes[1, 0], groups["fifo"], ["request_stall_cycles", "driver_staging_wait_cycles"], ["RTL request stall", "Driver staging wait"], [ORANGE, BLUE])
    decorate(axes[1, 0], "Waiting counts (may overlap; do not add)", "FIFO depth (requests)", "Cycles")
    grouped(axes[1, 1], groups["fifo"], ["mean_request_latency_cycles"], ["Latency"], [TEAL], ".2f")
    decorate(axes[1, 1], "Queue waiting after RTL acceptance", "FIFO depth (requests)", "Mean request-to-completion latency (cycles)")
    fig.supxlabel("FIFO comparisons use the same 26 transactions. Staging is excluded from request latency. No LLM latency claim.", fontsize=10)
    save(fig, args.output, "rtl")

    fig, axes = plt.subplots(1, 3, figsize=(13.5, 4.5), layout="constrained", sharey=True)
    fig.suptitle("tiny-vLLM | Chunk size redistributes work across logical serving steps", fontsize=15, fontweight="bold")
    for ax, chunk in zip(axes, (1, 2, 3)):
        current = [r for r in steps if r["case"] == f"chunk-{chunk}"]
        x = [int(r["step"]) for r in current]
        write = np.array([int(r["write_bytes"]) for r in current])
        read = np.array([int(r["read_bytes"]) for r in current])
        ax.bar(x, write, label="Write", color=BLUE)
        ax.bar(x, read, bottom=write, label="Read", color=ORANGE)
        ax.set_xticks(x)
        decorate(ax, f"Chunk cap {chunk} | total W=56 B, R=104 B", "Logical serving step (not elapsed time)", "Logical KV bytes / step")
    axes[0].legend(frameon=False, loc="upper left")
    fig.supxlabel("P=5, O=3, token budget=3. Decode bytes dominate the overall maximum; smaller chunks only spread prefill.", fontsize=10)
    save(fig, args.output, "steps")
    (args.output / "plot-environment.json").write_text(json.dumps(dict(python=platform.python_version(), matplotlib=matplotlib.__version__, numpy=np.__version__), indent=2), encoding="utf-8")
    print(f"Saved workload, rtl, steps as PNG and SVG: {args.output}")


if __name__ == "__main__":
    main()
