"""Recheck saved raw runs, then derive tables. Never run an executable or hide failures."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_rtl_run import audit, require, rows


def write_csv(path, data):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=data[0].keys())
        writer.writeheader()
        writer.writerows(data)


def analyze(source):
    source = Path(source)
    plan = json.loads((source / "plan.json").read_text(encoding="utf-8"))
    runs = json.loads((source / "runs.json").read_text(encoding="utf-8"))
    cases = plan["cases"]
    require(len({c["name"] for c in cases}) == len(cases), "Duplicate case name")
    require([r["name"] for r in runs] == [c["name"] for c in cases], "Missing or reordered experiment results")
    require(all(r["status"] == "passed" for r in runs), "Failed/incomplete cases remain; no success-only aggregation")
    metrics, steps, request_metrics, details = [], [], [], {}
    identical = defaultdict(list)
    for case in cases:
        folder = source / case["name"]
        checked = audit(folder / "rtl", folder / "trace")
        s = rows(folder / "rtl" / "summary.csv")[0]
        ts = rows(folder / "trace" / "summary.csv")[0]
        # Backend timing is excluded, but all scheduler state/occupancy totals must agree.
        equal_fields = ("serving_steps", "completed_transactions", "completed_write_bytes", "completed_read_bytes",
                        "peak_allocated_bytes", "peak_reserved_bytes", "peak_valid_kv_bytes", "outputs", "computed_tokens",
                        "admitted_requests", "completed_requests", "admission_step", "completion_step", "peak_active_requests")
        require(all(s[k] == ts[k] for k in equal_fields), f"Trace/RTL workload state differs: {case['name']}")
        require(int(s["completed_write_bytes"]) == case["expected_write_bytes"] and
                int(s["completed_read_bytes"]) == case["expected_read_bytes"], "Logical byte prediction differs")
        events = rows(folder / "rtl" / "events.csv")
        tx = [r for r in rows(folder / "rtl" / "transactions.csv") if r["event"] == "completed"]
        waits = Counter(r["reason"] for r in rows(folder / "rtl" / "driver_waits.csv"))
        wires = rows(folder / "rtl" / "rtl_cycles.csv")
        starts = [int(r["edge"]) for r in wires if r["service_start"] == "1"]
        expected_rtl = case["rtl"]
        # Check that ready reflects the requested runtime capacity on actual edges.
        for r in wires:
            ready = int(r["edge"]) > expected_rtl["initial_request_pause_cycles"] and int(r["queue_count"]) < expected_rtl["fifo_depth"]
            require(int(r["req_ready"]) == int(ready), "Declared FIFO/gate setting not reflected at RTL pins")
        for req in case["workload"]["requests"]:
            admitted = [r for r in events if r["event"] == "admitted" and r["request_id"] == req["id"]]
            finished = [r for r in events if r["event"] == "finished" and r["request_id"] == req["id"]]
            require(len(admitted) == len(finished) == 1, "Request admission/completion count mismatch")
            request_metrics.append(dict(case=case["name"], request_id=req["id"], arrival_step=req["arrival_step"],
                admission_step=int(admitted[0]["step"]), completion_step=int(finished[0]["step"]),
                admission_delay_steps=int(admitted[0]["step"]) - req["arrival_step"]))
        by_step = defaultdict(list)
        for t in tx:
            by_step[int(t["step"])].append(t)
        max_step_bytes = max_prefill_bytes = 0
        for step, transactions in sorted(by_step.items()):
            read = sum(int(t["bytes"]) for t in transactions if t["write"] == "0")
            write = sum(int(t["bytes"]) for t in transactions if t["write"] == "1")
            prefill = sum(int(t["bytes"]) for t in transactions if t["phase"] == "prefill")
            max_step_bytes = max(max_step_bytes, read + write)
            max_prefill_bytes = max(max_prefill_bytes, prefill)
            scheduled = [r for r in events if r["event"] == "scheduled" and int(r["step"]) == step]
            steps.append(dict(case=case["name"], group=case["group"], step=step, read_bytes=read, write_bytes=write,
                              total_bytes=read + write, prefill_bytes=prefill, transactions=len(transactions),
                              scheduled_tokens=sum(int(r["token_end"]) - int(r["token_begin"]) for r in scheduled)))
        c = case["workload"]
        m = c["model"]
        token_bytes = 2 * m["num_layers"] * m["kv_heads"] * m["head_dim"] * m["dtype_bytes"]
        row = dict(case=case["name"], group=case["group"], value=case["value"], status="passed",
                   read_bytes=int(s["completed_read_bytes"]), write_bytes=int(s["completed_write_bytes"]),
                   transactions=int(s["completed_transactions"]), serving_steps=int(s["serving_steps"]),
                   peak_allocated_bytes=int(s["peak_allocated_bytes"]), peak_reserved_bytes=int(s["peak_reserved_bytes"]),
                   peak_valid_kv_bytes=int(s["peak_valid_kv_bytes"]), max_step_bytes=max_step_bytes,
                   max_prefill_step_bytes=max_prefill_bytes,
                   pool_capacity_bytes=token_bytes * c["kv_cache"]["block_tokens"] * c["kv_cache"]["pool_blocks"],
                   peak_active_requests=int(s["peak_active_requests"]), peak_outstanding=int(s["peak_outstanding"]),
                   fifo_depth=expected_rtl["fifo_depth"], service_cycles=expected_rtl["service_cycles"],
                   max_outstanding=c["driver"]["max_outstanding"], completion_span_cycles=checked["completion_span_cycles"],
                   request_stall_cycles=checked["request_stall_cycles"], peak_queue_count=checked["peak_queue_count"],
                   mean_request_latency_cycles=checked["latency_sum_cycles"] / checked["transactions"],
                   max_request_latency_cycles=checked["latency_max_cycles"],
                   driver_outstanding_wait_cycles=waits["outstanding_limit"], driver_staging_wait_cycles=waits["backend_staging"])
        metrics.append(row)
        details[case["name"]] = dict(prediction=case["prediction"], service_start_edges=starts)
        identical[json.dumps((c, expected_rtl), sort_keys=True)].append(case["name"])

    by_name = {r["case"]: r for r in metrics}
    duplicates = [names for names in identical.values() if len(names) > 1]
    for names in duplicates:
        data_fields = [k for k in by_name[names[0]] if k not in ("case", "group", "value")]
        require(all(all(by_name[n][k] == by_name[names[0]][k] for k in data_fields) for n in names), "Identical configurations changed deterministic metrics")
    # Explicit predictions are reported alongside the observations, not used to
    # delete inconvenient runs. Violated hard expectations make analysis fail.
    exact_checks = []
    for block, expected in ((2, 80), (4, 96), (8, 128)):
        row = by_name[f"block-{block}"]
        exact_checks.append(row["peak_allocated_bytes"] == row["peak_reserved_bytes"] == expected)
    for service in (1, 3, 7):
        exact_checks.append(by_name[f"service-{service}"]["completion_span_cycles"] == 18 * (service + 2) - 1)
    fifo_names = [f"fifo-{d}" for d in (1, 2, 4)]
    report = dict(status="passed" if all(exact_checks) else "prediction_mismatch", cases=len(cases),
        unique_configurations=len(identical), duplicate_configurations=duplicates,
        deterministic_runs_are_not_independent_samples=True,
        scope="Synthetic logical KV bytes and this descriptor FIFO/service RTL only; no LLM/GPU latency claim.",
        fifo_service_start_edges_identical=all(details[n]["service_start_edges"] == details[fifo_names[0]]["service_start_edges"] for n in fifo_names),
        exact_block_and_serial_predictions_match=all(exact_checks), cases_detail=details)
    return metrics, steps, request_metrics, report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    try:
        metrics, steps, request_metrics, report = analyze(args.source)
        write_csv(args.output / "metrics.csv", metrics)
        write_csv(args.output / "steps.csv", steps)
        write_csv(args.output / "requests.csv", request_metrics)
    except Exception as exc:
        report = dict(status="failed", error=str(exc))
    (args.output / "analysis.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "cases_detail"}, indent=2))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
