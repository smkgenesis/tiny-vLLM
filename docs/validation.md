# Validation and reproduction

Validated environment: Ubuntu 24.04.4 / WSL2, GCC 13.3, CMake 3.28.3, Ninja 1.11.1, yaml-cpp 0.8, Catch2 3.4, Verilator 5.020, Python 3.12.3. This is not certification of other OS/toolchain combinations.

## Automated checks

| Area | Evidence |
|---|---|
| Core | Independent address golden, scheduler/KV state, boundaries, block reuse, reordered completion, configuration errors |
| Reference RTL | FIFO order/wrap, service timing, response backpressure, split-engine progress/arbitration, invalid addresses |
| Standard wrapper | 64-bit IDs, held responses, real target error propagation, staging, trace comparison, VCD, replay, watchdog |
| External AXI | RAM readback, strobes, 64-bit IDs, 4 KiB and 256-beat splits, dependencies and lifetime |
| Negative controls | Missing/duplicate completions, corrupted readback, copied premature-release/busy/completion records |

The full configuration has **27 CTests**. The `release` preset has 23 (AXI off); trace-only has 14. Tests of the bundled template are disabled when custom RTL sources are selected. Passing the template tests is not a correctness certificate for a user's DUT.

Independent wire auditors compare logged pins against frontend transactions and check completion-dependent phase/commit/release ordering. Shared-generator trace comparison checks transport consistency. Separate small address goldens and closed-form byte totals check the modeled access policy. Neither is a proof over all workloads or full AXI4 compliance.

## External RTL integration

The vendored `alexforencich/verilog-axi` RAM is unmodified at commit `516bd5dadc3365b7f9e225d2af8fe0b8d804fe53`, with its MIT notice and source hash retained. It is a pinned validation target from a deprecated upstream repository, not a claim of general AXI support.

```bash
cmake --preset release -DTINY_VLLM_ENABLE_AXI=ON
cmake --build --preset release
"$HOME/.cache/tiny-vllm/release/tiny-vllm-axi" \
  --config examples/basic.yaml --rtl-config examples/axi.yaml --output results/axi-basic
python3 tools/check_axi_run.py results/axi-basic
```

| Example | Write / read bytes | Memory descriptors | Normal / response-paused cycles |
|---|---:|---:|---:|
| basic | 56 / 104 | 18 | 71 / 151 |
| concurrent | 80 / 128 | 26 | 97 / 207 |
| kv_pressure | 80 / 128 | 24 | 95 / 207 |

