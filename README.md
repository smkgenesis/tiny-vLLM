# tiny-vLLM

**A lightweight, serving-aware workload frontend for early-stage LLM accelerator RTL evaluation.**

Build your RTL. tiny-vLLM provides the serving workload.

If you currently test an accelerator with fixed or manually constructed memory requests, tiny-vLLM provides the serving context: prompt/output lengths, concurrent requests, prefill/decode dependencies, and paged KV-cache lifetime.

```text
Workload YAML
     ↓
Serving frontend — Scheduler + Paged KV
     ↓
Memory requests
     ↓
Backend adapter → Your RTL
     ↑                │
     └── completion ──┘ → frontend progress
```

**tiny-vLLM is not an inference engine.** It does not execute Transformer kernels or predict GPU latency. It generates memory requests from simplified serving semantics and drives RTL through actual acceptance and completion events.

## Why tiny-vLLM?

You should not need to write an LLM scheduler and KV allocator to exercise a memory-facing RTL block. The frontend handles these tasks, holds requests through backpressure, and waits for completion before dependent reads, step commits, or KV release.

RTL timing changes submission and completion timing. Logical arrival steps and scheduling policy remain fixed; this is not a model of wall-clock arrivals or adaptive serving.

## Quick start

Use Ubuntu 24.04 or WSL2 Ubuntu. Install the [build dependencies](docs/installation.md) first. The preset uses an ASCII build path under `$HOME/.cache`.

**1. Get and build** (repository access is required while it is private):

```bash
git clone https://github.com/smkgenesis/tiny-vLLM.git
cd tiny-vLLM
cmake --preset release
cmake --build --preset release
```

**2. Run a reference target:**

```bash
"$HOME/.cache/tiny-vllm/release/tiny-vllm-rtl" \
  --config examples/basic.yaml --rtl-config examples/rtl.yaml --output results/basic
python3 tools/check_rtl_run.py results/basic
```

Expected: **4 serving steps, 18 completed memory requests, 56 write bytes, 104 read bytes, 65-cycle completion span**. Always choose a new output directory; failed runs are also preserved.

**3. Inspect:** the console reports the main counters. Open `summary.csv` for workload totals, `transactions.csv` for per-request traffic, and `rtl_summary.csv` for RTL timing. [Continue to the CSV and waveform tutorial](docs/bring-your-own-rtl.md).

## Bring your own RTL

Implement the request/completion ports in [`rtl/template/tiny_vllm_target.sv`](rtl/template/tiny_vllm_target.sv). The generic Verilator adapter does not require C++ changes or reference-engine instrumentation.

```bash
"$HOME/.cache/tiny-vllm/release/tiny-vllm-target" \
  --config examples/concurrent.yaml --output results/template --waveform
python3 tools/check_target_run.py results/template
```

This runs the bundled interface example. **It acknowledges descriptors; it is not a memory or accelerator implementation.** [Copy it, connect your DUT, and rebuild](docs/bring-your-own-rtl.md).

## Workloads and results

Start with `basic.yaml`, `concurrent.yaml`, or `kv_pressure.yaml`. Synthetic profiles `short_chat.yaml`, `mixed_chat.yaml`, and `long_context.yaml` offer larger length mixes. They are not collected traces or calibrated device workloads. [Edit a workload](docs/workload-model.md).

The first metrics to inspect are **completion span, accepted/completed memory requests, queue occupancy, read/write overlap, and request stalls**. Queue/engine metrics exist only for targets that expose them; the standard wrapper does not invent these values. [Definitions and advanced metrics](docs/metrics.md).

## Reference RTL targets

Two small targets support learning and regression testing:

| Target | CLI selection | Structure |
|---|---|---|
| Single-engine | `--rtl single` | Shared FIFO and one fixed-cost engine |
| Split read/write | `--rtl split-rw` | Separate read/write FIFOs and engines |

They are descriptor services, not final accelerators. The historical name `dual-rw` remains accepted and is retained in saved study configurations. For external-memory integration validation, the project also includes an unmodified AXI RAM target with real readback checks; see [validation](docs/validation.md).

## Case study

When does serving concurrency expose read/write overlap to independent RTL engines?

- One serving request exposes no read/write overlap under this access policy.
- Concurrent requests can expose independence; limiting outstanding memory requests to one removes the gain.
- More serving concurrency does not guarantee more overlap.

> In the declared mixed C8 case, single/split completion spans are **4,119 / 3,756 cycles**. Total waiting capacity is matched, but split adds an engine. This is not an equal-area or GPU speedup claim.

[All 11 pairs, controls, waveforms, and limitations](docs/case-study.md).

## Scope and validation

| Models | Does not model |
|---|---|
| Logical request arrivals and prompt/output lengths | Real-time arrivals or end-to-end inference latency |
| Prefill/decode progression and active-request limits | Transformer compute, CUDA, or GPU execution |
| Paged KV allocation, dependencies, and release | Full vLLM behavior, prefix caching, or preemption |
| Memory requests, backpressure, and actual completion | Real DRAM timing, area, power, or numeric KV values |

The full build has **27 automated tests** covering the core, reference RTL, generic template, and external AXI integration. The default preset has 23; AXI is optional. [`tools/reproduce.py`](docs/validation.md#reproduce-everything) builds and checks the complete artifact, including the unchanged research study.

## Documentation

Read in this order: [overview and glossary](docs/overview.md) → [workload model](docs/workload-model.md) → [bring your own RTL](docs/bring-your-own-rtl.md) → [backend contract](docs/backend-interface.md) → [validation](docs/validation.md) → [case study](docs/case-study.md).

The onboarding goal is an unassisted first integration within 30 minutes after toolchain setup. This is a target, **not a measured user-study result**. A [student usability-test protocol](docs/usability.md) is ready for actual participants.

MIT licensed; see [LICENSE](LICENSE). Vendored AXI RTL retains its own [copyright and MIT notice](rtl/vendor/verilog-axi/COPYING).
