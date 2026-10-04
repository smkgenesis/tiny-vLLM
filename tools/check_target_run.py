"""Audit standard target ports without assuming its latency, queues or service policy."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
from check_rtl_run import rows, require


def audit(directory, trace=None):
    directory = Path(directory)
    summary = rows(directory / "summary.csv")[0]
    target = rows(directory / "target_summary.csv")[0]
    require(summary["status"] == target["status"] == "ok", "Cannot certify failed/partial run")
    tx = rows(directory / "transactions.csv")
    fields = ("request_id", "step", "phase", "layer", "address", "bytes", "write")
    actions = {}
    for action in ("generated", "accepted", "completed"):
        entries = [r for r in tx if r["event"] == action]
        actions[action] = {int(r["transaction_id"]): r for r in entries}
        require(len(entries) == len(actions[action]), "Duplicate transaction event")
    generated, accepted, completed = (actions[k] for k in ("generated", "accepted", "completed"))
    require(generated.keys() == accepted.keys() == completed.keys(), "Incomplete transaction lifetime")
    for tid in accepted:
        require(all(generated[tid][f] == accepted[tid][f] == completed[tid][f] for f in fields), "Transaction mutated")
    requests, completions, pending = {}, {}, set()
    previous = None
    cycles = rows(directory / "target_cycles.csv")
    stalls = 0
    for edge, row in enumerate(cycles, 1):
        r = {k: int(v) for k, v in row.items()}
        require(r["edge"] == edge and r["cmp_ready"] == 1, "Invalid edge/adapter readiness")
        if previous and previous["req_valid"] and not previous["req_ready"]:
            require(r["req_valid"] and all(r[k] == previous[k] for k in ("req_id", "req_address", "req_bytes", "req_write")), "Stalled request changed")
        if r["req_valid"] and r["req_ready"]:
            tid = r["req_id"]
            require(tid in accepted and tid not in requests, "Unknown/duplicate request")
            require(int(accepted[tid]["backend_cycle"]) < edge, "DUT accepted before frontend")
            require(all(r["req_" + k] == int(accepted[tid][k]) for k in ("address", "bytes", "write")), "Wire payload differs from frontend")
            requests[tid] = edge; pending.add(tid)
        if r["cmp_valid"]:
            tid = r["cmp_id"]
            require(tid in pending and not r["cmp_error"], "Unknown/duplicate/error completion")
            require(int(completed[tid]["backend_cycle"]) == edge, "Core completion timing mismatch")
            completions[tid] = edge; pending.remove(tid)
        stalls += bool(r["req_valid"] and not r["req_ready"])
        previous = r
    require(not pending and requests.keys() == completions.keys() == accepted.keys(), "Unfinished target traffic")
    measured = dict(frontend_accepted=len(accepted), request_handshakes=len(requests), completion_handshakes=len(completions),
                    request_stall_cycles=stalls, first_request_edge=min(requests.values()), last_completion_edge=max(completions.values()),
                    completion_span_cycles=max(completions.values()) - min(requests.values()), observed_cycles=len(cycles), final_staging=0, final_inflight=0)
    for key, value in measured.items(): require(int(target[key]) == value, "Target summary mismatch: " + key)
    require(int(summary["backend_cycles"]) == len(cycles), "Backend cycle mismatch")
    require(all(summary[k] == "0" for k in ("final_outstanding", "final_allocated_bytes", "final_reserved_bytes", "final_active_requests")), "Resource leak")
    for write, field in (("0", "completed_read_bytes"), ("1", "completed_write_bytes")):
        require(int(summary[field]) == sum(int(r["bytes"]) for r in completed.values() if r["write"] == write), "Byte mismatch")
    groups = defaultdict(list)
    for tid, r in accepted.items(): groups[(r["request_id"], r["step"], int(r["layer"]))].append(tid)
    for (owner, step, layer), tids in groups.items():
        writes = [t for t in tids if accepted[t]["write"] == "1"]
        reads = [t for t in tids if accepted[t]["write"] == "0"]
        if reads: require(max(completions[t] for t in writes) < min(requests[t] for t in reads), "Read before write completion")
        if layer: require(max(completions[t] for t in groups[(owner, step, layer - 1)]) < min(requests[t] for t in tids), "Next layer began too early")
    for r in rows(directory / "events.csv"):
        if r["event"] in ("released", "committed"):
            key = "request_id" if r["event"] == "released" else "step"
            require(max(completions[t] for t in accepted if accepted[t][key] == r[key]) <= int(r["backend_cycle"]), "Premature release/commit")
    if trace:
        other = rows(Path(trace) / "transactions.csv")
        require(Counter(tuple(r[f] for f in fields) for r in completed.values()) ==
                Counter(tuple(r[f] for f in fields) for r in other if r["event"] == "completed"), "Trace/target transport differs")
    return dict(status="passed", **measured)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--trace", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(audit(args.directory, args.trace), indent=2))
    except (AssertionError, KeyError, ValueError, OSError) as exc:
        print(json.dumps(dict(status="failed", error=str(exc))))
        raise SystemExit(1)
