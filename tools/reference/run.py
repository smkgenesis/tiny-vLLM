"""A bounded, declared comparison matrix; failures remain in separate case folders."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys

import yaml


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vllm-source", type=Path, required=True)
    parser.add_argument("--tiny-executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[2]
    base = yaml.safe_load((root / "examples/concurrent.yaml").read_text(encoding="utf-8"))
    cases = {}
    cases["single"] = yaml.safe_load((root / "examples/basic.yaml").read_text(encoding="utf-8"))
    cases["mixed"] = copy.deepcopy(base)
    cases["active-one"] = copy.deepcopy(base)
    cases["active-one"]["scheduler"]["max_active_requests"] = 1
    cases["budget-one"] = copy.deepcopy(base)
    cases["budget-one"]["scheduler"]["max_tokens_per_step"] = 1
    cases["chunk-one"] = copy.deepcopy(base)
    cases["chunk-one"]["scheduler"]["prefill_chunk_tokens"] = 1
    cases["arrival-ties-and-gap"] = copy.deepcopy(base)
    cases["arrival-ties-and-gap"]["requests"] = [
        dict(id="future", arrival_step=10, prompt_tokens=1, output_tokens=1),
        dict(id="A", arrival_step=0, prompt_tokens=1, output_tokens=1),
        dict(id="B", arrival_step=0, prompt_tokens=5, output_tokens=2)]
    cases["longer-mixed"] = copy.deepcopy(base)
    cases["longer-mixed"]["scheduler"].update(max_tokens_per_step=8, prefill_chunk_tokens=5, max_active_requests=3)
    cases["longer-mixed"]["requests"] = [
        dict(id="A", arrival_step=0, prompt_tokens=64, output_tokens=8),
        dict(id="B", arrival_step=2, prompt_tokens=17, output_tokens=3),
        dict(id="C", arrival_step=2, prompt_tokens=4, output_tokens=1)]
    cases["concurrent-chunk-one"] = copy.deepcopy(base)
    cases["concurrent-chunk-one"]["scheduler"]["prefill_chunk_tokens"] = 1
    cases["concurrent-chunk-one"]["requests"] = [
        dict(id="A", arrival_step=0, prompt_tokens=5, output_tokens=3),
        dict(id="B", arrival_step=0, prompt_tokens=5, output_tokens=3)]
    cases["concurrent-long"] = copy.deepcopy(base)
    cases["concurrent-long"]["scheduler"].update(max_tokens_per_step=8, prefill_chunk_tokens=3)
    cases["concurrent-long"]["requests"] = [
        dict(id="A", arrival_step=0, prompt_tokens=64, output_tokens=8),
        dict(id="B", arrival_step=0, prompt_tokens=64, output_tokens=8)]
    # Final reservations must all fit: this matrix isolates common scheduler semantics.
    for config in cases.values():
        config["kv_cache"]["pool_blocks"] = 64
    configs = args.output / "configs"
    configs.mkdir()
    manifest = dict(cases=list(cases), negative_control="mixed output count +1", results=[])
    manifest_path = args.output / "suite.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    for name, config in cases.items():
        (configs / f"{name}.yaml").write_text(yaml.safe_dump(config, sort_keys=False), encoding="utf-8")
    for name in [*cases, "negative-control"]:
        config_name = "mixed" if name == "negative-control" else name
        command = [sys.executable, str(root / "tools/reference/compare.py"),
                   "--vllm-source", str(args.vllm_source.resolve()),
                   "--tiny-executable", str(args.tiny_executable.resolve()),
                   "--config", str(configs / f"{config_name}.yaml"), "--output", str(args.output / name)]
        if name == "negative-control":
            command.append("--inject-tiny-output-error")
        with (args.output / f"{name}.log").open("w", encoding="utf-8") as log:
            try:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=120)
                report = json.loads((args.output / name / "comparison.json").read_text(encoding="utf-8"))
                expected = "mismatch" if name == "negative-control" else "matched"
                passed = report["status"] == expected and result.returncode == (1 if name == "negative-control" else 0)
                row = dict(case=name, status=report["status"], passed=passed,
                           compared_rows=report.get("compared_rows"), returncode=result.returncode)
            except Exception as error:
                row = dict(case=name, status="error", passed=False, error=str(error))
        manifest["results"].append(row)
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(row), flush=True)
    return 0 if all(row["passed"] for row in manifest["results"]) else 1


if __name__ == "__main__":
    sys.exit(main())
