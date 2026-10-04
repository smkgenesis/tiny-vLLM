"""Fresh builds, real CLI checks, experiments and plots using only this source tree.

System build dependencies and a Python environment with tools/study/requirements.txt
must already be installed. Upstream vLLM comparison is a separate optional run.
All commands and failures remain in the new output directory.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import traceback
import shutil


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-root", type=Path, required=True, help="New ASCII path for CMake/Verilator")
    p.add_argument("--output", type=Path, required=True, help="New evidence directory")
    p.add_argument("--plot-python", type=Path, required=True)
    p.add_argument("--jobs", type=int, default=2)
    p.add_argument("--baseline", type=Path, help="Optional result directory for exact CSV/PNG regression comparison")
    args = p.parse_args()
    source = Path(__file__).resolve().parents[1]
    build, output = args.build_root.resolve(), args.output.resolve()
    # A venv's bin/python is often a symlink to the system interpreter. Resolving
    # that symlink discards the venv prefix and its installed plotting packages.
    plot_python = args.plot_python.absolute()
    if args.jobs < 1 or not str(build).isascii():
        p.error("Use jobs >= 1 and an ASCII build-root (Verilator 5.020 limitation)")
    if build.exists() or output.exists():
        p.error("build-root and output must both be new paths; nothing is overwritten")
    if not plot_python.is_file():
        p.error("Missing plot Python; install tools/study/requirements.txt in a venv first")
    build.mkdir(parents=True)
    output.mkdir(parents=True)
    (output / "logs").mkdir()
    record = dict(status="running", source=str(source), build=str(build),
                  scope="Fresh build/output paths on the installed host toolchain; source and plot Python supplied by caller. Not clean-OS or cross-platform certification.",
                  upstream_comparison="separate", commands=[])

    def persist():
        (output / "reproduction.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")

    def run(name, command, timeout=300):
        command = [str(v) for v in command]
        log = output / "logs" / f"{name}.log"
        item = dict(name=name, command=command, log=str(log.relative_to(output)), status="running")
        record["commands"].append(item)
        persist()
        print(name, flush=True)
        try:
            with log.open("w", encoding="utf-8") as stream:
                proc = subprocess.run(command, cwd=source, stdout=stream, stderr=subprocess.STDOUT, text=True, timeout=timeout)
            item.update(exit=proc.returncode, status="passed" if proc.returncode == 0 else "failed")
            persist()
            if proc.returncode:
                raise RuntimeError(f"{name} failed with exit {proc.returncode}; see {log}")
        except subprocess.TimeoutExpired:
            item.update(status="timeout")
            persist()
            raise

    try:
        run("compiler", ["g++", "--version"])
        run("cmake", ["cmake", "--version"])
        run("verilator", ["verilator", "--version"])
        run("python", [sys.executable, "--version"])
        run("plot-environment", [plot_python, "-m", "pip", "check"])
        for target, rtl in (("trace", "OFF"), ("rtl", "ON")):
            directory = build / target
            run(f"configure-{target}", ["cmake", "-S", source, "-B", directory, "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", f"-DTINY_VLLM_ENABLE_RTL={rtl}",
                f"-DTINY_VLLM_ENABLE_AXI={rtl}", f"-DTINY_VLLM_ENABLE_TARGET={rtl}", "-DBUILD_TESTING=ON"])
            run(f"build-{target}", ["cmake", "--build", directory, "--parallel", args.jobs])
            run(f"test-{target}", ["ctest", "--test-dir", directory, "--output-on-failure"])
        trace, rtl = build / "trace" / "tiny-vllm-trace", build / "rtl" / "tiny-vllm-rtl"
        # The independent trace build participates in every transport comparison.
        run("protocol-cases", [sys.executable, source / "tests/rtl_cli.py", rtl, trace,
            source / "examples/basic.yaml", output / "protocol"])
        run("external-axi", [sys.executable, source / "tools/run_axi_validation.py",
            "--axi", build / "rtl" / "tiny-vllm-axi", "--trace", trace,
            "--examples", source / "examples", "--output", output / "external-axi"])
        run("standard-target", [sys.executable, source / "tests/target_cli.py", build / "rtl" / "tiny-vllm-target",
            trace, source / "examples", output / "standard-target"])
        # Exercise the documented custom-source route, not just the default template target.
        wrapper = output / "my_target.sv"
        shutil.copyfile(source / "rtl/template/tiny_vllm_target.sv", wrapper)
        run("configure-custom", ["cmake", "-S", source, "-B", build / "custom", "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF", "-DTINY_VLLM_ENABLE_TARGET=ON",
            f"-DTINY_VLLM_TARGET_SOURCES={wrapper}"])
        run("build-custom", ["cmake", "--build", build / "custom", "--parallel", args.jobs])
        run("custom-tutorial", [build / "custom" / "tiny-vllm-target", "--config", source / "examples/concurrent.yaml",
            "--output", output / "custom-tutorial", "--waveform"])
        run("check-custom-tutorial", [sys.executable, source / "tools/check_target_run.py", output / "custom-tutorial",
            "--trace", output / "standard-target/concurrent-trace"])
        run("experiments", [sys.executable, source / "tools/study/run.py", "--trace", trace,
            "--rtl", rtl, "--output", output / "experiments"])
        run("analysis", [sys.executable, source / "tools/study/analyze.py", output / "experiments",
            "--output", output / "analysis"])
        run("plots", [plot_python, source / "tools/study/plot.py", output / "analysis",
            "--output", output / "figures"])
        run("architecture", [plot_python, source / "tools/study/architecture.py", "--trace", trace,
            "--rtl", rtl, "--output", output / "architecture"])
        if args.baseline:
            baseline = args.baseline.resolve()
            previous = json.loads((baseline / "reproduction.json").read_text())
            if previous["status"] != "passed": raise RuntimeError("Baseline reproduction did not pass")
            comparisons = []
            for section in ("experiments", "external-axi", "architecture", "analysis", "figures"):
                files = sorted((baseline / section).rglob("*.csv"))
                files += sorted((baseline / section).rglob("*.png"))
                if not files: raise RuntimeError(f"Missing baseline evidence: {section}")
                for old in files:
                    relative = old.relative_to(baseline)
                    new = output / relative
                    equal = new.is_file() and old.read_bytes() == new.read_bytes()
                    comparisons.append(dict(path=str(relative), identical=equal))
            (output / "baseline-comparison.json").write_text(json.dumps(comparisons, indent=2) + "\n")
            if not all(r["identical"] for r in comparisons): raise RuntimeError("Baseline numerical/figure regression; see baseline-comparison.json")
        record["status"] = "passed"
    except Exception as exc:
        record.update(status="failed", error=str(exc))
        (output / "failure.txt").write_text(traceback.format_exc(), encoding="utf-8")
    persist()
    print(f"{record['status']}: {output / 'reproduction.json'}")
    return 0 if record["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
