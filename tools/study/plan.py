"""Fixed workload and architecture study configurations."""
from copy import deepcopy


def build_plan():
    small = {
        "model": dict(num_layers=1, kv_heads=1, head_dim=2, dtype_bytes=2),
        "scheduler": dict(max_tokens_per_step=3, prefill_chunk_tokens=3, max_active_requests=1),
        "kv_cache": dict(block_tokens=4, pool_blocks=2),
        "driver": dict(transaction_bytes=16, max_outstanding=4, max_backend_cycles=10000),
        "requests": [dict(id="A", arrival_step=0, prompt_tokens=5, output_tokens=3)],
    }
    mixed = deepcopy(small)
    mixed["scheduler"]["max_active_requests"] = 2
    mixed["kv_cache"]["pool_blocks"] = 3
    mixed["requests"].append(dict(id="B", arrival_step=1, prompt_tokens=2, output_tokens=2))
    pressure = deepcopy(mixed)
    pressure["kv_cache"]["pool_blocks"] = 2
    cases = []

    def add(name, group, value, workload, prediction, rtl=None):
        model = workload["model"]
        token_bytes = 2 * model["num_layers"] * model["kv_heads"] * model["head_dim"] * model["dtype_bytes"]
        # Closed-form logical bytes, independent of scheduler/layout/transaction splitting.
        write = token_bytes * sum(r["prompt_tokens"] + r["output_tokens"] - 1 for r in workload["requests"])
        read = token_bytes * sum((r["output_tokens"] - 1) * r["prompt_tokens"] +
                                r["output_tokens"] * (r["output_tokens"] - 1) // 2 for r in workload["requests"])
        cases.append(dict(name=name, group=group, value=value, workload=deepcopy(workload),
                          rtl=rtl or dict(fifo_depth=2, service_cycles=3, initial_request_pause_cycles=0),
                          prediction=prediction, expected_write_bytes=write, expected_read_bytes=read))

    for name, config in (("small", small), ("mixed", mixed), ("pressure", pressure)):
        add("baseline-" + name, "baseline", name, config,
            "Reproduce the declared demo bytes, occupancy, and request schedule; all resources drain.")
    for prompt in (1, 5, 9):
        c = deepcopy(small)
        c["kv_cache"]["pool_blocks"] = 4
        c["requests"][0]["prompt_tokens"] = prompt
        add(f"prompt-{prompt}", "prompt", prompt, c,
            "At O=3, W=8(P+2), R=8(2P+3); both rise linearly with prompt length. Pool fixed at 128 B.")
    for output in (1, 3, 5):
        c = deepcopy(small)
        c["kv_cache"]["pool_blocks"] = 4
        c["requests"][0]["output_tokens"] = output
        add(f"output-{output}", "output", output, c,
            "At P=5, W=8(O+4), R=8(5(O-1)+O(O-1)/2). O=1 has no decode reads. Pool fixed at 128 B.")
    for active in (1, 2):
        c = deepcopy(mixed)
        c["scheduler"]["max_active_requests"] = active
        add(f"active-{active}", "active", active, c,
            "Same two requests and 96 B pool: active=1 delays B; active=2 overlaps reservations. W/R stay 80/128 B; splitting may change.")
    for chunk in (1, 2, 3):
        c = deepcopy(small)
        c["scheduler"]["prefill_chunk_tokens"] = chunk
        add(f"chunk-{chunk}", "chunk", chunk, c,
            "P=5,O=3,budget=3: W/R stay 56/104 B. Smaller chunks add prefill steps and alter transaction boundaries; decode reads are unchanged.")
    for block in (2, 4, 8):
        c = deepcopy(small)
        c["requests"][0]["output_tokens"] = 5
        c["kv_cache"] = dict(block_tokens=block, pool_blocks=128 // (8 * block))
        add(f"block-{block}", "block", block, c,
            "Capacity fixed at 128 B, final KV=9 tokens=72 B: reserved/allocated peak 80/96/128 B for B=2/4/8. W/R=72/240 B; no padding traffic.")
    for service in (1, 3, 7):
        c = deepcopy(small)
        c["driver"]["max_outstanding"] = 1
        add(f"service-{service}", "service", service, c,
            "Serial outstanding=1, 18 transactions: first request edge 1, final edge 18*(service+2), span 18*(service+2)-1. Bytes unchanged.",
            dict(fifo_depth=2, service_cycles=service, initial_request_pause_cycles=0))
    for depth in (1, 2, 4):
        c = deepcopy(mixed)
        c["driver"]["max_outstanding"] = 8
        add(f"fifo-{depth}", "fifo", depth, c,
            "Mixed requests, service=7,outstanding=8: larger FIFO may reduce request stalls and move waiting into FIFO; span need not improve with one service engine. Inspect actual queue occupancy, not only config.",
            dict(fifo_depth=depth, service_cycles=7, initial_request_pause_cycles=0))
    return dict(
        purpose="Explain workload occupancy/traffic and this synthetic RTL's timing, without model/GPU latency claims.",
        design="One axis within each group; no Cartesian sweep. Repeated identical configs are deterministic consistency checks, not independent samples.",
        semantics="Serving steps are logical work-bearing steps; RTL span is last completion edge minus first request edge; reset excluded.",
        cases=cases,
    )


def build_architecture_plan():
    """Architectural comparison with matched total waiting slots, not silicon area."""
    base = {
        "model": dict(num_layers=1, kv_heads=1, head_dim=8, dtype_bytes=2),
        "scheduler": dict(max_tokens_per_step=32, prefill_chunk_tokens=4, max_active_requests=1),
        "kv_cache": dict(block_tokens=4, pool_blocks=64),
        "driver": dict(transaction_bytes=64, max_outstanding=16, max_backend_cycles=100000),
    }
    cases = []
    for group, caps in (("single-request", (1,)), ("homogeneous", (1, 2, 4, 8)), ("mixed", (1, 2, 4, 8))):
        for cap in caps:
            c = deepcopy(base)
            c["scheduler"]["max_active_requests"] = cap
            lengths = [(8, 8)] * (1 if group == "single-request" else 8)
            if group == "mixed":
                lengths = [(24, 2), (2, 16)] * 4
            c["requests"] = [dict(id=f"R{i}", arrival_step=0, prompt_tokens=p, output_tokens=o)
                             for i, (p, o) in enumerate(lengths)]
            cases.append(dict(name=f"{group}-c{cap}", group=group, concurrency=cap, workload=c))
    for group in ("homogeneous", "mixed"):
        c = deepcopy(next(c for c in cases if c["name"] == f"{group}-c4"))
        c["name"] += "-serial-driver"
        c["workload"]["driver"]["max_outstanding"] = 1
        cases.append(c)
    for c in cases:
        req = c["workload"]["requests"]
        c["expected_write_bytes"] = 32 * sum(r["prompt_tokens"] + r["output_tokens"] - 1 for r in req)
        c["expected_read_bytes"] = 32 * sum((r["output_tokens"] - 1) * r["prompt_tokens"] +
                                            r["output_tokens"] * (r["output_tokens"] - 1) // 2 for r in req)
    return dict(
        question="When does serving concurrency expose read/write overlap to independent RTL engines?",
        architectures=["single", "dual-rw"],
        rtl=dict(fifo_depth=8, service_cycles=7, initial_request_pause_cycles=0),
        fairness="Same total waiting capacity (single 8; dual 4+4), service per descriptor, one ingress and one completion port. Dual adds an active engine; not equal-area or equal-resource optimization.",
        prediction="A single request and outstanding=1 controls cannot overlap dependent phases. Multiple requests may expose overlap; homogeneous lockstep, staging HOL, queue partitioning and global barriers may limit or reverse gains. No monotonic speedup prediction.",
        metrics="Span=last completion minus first request. Busy includes completion wait. Overlap=both-busy/any-busy; utilization=direction-busy/all post-reset edges. FIFO occupancy sampled before each edge. Stall categories may overlap and are not an additive span decomposition.",
        limitations="Synthetic lengths and fixed-cost descriptors, no numeric inference or measured device performance. Concurrency changes batching/splitting; compare architecture within identical input pairs.",
        waveform_case="mixed-c4", cases=cases)


def yaml_text(config):
    """Write this plan's narrow scalar/list schema, without a Python YAML dependency."""
    import json
    lines = []
    for key, value in config.items():
        if isinstance(value, dict):
            lines.append(f"{key}:")
            lines.extend(f"  {k}: {json.dumps(v)}" for k, v in value.items())
        elif isinstance(value, list):
            lines.append(f"{key}:")
            for item in value:
                for index, (k, v) in enumerate(item.items()):
                    lines.append(f"  {'- ' if index == 0 else '  '}{k}: {json.dumps(v)}")
        else:
            lines.append(f"{key}: {json.dumps(value)}")
    return "\n".join(lines) + "\n"