The AXI suite has nine normal executions (including one resolved replay), plus expected failures. Multilayer/head tests check 720/1152 B; large burst tests check 43036/79924 B. Odd addresses, IDs above 40 bits, partial overwrites, simultaneous lanes, strobe/data corruption, and live-resource failures are covered. Injected errors change adapter observations/delivery; they are not vendor design bugs. [Exact interface behavior](backend-interface.md#external-axi-adapter).

## Reproduce everything

Install [build dependencies](installation.md), then prepare the plotting environment once:

```bash
python3 -m venv "$HOME/.cache/tiny-vllm/plot-env"
"$HOME/.cache/tiny-vllm/plot-env/bin/python" -m pip install -r tools/study/requirements.txt
python3 tools/reproduce.py \
  --build-root "$HOME/.cache/tiny-vllm/reproduce-build" \
  --output results/reproduction \
  --plot-python "$HOME/.cache/tiny-vllm/plot-env/bin/python" --jobs 4
```

Both build/output paths must be new. The script builds trace plus all RTL adapters, runs CTest, preserves protocol failures, tests all six onboarding workloads through the template, builds a **copied custom source** through the tutorial route, and runs the original studies/plots. Commands, exits, and logs are in `reproduction.json` and `logs/`. Expected negative controls must fail with their declared exit code for the suite to pass.

`protocol/`, `external-axi/`, `standard-target/`, and `custom-tutorial/` contain integration evidence. `experiments/`, `analysis/`, and `figures/` retain the original 23-condition evaluation. `architecture/` contains 11 trace runs, 22 reference RTL runs, tables, PNG/SVG figures, and the mixed C4 VCDs.

The pre-onboarding artifact is preserved by tag **`kcs2027-freeze`**. To compare against a previous successful reproduction, add `--baseline /path/to/previous/reproduction`. The script compares every CSV/PNG under the original experiment, AXI, architecture, analysis, and figure directories and writes `baseline-comparison.json`. Any missing or changed file fails the run. Paths, host timing, metadata source revisions, and VCD hierarchy names are intentionally excluded.

The onboarding reorganization passed a fresh source-package reproduction on 2026-10-05: **570 CSV/PNG files matched the freeze evidence byte-for-byte**, and the copied-wrapper tutorial completed 26 transactions with 80 write / 128 read bytes. This is automated integration evidence, not a participant timing measurement.

## Studies and packaging

The original 23 conditions contain 19 unique configurations and four repeated pairs. FIFO depths 1/2/4 yield request stalls 100/64/10 with span 189/189/189 cycles. Changing waiting location does not necessarily change the single-engine bottleneck. The separate architecture study and its controls are described in [case-study.md](case-study.md); study plans and bytes remain unchanged by the onboarding reorganization.

```bash
python3 tools/study/run.py --trace /path/tiny-vllm-trace --rtl /path/tiny-vllm-rtl --output results/study
python3 tools/study/analyze.py results/study --output results/analysis
"$HOME/.cache/tiny-vllm/plot-env/bin/python" tools/study/plot.py results/analysis --output results/figures
python3 tools/package_source.py --output dist/tiny-vllm-source.zip
```

The source archive contains code, examples, tests, documentation, CMake presets, and licenses. It excludes Git, environments, build trees, and raw run directories. Raw evidence stays under ignored `results/`; published case-study CSVs/figures remain under `docs/`.

## Optional upstream vLLM comparison

The unchanged scheduler/allocator from [vLLM commit b558f160](https://github.com/vllm-project/vllm/tree/b558f160a2c0abcb5902acc3c91a14c38a4af173) is driven with synthetic `ModelRunnerOutput`. No weights or GPU kernels run. This Python environment is not required to use the frontend or RTL targets.

```bash
python3 -m venv "$HOME/.cache/tiny-vllm/reference-env"
"$HOME/.cache/tiny-vllm/reference-env/bin/python" -m pip install -r tools/reference/requirements.txt
git clone --filter=blob:none --no-checkout https://github.com/vllm-project/vllm.git "$HOME/.cache/tiny-vllm/vllm-reference"
git -C "$HOME/.cache/tiny-vllm/vllm-reference" fetch --depth 1 origin b558f160a2c0abcb5902acc3c91a14c38a4af173
git -C "$HOME/.cache/tiny-vllm/vllm-reference" checkout --detach b558f160a2c0abcb5902acc3c91a14c38a4af173
VLLM_TARGET_DEVICE=empty "$HOME/.cache/tiny-vllm/reference-env/bin/python" -m pip install \
  -e "$HOME/.cache/tiny-vllm/vllm-reference" --no-build-isolation --no-deps
"$HOME/.cache/tiny-vllm/reference-env/bin/python" tools/reference/run.py \
  --vllm-source "$HOME/.cache/tiny-vllm/vllm-reference" \
  --tiny-executable "$HOME/.cache/tiny-vllm/release/tiny-vllm-trace" --output results/reference
```

Reuse an existing environment/checkout if available. The recorded result is **7 matching conditions / 104 matching rows, and 2 chunk-policy differences** (`chunk-one`, `longer-mixed`). Upstream [disables the threshold for one eligible request](https://github.com/vllm-project/vllm/blob/b558f160a2c0abcb5902acc3c91a14c38a4af173/vllm/v1/core/sched/scheduler.py#L623-L640); tiny-vLLM always applies its chunk cap. The comparison suite intentionally returns **exit 1** for those differences. Pressure policy, physical addresses, and GPU traffic equality are outside this comparison.

## Human onboarding

The build/run/check tutorial is exercised automatically. No student participant study has yet been completed. Use the [prepared usability protocol](usability.md) before making claims about unassisted integration time or adoption.
