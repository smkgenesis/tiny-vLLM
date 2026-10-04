"""Standard-port tutorial, replay, watchdog and independent checker negative control."""
import csv
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_target_run import audit, rows


def exercise(target, trace, examples, root):
    def invoke(binary, config, name, *options, code=0):
        result = root / name
        p = subprocess.run([str(binary), "--config", str(config), "--output", str(result), *options], capture_output=True, text=True, timeout=20)
        assert p.returncode == code, (p.returncode, p.stdout, p.stderr)
        return result
    # Closed-form policy totals, independent of the trace/target generator.
    profiles = {"basic": (56, 104), "concurrent": (80, 128), "kv_pressure": (80, 128),
                "short_chat": (4992, 32256), "mixed_chat": (8448, 62144), "long_context": (16608, 115584)}
    for name, (write, read) in profiles.items():
        config = examples / (name + ".yaml")
        reference = invoke(trace, config, name + "-trace")
        result = invoke(target, config, name, *(["--waveform"] if name == "basic" else []))
        measured = audit(result, reference)
        if name in ("basic", "concurrent", "kv_pressure"):
            assert measured["completion_handshakes"] == {"basic": 18, "concurrent": 26, "kv_pressure": 24}[name]
        s = rows(result / "summary.csv")[0]
        assert (int(s["completed_write_bytes"]), int(s["completed_read_bytes"])) == (write, read)
        if name == "basic": assert (result / "waveform.vcd").stat().st_size > 100
    original = (root / "basic" / "summary.csv").read_bytes()
    invoke(target, examples / "basic.yaml", "basic", code=4)
    assert original == (root / "basic" / "summary.csv").read_bytes()
    replay = invoke(target, root / "basic" / "resolved_config.yaml", "replay")
    assert not (replay / "waveform.vcd").exists()
    assert (replay / "target_cycles.csv").read_bytes() == (root / "basic" / "target_cycles.csv").read_bytes()
    # Watchdog must preserve uncompleted work, never release it as successful.
    short = root / "short.yaml"
    short.write_text((examples / "basic.yaml").read_text().replace("max_backend_cycles: 10000", "max_backend_cycles: 2"))
    failed = invoke(target, short, "timeout", code=3)
    assert rows(failed / "summary.csv")[0]["status"] == "timeout"
    assert rows(failed / "target_summary.csv")[0]["completion_span_cycles"] == ""
    assert not any(r["event"] == "released" for r in rows(failed / "events.csv"))
    # A copied replay is disposable negative evidence; never alter the good run.
    wires = rows(replay / "target_cycles.csv")
    next(r for r in wires if r["cmp_valid"] == "1")["cmp_id"] = "999999"
    with (replay / "target_cycles.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=wires[0].keys()); writer.writeheader(); writer.writerows(wires)
    try: audit(replay)
    except AssertionError as exc: assert "completion" in str(exc)
    else: raise AssertionError("Corrupted completion accepted")
    print("Template CLI, trace consistency, VCD, replay, watchdog and checker negative control passed")


if __name__ == "__main__":
    target, trace, examples = (Path(p).resolve() for p in sys.argv[1:4])
    if len(sys.argv) == 5:
        root = Path(sys.argv[4]).resolve(); root.mkdir(parents=True, exist_ok=False)
        exercise(target, trace, examples, root)
    else:
        with tempfile.TemporaryDirectory(prefix="tiny-target-cli-") as temp:
            exercise(target, trace, examples, Path(temp))
