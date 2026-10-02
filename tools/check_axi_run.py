"""Audit external AXI wire history, byte coverage, payload, and KV lifetimes.

This checks the transport of the declared synthetic access policy, not real GPU
traffic or numerical attention. It does not import the C++ burst splitter.
"""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path


def rows(path):
    with Path(path).open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def require(value, message):
    if not value:
        raise AssertionError(message)


def audit(directory, trace=None):
    directory = Path(directory)
    summary = rows(directory / "summary.csv")[0]
    hardware = rows(directory / "axi_summary.csv")[0]
    require(summary["status"] == hardware["status"] == "ok", "Failed/partial run")
    tx = rows(directory / "transactions.csv")
    groups = {a: {int(r["transaction_id"]): r for r in tx if r["event"] == a}
              for a in ("generated", "accepted", "completed")}
    for a, g in groups.items():
        require(len(g) == sum(r["event"] == a for r in tx), "Duplicate descriptor lifetime record")
    descriptors = groups["accepted"]
    require(groups["generated"].keys() == descriptors.keys() == groups["completed"].keys(), "Incomplete descriptors")
    fields = ("request_id", "step", "phase", "layer", "address", "bytes", "write")
    for tid, r in descriptors.items():
        for action in ("generated", "completed"):
            require(all(r[k] == groups[action][tid][k] for k in fields), "Descriptor changed")

    memory, coverage, first, last = {}, defaultdict(list), {}, {}
    channels = ("AW", "W", "B", "AR", "R")
    previous, active = {}, {"W": None, "R": None}
    counts, stalls = Counter(), Counter()
    bursts, narrow, max_beats = [], 0, 0
    for index, text in enumerate(rows(directory / "axi_cycles.csv")):
        ch = text["channel"]
        r = {k: int(v) for k, v in text.items() if k != "channel"}
        edge = index // 5 + 1
        require(ch == channels[index % 5] and r["edge"] == edge, "Missing/reordered AXI edge")
        old = previous.get(ch)
        if old and old["valid"] and not old["ready"]:
            require(r["valid"] == 1 and all(r[k] == old[k] for k in r if k not in ("edge", "ready")), "Unstable stalled AXI payload")
        previous[ch] = r
        if r["valid"] and not r["ready"]:
            stalls[ch] += 1
        if not (r["valid"] and r["ready"]):
            continue
        counts[ch] += 1
        if ch in ("AW", "AR"):
            lane = "W" if ch == "AW" else "R"
            require(active[lane] is None, "Multiple bursts in one adapter lane")
            tid, address, beats, size = r["id"], r["address"], r["len"] + 1, 1 << r["size"]
            require(tid in descriptors and descriptors[tid]["write"] == str(int(ch == "AW")), "Unknown/wrong-direction address")
            require(r["burst"] == 1 and 1 <= beats <= 256 and size <= 8 and address % size == 0, "Invalid aligned INCR burst")
            require(address // 4096 == (address + beats * size - 1) // 4096, "Burst crosses 4 KiB")
            require(int(descriptors[tid]["backend_cycle"]) < edge, "Bus request before frontend accept")
            coverage[tid].append((address, address + beats * size))
            active[lane] = dict(id=tid, address=address, beats=beats, size=size, seen=0)
            first.setdefault(tid, edge)
            narrow += size < 8
            max_beats = max(max_beats, beats)
            bursts.append((tid, address, beats * size))
        elif ch in ("W", "R"):
            t = active[ch]
            require(t is not None and t["seen"] < t["beats"], "Data without address / too many beats")
            address = t["address"] + t["seen"] * t["size"]
            require(r["last"] == int(t["seen"] + 1 == t["beats"]), "Invalid LAST")
            if ch == "W":
                mask = sum(1 << (a % 8) for a in range(address, address + t["size"]))
                require(r["strb"] == mask, "Write strobe covers wrong bytes")
                for a in range(address, address + t["size"]):
                    value = (r["data"] >> (8 * (a % 8))) & 255
                    require(value == (t["id"] * 131 + a * 17 + (a >> 8)) % 256, "Wrong synthetic W payload")
                    memory[a] = value
            else:
                require(r["id"] == t["id"] and r["resp"] == 0, "Wrong R ID/response")
                for a in range(address, address + t["size"]):
                    require(a in memory and memory[a] == ((r["data"] >> (8 * (a % 8))) & 255), "RAM readback mismatch")
            t["seen"] += 1
            if ch == "R" and r["last"]:
                last[t["id"]] = edge
                active[ch] = None
        else:
            t = active["W"]
            require(t is not None and t["seen"] == t["beats"] and r["id"] == t["id"] and r["resp"] == 0, "Premature/wrong B response")
            last[t["id"]] = edge
            active["W"] = None
    require(index % 5 == 4 and not any(active.values()), "Undrained/truncated bus history")
    require(descriptors.keys() == first.keys() == last.keys(), "Missing bus completion")
    for tid, descriptor in descriptors.items():
        cursor = int(descriptor["address"])
        for begin, end in coverage[tid]:
            require(begin == cursor, "Descriptor byte hole/overlap/reorder")
            cursor = end
        require(cursor == int(descriptor["address"]) + int(descriptor["bytes"]), "Descriptor under/over-transfer")
        require(int(groups["completed"][tid]["backend_cycle"]) == last[tid], "Frontend completed before/after final AXI response")
    dependent = defaultdict(list)
    for tid, r in descriptors.items():
        dependent[(r["request_id"], r["step"], int(r["layer"]))].append(tid)
    for (owner, step, layer), tids in dependent.items():
        writes = [t for t in tids if descriptors[t]["write"] == "1"]
        reads = [t for t in tids if descriptors[t]["write"] == "0"]
        if reads:
            require(max(last[t] for t in writes) < min(first[t] for t in reads), "Read before own writes completed")
        if layer:
            require(max(last[t] for t in dependent[(owner, step, layer-1)]) < min(first[t] for t in tids), "Layer advanced too early")
    for e in rows(directory / "events.csv"):
        if e["event"] in ("committed", "released"):
            scope = "step" if e["event"] == "committed" else "request_id"
            require(max(last[t] for t, r in descriptors.items() if r[scope] == e[scope]) <= int(e["backend_cycle"]), "Premature commit/release")
    for key in ("final_allocated_bytes", "final_reserved_bytes", "final_valid_kv_bytes", "final_outstanding", "final_active_requests"):
        require(summary[key] == "0", "Resource leak")
    measured = {"descriptors_accepted": len(descriptors), "descriptors_completed": len(descriptors),
                "aw_bursts": counts["AW"], "w_beats": counts["W"], "b_responses": counts["B"],
                "ar_bursts": counts["AR"], "r_beats": counts["R"], "b_stall_cycles": stalls["B"],
                "r_stall_cycles": stalls["R"], "backend_cycles": edge}
    require(int(summary["backend_cycles"]) == edge, "Core/bus cycle mismatch")
    for key, value in measured.items():
        require(int(hardware[key]) == value, "AXI summary mismatch: " + key)
    for write, key in (("1", "write_bytes"), ("0", "read_bytes")):
        size = sum(int(r["bytes"]) for r in descriptors.values() if r["write"] == write)
        require(size == int(hardware[key]) == int(summary["completed_" + key]), "Byte total mismatch")
        measured[key] = size
    if trace:
        reference = rows(Path(trace) / "transactions.csv")
        require(Counter(tuple(r[f] for f in fields) for r in tx if r["event"] == "completed") ==
                Counter(tuple(r[f] for f in fields) for r in reference if r["event"] == "completed"), "Trace/AXI transport differs")
    return {"status": "passed", **measured, "narrow_bursts": narrow, "max_burst_beats": max_beats,
            "split_descriptors": sum(len(c) > 1 for c in coverage.values()), "trace_transport_compared": trace is not None}


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path)
    p.add_argument("--trace", type=Path)
    a = p.parse_args()
    print(json.dumps(audit(a.directory, a.trace), indent=2))
