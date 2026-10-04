"""Run the fixed small experiment plan, preserving every attempted case."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import traceback

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_rtl_run import audit, rows, require
from plan import build_plan, yaml_text


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--rtl", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for executable in (args.trace, args.rtl):
        if not executable.is_file():
            parser.error(f"Missing executable: {executable}")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    plan = build_plan()
    plan["declared_at_utc"] = datetime.now(timezone.utc).isoformat()
    save(output / "plan.json", plan)  # Record inputs and expected behavior.
    results = []
    for case in plan["cases"]:
        folder = output / case["name"]
        folder.mkdir()
        (folder / "workload.yaml").write_text(yaml_text(case["workload"]), encoding="utf-8")
        (folder / "rtl.yaml").write_text(yaml_text(case["rtl"]), encoding="utf-8")
        result = dict(name=case["name"], status="running", commands=[])
        results.append(result)
        save(output / "runs.json", results)
        try:
            for backend, executable in (("trace", args.trace), ("rtl", args.rtl)):
                command = [str(executable.resolve()), "--config", str(folder / "workload.yaml"), "--output", str(folder / backend)]
                if backend == "rtl":
                    command += ["--rtl-config", str(folder / "rtl.yaml")]
                entry = dict(backend=backend, command=command)
                result["commands"].append(entry)
                save(output / "runs.json", results)
                try:
                    proc = subprocess.run(command, text=True, capture_output=True, timeout=30)
                    entry.update(exit=proc.returncode, stdout=proc.stdout, stderr=proc.stderr)
                    save(folder / f"{backend}-command.json", entry)
                    require(proc.returncode == 0, f"{backend} failed with exit {proc.returncode}")
                except subprocess.TimeoutExpired as exc:
                    entry.update(error="subprocess timeout", stdout=str(exc.stdout), stderr=str(exc.stderr))
                    save(folder / f"{backend}-command.json", entry)
                    raise
            checked = audit(folder / "rtl", folder / "trace")
            summary = rows(folder / "rtl" / "summary.csv")[0]
            require(int(summary["completed_write_bytes"]) == case["expected_write_bytes"], "Closed-form write total mismatch")
            require(int(summary["completed_read_bytes"]) == case["expected_read_bytes"], "Closed-form read total mismatch")
            save(folder / "checks.json", checked)
            result["status"] = "passed"
        except Exception as exc:
            result.update(status="failed", error=str(exc))
            (folder / "failure.txt").write_text(traceback.format_exc(), encoding="utf-8")
        save(output / "runs.json", results)
        print(f"{case['name']}: {result['status']}", flush=True)
    passed = sum(r["status"] == "passed" for r in results)
    print(f"{passed}/{len(results)} cases passed; each case ran trace and RTL. Evidence: {output}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
