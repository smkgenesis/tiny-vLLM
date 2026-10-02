"""Actual trace/RTL CLI checks. Optional fourth argument preserves all evidence."""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_rtl_run import audit, rows


def exercise(rtl, trace, example, root):
    commands = []
    checker = Path(__file__).resolve().parents[1] / "tools" / "check_rtl_run.py"
    text = example.read_text(encoding="utf-8")

    def invoke(executable, config, name, settings=None, fault=None, code=0):
        destination = root / name
        command = [str(executable), "--config", str(config), "--output", str(destination)]
        if settings:
            command += ["--rtl-config", str(settings)]
        if fault:
            command += ["--inject-fault", fault]
        process = subprocess.run(command, text=True, capture_output=True, timeout=20)
        entry = dict(command=command, expected_exit=code, exit=process.returncode,
                     stdout=process.stdout, stderr=process.stderr)
        commands.append(entry)
        (root / "commands.json").write_text(json.dumps(commands, indent=2), encoding="utf-8")
        assert process.returncode == code, entry
        return destination

    def settings(name, depth=2, service=3, pause=0):
        path = root / f"{name}-rtl.yaml"
        path.write_text(f"fifo_depth: {depth}\nservice_cycles: {service}\ninitial_request_pause_cycles: {pause}\n", encoding="utf-8")
        return path

    def workload(name, contents):
        path = root / f"{name}.yaml"
        path.write_text(contents, encoding="utf-8")
        return path

    defaults = settings("default")
    checked = {}
    for case in ("small", "mixed", "pressure"):
        config = example.parent / f"{case}.yaml"
        reference = invoke(trace, config, f"{case}-trace")
        result = invoke(rtl, config, f"{case}-rtl", defaults)
        checked[case] = audit(result, reference)
        summary = rows(result / "summary.csv")[0]
        assert (summary["completed_write_bytes"], summary["completed_read_bytes"]) == (("56", "104") if case == "small" else ("80", "128"))
        if case == "small":
            # Independently specified stage-0 address oracle, not trace output.
            completed = rows(result / "transactions.csv")
            observed = [(int(r["address"]), int(r["bytes"]), int(r["write"])) for r in completed if r["event"] == "completed"]
            assert observed == [(0,12,1),(16,12,1),(12,4,1),(32,4,1),(28,4,1),(48,4,1),
                                (36,4,1),(52,4,1),(0,16,0),(32,8,0),(16,16,0),(48,8,0),
                                (40,4,1),(56,4,1),(0,16,0),(32,12,0),(16,16,0),(48,12,0)]
        if case == "pressure":
            events = rows(result / "events.csv")
            assert [(r["request_id"], r["step"]) for r in events if r["event"] == "admitted"] == [("A","0"),("B","4")]

    # Serial outstanding restriction must reach the driver, distinct from FIFO stalls.
    serial = workload("serial", text.replace("max_outstanding: 4", "max_outstanding: 1"))
    serial_results = []
    for service in (1, 7):
        result = invoke(rtl, serial, f"serial-{service}", settings(f"serial-{service}", service=service))
        measured = audit(result, root / "small-trace")
        serial_results.append(measured)
        waits = rows(result / "driver_waits.csv")
        assert any(r["reason"] == "outstanding_limit" for r in waits)
        assert rows(result / "summary.csv")[0]["peak_outstanding"] == "1"
        assert measured["request_stall_cycles"] == 0
    assert serial_results[1]["completion_span_cycles"] - serial_results[0]["completion_span_cycles"] == 18 * 6

    stalled = invoke(rtl, example, "stalled", settings("stalled", depth=1, service=7, pause=8))
    checked["stalled"] = audit(stalled, root / "small-trace")
    assert checked["stalled"]["request_stall_cycles"] > 8
    assert checked["stalled"]["first_request_edge"] == 9
    assert checked["stalled"]["peak_queue_count"] == 1
    assert any(r["reason"] == "backend_staging" for r in rows(stalled / "driver_waits.csv"))

    multilayer_text = (example.parent / "mixed.yaml").read_text(encoding="utf-8").replace("num_layers: 1", "num_layers: 2")
    multilayer = workload("multilayer", multilayer_text)
    reference = invoke(trace, multilayer, "multilayer-trace")
    result = invoke(rtl, multilayer, "multilayer-rtl", settings("multilayer", depth=1, service=9, pause=3))
    checked["multilayer"] = audit(result, reference)
    assert rows(result / "summary.csv")[0]["completed_write_bytes"] == "160"

    # Corrupt the wire address, or the RTL's completion behavior. These are
    # actual executable runs, and must leave allocations live on failure.
    short = workload("short-watchdog", text.replace("max_backend_cycles: 10000", "max_backend_cycles: 100"))
    for fault, status in (("address", "protocol_error"), ("duplicate-completion", "protocol_error"), ("drop-completion", "timeout")):
        result = invoke(rtl, short, f"fault-{fault}", defaults, fault, 3)
        partial = rows(result / "summary.csv")[0]
        assert partial["status"] == status and partial["completion_step"] == ""
        assert int(partial["final_allocated_bytes"]) > 0 and int(partial["final_outstanding"]) > 0
        assert not any(r["event"] in ("committed", "released") for r in rows(result / "events.csv"))
        assert rows(result / "rtl_summary.csv")[0]["completion_span_cycles"] == ""
        if fault == "address":
            assert any(r["event"] == "completion" and r["error"] == "1" for r in rows(result / "rtl_handshakes.csv"))
        if fault == "drop-completion":
            assert partial["backend_cycles"] == "100"

    # A premature release is injected into a COPY of evidence to test the
    # independent lifetime checker; the running core is not modified.
    events = rows(root / "small-rtl" / "events.csv")
    next(r for r in events if r["event"] == "released")["backend_cycle"] = "0"
    altered = root / "premature-release-events.csv"
    with altered.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=events[0].keys())
        writer.writeheader(); writer.writerows(events)
    negative = subprocess.run([sys.executable, str(checker), str(root / "small-rtl"), "--events", str(altered)], text=True, capture_output=True, timeout=20)
    (root / "premature-release-check.json").write_text(json.dumps(dict(exit=negative.returncode, stdout=negative.stdout, stderr=negative.stderr), indent=2), encoding="utf-8")
    assert negative.returncode == 1 and "Premature block release" in negative.stdout

    # Errors and a second attempt must preserve prior evidence.
    original = (root / "small-rtl" / "summary.csv").read_bytes()
    invoke(rtl, example, "small-rtl", defaults, code=4)
    assert (root / "small-rtl" / "summary.csv").read_bytes() == original
    bad = settings("invalid-depth", depth=65)
    result = invoke(rtl, example, "invalid-depth", bad, code=2)
    assert rows(result / "summary.csv")[0]["status"] == "config_error"
    invoke(rtl, example, "invalid-fault", defaults, fault="unknown", code=2)
    replay = invoke(rtl, root / "mixed-rtl" / "resolved_config.yaml", "resolved-replay", root / "mixed-rtl" / "resolved_rtl_config.yaml")
    checked["replay"] = audit(replay, root / "mixed-trace")
    (root / "checks.json").write_text(json.dumps({"status": "passed", "wire_audits": checked, "serial_latency": serial_results,
        "faults_detected": ["address", "duplicate-completion", "drop-completion"],
        "checker_negative_control": "copied premature release detected"}, indent=2), encoding="utf-8")
    print("RTL wire/trace audits, independent addresses, backpressure, dependencies, fault runs and lifetime negative control passed")


if __name__ == "__main__":
    rtl, trace, example = (Path(p).resolve() for p in sys.argv[1:4])
    if len(sys.argv) == 5:
        output = Path(sys.argv[4]).resolve()
        output.mkdir(parents=True, exist_ok=False)
        exercise(rtl, trace, example, output)
    else:
        with tempfile.TemporaryDirectory(prefix="tiny-vllm-rtl-") as temporary:
            exercise(rtl, trace, example, Path(temporary))
