"""Run external RTL workloads and preserve normal/failing evidence in a new folder."""
import argparse
import csv
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from check_axi_run import audit, rows


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--axi", type=Path, required=True)
    p.add_argument("--trace", type=Path, required=True)
    p.add_argument("--examples", type=Path, default=Path(__file__).resolve().parents[1] / "examples")
    p.add_argument("--output", type=Path)
    args = p.parse_args()
    root = args.output or Path(tempfile.mkdtemp(prefix="tiny-vllm-axi-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    commands, checks = [], {}

    def invoke(binary, config, name, rtl=None, fault=None, expected=0):
        result = root / name
        cmd = [str(binary.absolute()), "--config", str(config), "--output", str(result)]
        if rtl:
            cmd += ["--rtl-config", str(rtl)]
        if fault:
            cmd += ["--inject-fault", fault]
        entry = dict(command=cmd, expected_exit=expected, status="running")
        commands.append(entry)
        (root / "commands.json").write_text(json.dumps(commands, indent=2))
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        entry.update(status="finished", exit=proc.returncode, stdout=proc.stdout, stderr=proc.stderr)
        (root / "commands.json").write_text(json.dumps(commands, indent=2))
        assert proc.returncode == expected, entry
        return result

    def workload(name, text):
        path = root / (name + ".yaml")
        path.write_text(text)
        return path

    try:
        normal, paused = args.examples / "axi.yaml", args.examples / "axi-paused.yaml"
        for name in ("small", "mixed", "pressure"):
            config = args.examples / ({"small": "basic", "mixed": "concurrent", "pressure": "kv_pressure"}[name] + ".yaml")
            trace = invoke(args.trace, config, name + "-trace")
            for suffix, rtl in (("normal", normal), ("paused", paused)):
                run = invoke(args.axi, config, name + "-" + suffix, rtl)
                checked = audit(run, trace)
                checks[name + "-" + suffix] = checked
                assert (checked["write_bytes"], checked["read_bytes"]) == ((56,104) if name == "small" else (80,128))
                if suffix == "paused":
                    assert checked["b_stall_cycles"] > 0 and checked["r_stall_cycles"] > 0
                if name == "pressure":
                    assert [(r["request_id"], r["step"]) for r in rows(run/"events.csv") if r["event"] == "admitted"] == [("A","0"),("B","4")]
                if name == "small":
                    # Independent fixed address oracle from the original contract.
                    observed = [(int(r["address"]),int(r["bytes"]),int(r["write"])) for r in rows(run/"transactions.csv") if r["event"] == "completed"]
                    assert observed == [(0,12,1),(16,12,1),(12,4,1),(32,4,1),(28,4,1),(48,4,1),
                                        (36,4,1),(52,4,1),(0,16,0),(32,8,0),(16,16,0),(48,8,0),
                                        (40,4,1),(56,4,1),(0,16,0),(32,12,0),(16,16,0),(48,12,0)]
        text = (args.examples / "basic.yaml").read_text()
        multi = (args.examples / "concurrent.yaml").read_text().replace("num_layers: 1", "num_layers: 2").replace("kv_heads: 1", "kv_heads: 3").replace("head_dim: 2", "head_dim: 3")
        large = text.replace("head_dim: 2", "head_dim: 1537").replace("transaction_bytes: 16", "transaction_bytes: 8192").replace("max_backend_cycles: 10000", "max_backend_cycles: 200000")
        for name, contents, settings in (("multilayer-narrow", multi, paused), ("boundary-bursts", large, normal)):
            config = workload(name, contents)
            trace = invoke(args.trace, config, name + "-trace")
            run = invoke(args.axi, config, name, settings)
            checks[name] = audit(run, trace)
            assert checks[name]["narrow_bursts"] > 0 and checks[name]["split_descriptors"] > 0
        assert checks["boundary-bursts"]["max_burst_beats"] == 256
        assert (checks["boundary-bursts"]["write_bytes"], checks["boundary-bursts"]["read_bytes"]) == (43036,79924)
        assert (checks["multilayer-narrow"]["write_bytes"], checks["multilayer-narrow"]["read_bytes"]) == (720,1152)
        # Same resolved inputs replay the complete external RTL path.
        original = root / "small-paused"
        replay = invoke(args.axi, original/"resolved_config.yaml", "resolved-replay", original/"resolved_rtl_config.yaml")
        checks["resolved-replay"] = audit(replay, root/"small-trace")
        for file in ("transactions.csv", "events.csv", "axi_cycles.csv", "summary.csv", "axi_summary.csv"):
            assert (original/file).read_bytes() == (replay/file).read_bytes(), file
        short = workload("short-watchdog", text.replace("max_backend_cycles: 10000", "max_backend_cycles: 300"))
        for fault, status in (("read-data","protocol_error"), ("drop-completion","timeout")):
            run = invoke(args.axi, short, "fault-"+fault, normal, fault, 3)
            s = rows(run/"summary.csv")[0]
            assert s["status"] == status and s["completion_step"] == "" and int(s["final_allocated_bytes"]) > 0
            assert not any(r["event"] == "released" for r in rows(run/"events.csv"))
            if fault == "drop-completion":
                assert not any(r["event"] == "committed" for r in rows(run/"events.csv"))
            checks["fault-"+fault] = dict(status="expected_failure_detected", failure=status)
        invalid = workload("too-large", text.replace("head_dim: 2", "head_dim: 131073"))
        invoke(args.axi, invalid, "capacity-rejected", normal, expected=2)
        bad = workload("bad-axi", "max_burst_beats: 257\n")
        invoke(args.axi, args.examples/"basic.yaml", "bad-config-rejected", bad, expected=2)
        # Checker negative controls mutate COPIES, never original measurements.
        for kind in ("premature-release", "read-payload"):
            changed = root / ("checker-"+kind)
            shutil.copytree(root/"small-normal", changed)
            file = "events.csv" if kind == "premature-release" else "axi_cycles.csv"
            data = rows(changed/file)
            if kind == "premature-release":
                next(r for r in data if r["event"] == "released")["backend_cycle"] = "0"
            else:
                row = next(r for r in data if r["channel"] == "R" and r["valid"] == r["ready"] == "1")
                row["data"] = str(int(row["data"]) ^ 1)
            with (changed/file).open("w", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=data[0].keys()); writer.writeheader(); writer.writerows(data)
            try:
                audit(changed)
            except AssertionError as e:
                checks["checker-"+kind] = dict(status="corruption_detected", reason=str(e))
            else:
                raise AssertionError("Checker missed " + kind)
        (root/"validation.json").write_text(json.dumps(dict(status="passed", checks=checks), indent=2))
    except Exception as e:
        (root/"validation.json").write_text(json.dumps(dict(status="failed", error=str(e), checks=checks), indent=2))
        raise
    print(json.dumps(dict(status="passed", output=str(root), checks=checks), indent=2))


if __name__ == "__main__":
    main()
