"""Audit persisted RTL wires independently of the C++ adapter's counters.

Trace comparison checks transport consistency, not correctness of the shared
access generator. The core's independent address oracle remains separate.
"""
import argparse
from collections import Counter, defaultdict, deque
import csv
import json
from pathlib import Path


def rows(path):
    with Path(path).open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def audit(directory, trace=None, events_path=None):
    directory = Path(directory)
    summary = rows(directory / "summary.csv")[0]
    rtl_summary = rows(directory / "rtl_summary.csv")[0]
    require(summary["status"] == rtl_summary["status"] == "ok", "Cannot certify a failed/partial run")
    settings = dict(line.split(":", 1) for line in (directory / "resolved_rtl_config.yaml").read_text().splitlines())
    depth, service = int(settings["fifo_depth"]), int(settings["service_cycles"])
    architecture = settings.get("architecture", "single").strip().strip('"')
    require(architecture in ("single", "dual-rw"), "Unknown RTL architecture")
    dual = architecture == "dual-rw"
    tx = rows(directory / "transactions.csv")
    by_action = {action: {int(r["transaction_id"]): r for r in tx if r["event"] == action}
                 for action in ("generated", "accepted", "completed")}
    for action, group in by_action.items():
        require(len(group) == sum(r["event"] == action for r in tx), f"Duplicate {action} record")
    require(by_action["generated"].keys() == by_action["accepted"].keys() == by_action["completed"].keys(), "Incomplete transaction lifetime")
    fields = ("request_id", "step", "phase", "layer", "address", "bytes", "write")
    for tid, generated in by_action["generated"].items():
        for action in ("accepted", "completed"):
            require(all(generated[f] == by_action[action][tid][f] for f in fields), "Transaction mutated after generation")

    queue = deque()
    direction_queues = [deque(), deque()]
    active = [None, None]
    counters = Counter()
    starts, requests, completions = {}, {}, {}
    expected_handshakes = []
    stalls, peak = 0, 0
    previous = None
    cycles = rows(directory / "rtl_cycles.csv")
    for edge, text in enumerate(cycles, 1):
        r = {k: int(v) for k, v in text.items()}
        require(r["edge"] == edge, "Nonconsecutive RTL edges")
        require(r["cmp_ready"] == 1, "Adapter unexpectedly stopped completion ready")
        eligible = [tid for tid in active if tid is not None and edge >= starts[tid] + service]
        require(bool(r["cmp_valid"]) == bool(eligible), "Completion arbiter failed to present an eligible response")
        if eligible:
            require(r["cmp_id"] in eligible, "Completion from ineligible engine")
        target_depth = depth // 2 if dual else depth
        target_count = len(direction_queues[r["req_write"]]) if dual else len(queue)
        require(bool(r["req_ready"]) == (edge > int(settings["initial_request_pause_cycles"]) and target_count < target_depth), "Request ready differs from configured capacity/gate")
        require(r["queue_count"] == sum(map(len, direction_queues)) <= depth, "FIFO occupancy disagrees with wire history")
        for direction, label in enumerate(("read", "write")):
            q = direction_queues[direction]
            require(r[label + "_queue_count"] == len(q) <= (depth // 2 if dual else depth), "Direction queue mismatch")
            require(bool(r[label + "_busy"]) == (active[direction] is not None), "Engine busy disagrees with start/completion history")
            tid = active[direction]
            waiting = tid is not None and edge >= starts[tid] + service and not (r["cmp_valid"] and r["cmp_id"] == tid)
            require(bool(r[label + "_completion_wait"]) == waiting, "Completion wait mismatch")
            counters[label + "_completion_wait_cycles"] += waiting
            counters[label + "_queue_sum"] += len(q)
            counters[label + "_peak_queue_count"] = max(counters[label + "_peak_queue_count"], len(q))
        require(dual or not all(t is not None for t in active), "Single engine overlapped")
        if all(t is not None for t in active):
            require(by_action["accepted"][active[0]]["request_id"] != by_action["accepted"][active[1]]["request_id"], "Same request overlapped dependent read/write phases")
        category = "overlap" if all(t is not None for t in active) else "read_only" if active[0] is not None else "write_only" if active[1] is not None else "idle"
        counters[category + "_cycles"] += 1
        if previous and previous["req_valid"] and not previous["req_ready"]:
            require(r["req_valid"] and all(r[f] == previous[f] for f in
                    ("req_id", "req_address", "req_bytes", "req_write")), "Stalled request was lost or changed")
        if r["cmp_valid"] and r["cmp_ready"]:
            tid = r["cmp_id"]
            require(tid in requests and tid not in completions and tid in starts, "Unknown/duplicate/premature completion")
            require(not r["cmp_error"], "RTL completion contains address error")
            require(edge - starts[tid] >= service if dual else edge - starts[tid] == service, "Service cycle setting did not reach actual RTL path")
            direction = int(by_action["accepted"][tid]["write"])
            require(active[direction] == tid, "Completion not from active engine")
            active[direction] = None
            completions[tid] = edge
            require(int(by_action["completed"][tid]["backend_cycle"]) == edge, "Core completion observed at wrong edge")
        for direction, label in enumerate(("read", "write")):
            if r[label + "_service_start"]:
                tid = r[label + "_service_id"]
                require(active[direction] is None, "Overwriting busy engine")
                q = direction_queues[direction]
                require(q and q.popleft() == tid and tid not in starts, "Service start violated direction FIFO order")
                if not dual:
                    require(queue and queue.popleft() == tid, "Service start violated shared FIFO order")
                starts[tid] = edge
                active[direction] = tid
        if r["req_valid"] and r["req_ready"]:
            tid = r["req_id"]
            require(tid in by_action["accepted"] and tid not in requests, "Unknown/duplicate RTL request")
            require(int(by_action["accepted"][tid]["backend_cycle"]) < edge, "RTL accepted before frontend")
            for wire, field in (("req_address", "address"), ("req_bytes", "bytes"), ("req_write", "write")):
                require(r[wire] == int(by_action["accepted"][tid][field]), "RTL payload differs from frontend")
            requests[tid] = edge
            if not dual:
                queue.append(tid)
            direction_queues[r["req_write"]].append(tid)
            counters[("write" if r["req_write"] else "read") + "_requests"] += 1
            expected_handshakes.append(("request", edge, tid))
        if r["cmp_valid"] and r["cmp_ready"]:
            expected_handshakes.append(("completion", edge, r["cmp_id"]))
        if r["req_valid"] and not r["req_ready"]:
            stalls += 1
        peak = max(peak, sum(map(len, direction_queues)), r["queue_count"])
        previous = r
    require(not queue and not any(direction_queues) and active == [None, None] and requests.keys() == completions.keys() == by_action["accepted"].keys(), "Unfinished RTL requests")
    handshakes = rows(directory / "rtl_handshakes.csv")
    require([(r["event"], int(r["edge"]), int(r["transaction_id"])) for r in handshakes] == expected_handshakes, "Handshake recorder disagrees with wires")
    latencies = {tid: completions[tid] - requests[tid] for tid in requests}
    for r in handshakes:
        require(r["error"] == "0", "Error handshake in normal run")
        tid = int(r["transaction_id"])
        for field in ("address", "bytes", "write"):
            require(r[field] == by_action["accepted"][tid][field], "Handshake payload mismatch")
        if r["event"] == "completion":
            require(int(r["latency_cycles"]) == latencies[tid], "Latency recorder mismatch")
    measured = {"frontend_accepted": len(requests), "request_handshakes": len(requests),
                "completion_handshakes": len(completions), "valid_completions": len(completions),
                "request_stall_cycles": stalls, "peak_queue_count": peak,
                "first_request_edge": min(requests.values()), "last_completion_edge": max(completions.values()),
                "completion_span_cycles": max(completions.values()) - min(requests.values()),
                "latency_sum_cycles": sum(latencies.values()), "latency_max_cycles": max(latencies.values()),
                "final_staging": 0, "final_inflight": 0, "final_queue_count": 0, "final_service_busy": 0}
    for key in ("idle_cycles", "read_only_cycles", "write_only_cycles", "overlap_cycles", "read_completion_wait_cycles",
                "write_completion_wait_cycles", "read_requests", "write_requests", "read_queue_sum", "write_queue_sum",
                "read_peak_queue_count", "write_peak_queue_count"):
        measured[key] = counters[key]
    measured["observed_cycles"] = len(cycles)
    for key, value in measured.items():
        require(int(rtl_summary[key]) == value, f"RTL summary mismatch: {key}")
    require(int(summary["backend_cycles"]) == len(cycles), "Backend clock count mismatch")
    for field in ("final_allocated_bytes", "final_reserved_bytes", "final_outstanding", "final_active_requests"):
        require(summary[field] == "0", f"Resource leak: {field}")
    for write, field in (("1", "completed_write_bytes"), ("0", "completed_read_bytes")):
        require(int(summary[field]) == sum(int(t["bytes"]) for t in by_action["completed"].values() if t["write"] == write), "Byte accounting mismatch")

    # Per-request/layer dependency and block lifetime checks use wire completion
    # edges, not the frontend's accepted or generated timestamps.
    grouped = defaultdict(list)
    for tid, r in by_action["accepted"].items():
        grouped[(r["request_id"], r["step"], int(r["layer"]))].append(tid)
    for (owner, step, layer), tids in grouped.items():
        writes = [tid for tid in tids if by_action["accepted"][tid]["write"] == "1"]
        reads = [tid for tid in tids if by_action["accepted"][tid]["write"] == "0"]
        if reads:
            require(max(completions[t] for t in writes) < min(requests[t] for t in reads), "Read before writes completed")
        if layer:
            require(max(completions[t] for t in grouped[(owner, step, layer - 1)]) < min(requests[t] for t in tids), "Next layer began too early")
    events = rows(events_path or directory / "events.csv")
    for event in events:
        if event["event"] == "released":
            tids = [tid for tid, r in by_action["accepted"].items() if r["request_id"] == event["request_id"]]
            require(max(completions[t] for t in tids) <= int(event["backend_cycle"]), "Premature block release before RTL completion")
        if event["event"] == "committed":
            tids = [tid for tid, r in by_action["accepted"].items() if r["step"] == event["step"]]
            require(max(completions[t] for t in tids) <= int(event["backend_cycle"]), "Commit before global step barrier")
    if trace:
        other = rows(Path(trace) / "transactions.csv")
        require(Counter(tuple(r[f] for f in fields) for r in tx if r["event"] == "completed") ==
                Counter(tuple(r[f] for f in fields) for r in other if r["event"] == "completed"), "Trace/RTL transaction contents differ")
    return {"status": "passed", "transactions": len(requests), **measured,
            "trace_transport_compared": trace is not None}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path)
    p.add_argument("--trace", type=Path)
    p.add_argument("--events", type=Path, help="Alternate events for checker negative controls")
    args = p.parse_args()
    try:
        result = audit(args.directory, args.trace, args.events)
    except (AssertionError, KeyError, ValueError, OSError) as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}))
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
