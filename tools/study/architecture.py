"""Declared single/dual study: retain all runs, audit wires, then make tables/figures."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_rtl_run import audit, require, rows
from analyze import write_csv
from plan import build_architecture_plan, yaml_text


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def run_experiments(output, trace, rtl):
    plan = build_architecture_plan()
    plan["declared_at_utc"] = datetime.now(timezone.utc).isoformat()
    source = Path(__file__).resolve().parents[2]
    inputs = [source / "CMakeLists.txt"]
    for directory in ("src", "include", "rtl", "tests", "tools"):
        inputs.extend(p for p in (source / directory).rglob("*") if p.is_file() and
                      p.suffix in (".cpp", ".hpp", ".sv", ".v", ".py", ".txt"))
    plan["source_sha256"] = {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)}
    plan["executables"] = {label: dict(path=str(p), sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                           for label, p in (("trace", trace), ("rtl", rtl))}
    save(output / "plan.json", plan)  # Record the study configuration and provenance.
    runs = []
    for case in plan["cases"]:
        folder = output / case["name"]
        folder.mkdir()
        (folder / "workload.yaml").write_text(yaml_text(case["workload"]), encoding="utf-8")
        settings = dict(plan["rtl"], trace_waveform=int(case["name"] == plan["waveform_case"]))
        (folder / "rtl.yaml").write_text(yaml_text(settings), encoding="utf-8")
        for backend in ("trace", *plan["architectures"]):
            command = [str(trace if backend == "trace" else rtl), "--config", str(folder / "workload.yaml"),
                       "--output", str(folder / backend)]
            if backend != "trace":
                command += ["--rtl-config", str(folder / "rtl.yaml"), "--rtl", backend]
            entry = dict(case=case["name"], backend=backend, command=command, status="running")
            runs.append(entry)
            save(output / "runs.json", runs)
            try:
                proc = subprocess.run(command, text=True, capture_output=True, timeout=60)
                entry.update(exit=proc.returncode, stdout=proc.stdout, stderr=proc.stderr)
                require(proc.returncode == 0, "Executable failed")
                entry["status"] = "passed"
            except Exception as exc:
                entry.update(status="failed", error=str(exc), detail=traceback.format_exc())
            save(output / "runs.json", runs)
        print(case["name"], flush=True)


def analyze(source):
    plan = json.loads((source / "plan.json").read_text())
    runs = json.loads((source / "runs.json").read_text())
    require([(r["case"], r["backend"]) for r in runs] ==
            [(c["name"], b) for c in plan["cases"] for b in ("trace", *plan["architectures"])], "Missing/reordered runs")
    require(all(r["status"] == "passed" and r["exit"] == 0 for r in runs), "Failed/partial cases: refusing success-only aggregation")
    metrics, comparisons = [], []
    for c in plan["cases"]:
        folder = source / c["name"]
        trace = rows(folder / "trace" / "summary.csv")[0]
        for backend in ("trace", *plan["architectures"]):
            require((folder / backend / "input.yaml").read_text(encoding="utf-8") == yaml_text(c["workload"]), "Executed input differs from declared plan")
        pair = []
        for architecture in plan["architectures"]:
            directory = folder / architecture
            checked = audit(directory, folder / "trace")
            s = rows(directory / "summary.csv")[0]
            settings = dict(line.split(":", 1) for line in (directory / "resolved_rtl_config.yaml").read_text().splitlines())
            require(settings["architecture"].strip() == architecture, "Architecture selection did not reach adapter")
            for k, v in plan["rtl"].items():
                require(int(settings[k]) == v, "Declared RTL settings changed")
            # Scheduler state must remain identical; backend time/pressure may differ.
            for k in ("serving_steps", "completed_transactions", "completed_read_bytes", "completed_write_bytes",
                      "peak_allocated_bytes", "peak_reserved_bytes", "peak_valid_kv_bytes", "outputs", "computed_tokens",
                      "admitted_requests", "completed_requests", "peak_active_requests"):
                require(s[k] == trace[k], f"Trace/RTL state mismatch: {k}")
            require(int(s["completed_read_bytes"]) == c["expected_read_bytes"] and
                    int(s["completed_write_bytes"]) == c["expected_write_bytes"], "Independent byte oracle mismatch")
            waits = Counter(r["reason"] for r in rows(directory / "driver_waits.csv"))
            n = checked["observed_cycles"]
            busy = n - checked["idle_cycles"]
            row = dict(case=c["name"], group=c["group"], concurrency=c["concurrency"], architecture=architecture,
                       max_outstanding=c["workload"]["driver"]["max_outstanding"], peak_active_requests=int(s["peak_active_requests"]),
                       read_bytes=int(s["completed_read_bytes"]), write_bytes=int(s["completed_write_bytes"]),
                       serving_steps=int(s["serving_steps"]), **{k: v for k, v in checked.items() if isinstance(v, int) and not isinstance(v, bool)},
                       overlap_ratio=checked["overlap_cycles"] / busy if busy else 0,
                       read_utilization=(checked["read_only_cycles"] + checked["overlap_cycles"]) / n,
                       write_utilization=(checked["write_only_cycles"] + checked["overlap_cycles"]) / n,
                       mean_read_queue=checked["read_queue_sum"] / n, mean_write_queue=checked["write_queue_sum"] / n,
                       driver_staging_wait_cycles=waits["backend_staging"], driver_outstanding_wait_cycles=waits["outstanding_limit"])
            metrics.append(row); pair.append(row)
        single, dual = pair
        control = c["concurrency"] == 1 or single["max_outstanding"] == 1
        require(single["overlap_cycles"] == 0, "Single engine overlap")
        if control:
            require(dual["overlap_cycles"] == 0, "Dependent/serial control overlapped")
        comparisons.append(dict(case=c["name"], group=c["group"], concurrency=c["concurrency"], max_outstanding=single["max_outstanding"],
                                single_span=single["completion_span_cycles"], dual_span=dual["completion_span_cycles"],
                                span_ratio=single["completion_span_cycles"] / dual["completion_span_cycles"],
                                dual_overlap_ratio=dual["overlap_ratio"], dual_overlap_cycles=dual["overlap_cycles"],
                                transactions=single["transactions"]))
    return metrics, comparisons, plan


def plot(metrics, comparisons, source, output):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False,
                         "svg.hashsalt": "tiny-vllm-architecture"})
    colors = {"single": "#53657b", "dual-rw": "#087e8b"}

    def finish(fig, name):
        fig.savefig(output / (name + ".png"), dpi=180)
        fig.savefig(output / (name + ".svg"), metadata={"Date": None})
        plt.close(fig)

    fig, axes = plt.subplots(1, 3, figsize=(13, 4.3), layout="constrained")
    for ax, group in zip(axes[:2], ("homogeneous", "mixed")):
        for arch in colors:
            subset = [r for r in metrics if r["group"] == group and r["architecture"] == arch and r["max_outstanding"] == 16]
            ax.plot([r["concurrency"] for r in subset], [r["completion_span_cycles"] for r in subset], "o-", color=colors[arch], label=arch)
        ax.set(title=group.capitalize(), xlabel="Active-request limit", ylabel="Completion span (RTL cycles)", xticks=[1, 2, 4, 8])
        ax.grid(alpha=.2); ax.legend()
    labels, ratios = [], []
    for r in comparisons:
        if r["group"] == "single-request" or r["max_outstanding"] == 1:
            labels.append("1 request" if r["group"] == "single-request" else r["group"] + "\noutstanding=1")
            ratios.append(r["span_ratio"])
    axes[2].bar(labels, ratios, color="#bfcbd2")
    axes[2].axhline(1, color="#53657b", ls="--")
    axes[2].set(title="Declared controls", ylabel="Single span / dual span", ylim=(0, 1.25))
    for i, v in enumerate(ratios): axes[2].text(i, v + .025, f"{v:.3f}×", ha="center")
    fig.suptitle("Same workload and total waiting slots; dual adds a second engine")
    finish(fig, "architecture-span")

    fig, axes = plt.subplots(1, 2, figsize=(10, 4), layout="constrained")
    for group, color in (("homogeneous", "#087e8b"), ("mixed", "#c16635")):
        subset = [r for r in comparisons if r["group"] == group and r["max_outstanding"] == 16]
        for ax, field in zip(axes, ("span_ratio", "dual_overlap_ratio")):
            ax.plot([r["concurrency"] for r in subset], [r[field] for r in subset], "o-", color=color, label=group)
            ax.set(xlabel="Active-request limit", xticks=[1, 2, 4, 8]); ax.grid(alpha=.2); ax.legend()
    axes[0].axhline(1, color="grey", ls="--"); axes[0].set(ylabel="Single span / dual span", title="Architecture response")
    axes[1].set(ylabel="Both-busy cycles / any-busy cycles", title="Dual engine overlap", ylim=(0, 1))
    finish(fig, "architecture-overlap")

    selected = [r for r in metrics if r["group"] == "mixed" and r["max_outstanding"] == 16]
    labels = [f"C{r['concurrency']}\n{r['architecture']}" for r in selected]
    fig, axes = plt.subplots(2, 1, figsize=(11, 7), layout="constrained")
    bottom = [0] * len(selected)
    for key, color in (("idle", "#e2e6e9"), ("read_only", "#087e8b"), ("write_only", "#c16635"), ("overlap", "#7662aa")):
        values = [r[key + "_cycles"] for r in selected]
        axes[0].bar(labels, values, bottom=bottom, label=key.replace("_", " "), color=color)
        bottom = [a + b for a, b in zip(bottom, values)]
    axes[0].set(ylabel="Post-reset cycles", title="Mixed workload: mutually exclusive engine states")
    axes[0].set_ylim(0, max(bottom) * 1.2)
    axes[0].legend(ncols=4, loc="upper right")
    for i, (field, label, color) in enumerate((("request_stall_cycles", "RTL request stall", "#53657b"),
                                             ("driver_staging_wait_cycles", "Driver staging wait", "#c16635"),
                                             ("driver_outstanding_wait_cycles", "Driver outstanding wait", "#087e8b"))):
        axes[1].bar([x + (i - 1) * .24 for x in range(len(selected))], [r[field] for r in selected], width=.24, label=label, color=color)
    axes[1].set(xticks=list(range(len(labels))), xticklabels=labels, ylabel="Wait cycles", title="Wait categories can overlap; do not add them to explain span")
    axes[1].legend(ncols=3)
    finish(fig, "architecture-states")

    case = "mixed-c4"
    wire = rows(source / case / "dual-rw" / "rtl_cycles.csv")
    first = next((int(r["edge"]) for r in wire if r["read_busy"] == r["write_busy"] == "1"), 16)
    begin, end = max(1, first - 15), first + 75
    fig, axes = plt.subplots(2, 1, figsize=(12, 4.8), sharex=True, layout="constrained")
    for ax, arch in zip(axes, ("single", "dual-rw")):
        directory = source / case / arch
        cycles = rows(directory / "rtl_cycles.csv")
        tx = {int(r["transaction_id"]): r for r in rows(directory / "transactions.csv") if r["event"] == "completed"}
        completion = {int(r["cmp_id"]): int(r["edge"]) for r in cycles if r["cmp_valid"] == "1"}
        for r in cycles:
            start = int(r["edge"])
            for lane, label, color in ((0, "read", "#087e8b"), (1, "write", "#c16635")):
                if r[label + "_service_start"] != "1": continue
                tid = int(r[label + "_service_id"]); stop = completion[tid]
                if stop <= begin or start >= end: continue
                left, right = max(start, begin), min(stop, end)
                ax.broken_barh([(left, right - left)], (lane - .28, .56), facecolors=color, edgecolors="white")
                if right - left >= 6:
                    ax.text((left + right) / 2, lane, tx[tid]["request_id"], ha="center", va="center", color="white", fontsize=8)
        ax.set(yticks=[0, 1], yticklabels=["Read", "Write"], ylim=(-.65, 1.65), title=arch, xlim=(begin, end))
        ax.grid(axis="x", alpha=.2)
    axes[1].set_xlabel("Post-reset edge (service-start to completion; labels are request owners)")
    fig.suptitle("Actual RTL activity, mixed C4: window anchored at first dual overlap")
    finish(fig, "architecture-waveform")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--trace", type=Path)
    p.add_argument("--rtl", type=Path)
    p.add_argument("--input", type=Path, help="Re-audit saved runs without executing workloads")
    p.add_argument("--output", type=Path, required=True, help="New directory; never overwrite")
    args = p.parse_args()
    if not args.input and (not args.trace or not args.rtl): p.error("Supply --trace and --rtl, or --input")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = args.input.resolve() if args.input else output
    try:
        if not args.input: run_experiments(output, args.trace.resolve(), args.rtl.resolve())
        metrics, comparisons, plan = analyze(source)
        write_csv(output / "metrics.csv", metrics)
        write_csv(output / "comparisons.csv", comparisons)
        plot(metrics, comparisons, source, output)
        save(output / "checks.json", dict(status="passed", pairs=len(comparisons), rtl_runs=len(metrics),
                                         waveform_case=plan["waveform_case"], source=str(source)))
    except Exception as exc:
        save(output / "checks.json", dict(status="failed", error=str(exc), detail=traceback.format_exc()))
        raise
    print(f"Passed: {len(comparisons)} complete pairs. Tables and figures: {output}")


if __name__ == "__main__":
    main()
