# Bring your own RTL

**You do not need to implement an LLM scheduler, KV allocator, or workload generator. Your RTL only needs to accept memory requests and report completions.**

The standard Verilator adapter requires no C++ edits. First run the supplied template unchanged, then replace its body with a wrapper around your DUT. The template deliberately acknowledges descriptors after three cycles; it does not execute memory accesses or validate data.

Prerequisite: the [README quick start](../README.md#quick-start) passes in an Ubuntu/WSL shell. All commands below run from the repository root.

## 1. Copy the wrapper

```bash
mkdir -p build
cp rtl/template/tiny_vllm_target.sv build/my_target.sv
```

Keep the top module name `tiny_vllm_target` and the [standard ports](backend-interface.md#standard-rtl-ports). The file name can change. `build/` is ignored by Git; put your finished wrapper in your own source directory when ready.

## 2. Connect your DUT

For the first run, leave the body unchanged. When integrating the real block:

1. On `req_valid && req_ready`, latch the ID, address, byte count, and direction.
2. Translate the descriptor into your DUT's operations. If it cannot accept work, lower `req_ready`; the adapter retains the request.
3. Wait for **all** operations to finish, including write visibility needed by a subsequent read.
4. Present the original ID on `cmp_id` with `cmp_valid=1`. Hold ID/error until `cmp_ready` acknowledges it.
5. Report a rejected descriptor with `cmp_error=1`; never silently drop it.

Do not return completion when a command is merely issued. Do not retain the template's fixed counter as a substitute for your DUT's completion. A data interface also needs your wrapper to generate/check payloads. Use the [external AXI backend](backend-interface.md#external-axi-adapter) as a data-bearing integration example.

## 3. Build your target

```bash
cmake -S . -B "$HOME/.cache/tiny-vllm/my-target" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DTINY_VLLM_ENABLE_TARGET=ON \
  -DTINY_VLLM_TARGET_SOURCES="$PWD/build/my_target.sv"
cmake --build "$HOME/.cache/tiny-vllm/my-target" --parallel 4
```

For multiple files, pass a quoted semicolon-separated list: `-DTINY_VLLM_TARGET_SOURCES="/path/wrapper.sv;/path/dut.sv;/path/defs.svh"`. Sources and explicitly listed headers are copied into the ASCII build tree. Basenames must be unique; sibling includes are supported. Nested include trees, arbitrary vendor flags, and other build systems need a target-specific CMake integration.

`target-sources.txt` in the build directory records each staged file's SHA256. Its aggregate fingerprint appears in run metadata. Reconfigure/rebuild after source changes. Bundled template-specific tests are not applied to an arbitrary user target.

Custom-target lint permits file/module name differences (`DECLFILENAME`); other Verilator warnings remain enabled and fatal. A filename change does not change the required top-module name.

## 4. Run and check

```bash
"$HOME/.cache/tiny-vllm/my-target/tiny-vllm-target" \
  --config examples/concurrent.yaml --output results/my-target --waveform
python3 tools/check_target_run.py results/my-target
```

With the unchanged template, expect 2 serving requests, 26 memory transactions, 80 write bytes, and 128 read bytes. Your DUT may change cycle counts, not these logical byte totals for this input. Choose a new output path for each run.

For transport comparison, run `tiny-vllm-trace` with the same workload and pass that output directory as the checker's `--trace`. This compares transport contents; it is not an independent proof of the shared workload generator.

## 5. Open the results

Read the two summaries without extra Python packages:

```bash
python3 -c 'import csv; print(next(csv.DictReader(open("results/my-target/summary.csv"))))'
python3 -c 'import csv; print(next(csv.DictReader(open("results/my-target/target_summary.csv"))))'
```

`transactions.csv` links serving-request IDs to memory transaction IDs and generated/accepted/completed times. `target_cycles.csv` records actual wrapper pins. `waveform.vcd` is created only with `--waveform`.

If GTKWave is installed (`sudo apt-get install gtkwave`), run:

```bash
gtkwave results/my-target/waveform.vcd
```

Add these signals first: `clk`, `reset`, `req_valid`, `req_ready`, `req_id`, `cmp_valid`, `cmp_ready`, `cmp_id`, `cmp_error`. Inspect a request rising-edge handshake and follow its ID to completion. A stalled request must keep its payload stable. VCD edge/time mapping is in the [contract](backend-interface.md#standard-rtl-ports).

For the reference split target, use `--rtl split-rw --rtl-config examples/dual.yaml`; that config enables `rtl.vcd`. Add `read_busy`, `write_busy`, `read_queue_count`, and `write_queue_count` to see internal overlap. **These extra signals are not required from your wrapper.**

## If it stalls or fails

Check `metadata.yaml` first. A timeout usually needs inspection of the last request: was reset released, did ready go high, and was the same ID eventually completed? An error completion aborts the run. A wrong/duplicate ID is a protocol error. The watchdog is configured in the workload; increasing it cannot fix missing completions.

The standard checker verifies wire/ID accounting, stable stalled requests, dependency order, byte totals, and commit/release timing. It does not verify your internal computation, numeric data, physical memory capacity, or the truth of a completion signal. Add DUT-specific tests for those properties.
