# Backend interface

The C++ contract is in [`backend.hpp`](../include/tiny_vllm/backend.hpp):

```cpp
class Backend {
public:
    virtual ~Backend() = default;
    virtual bool try_submit(const MemoryRequest&) = 0;
    virtual void tick() = 0;
    virtual std::optional<uint64_t> poll_completion() = 0;
};
```

`try_submit()` asks whether the **adapter** can own a request now. `tick()` advances one backend cycle. `poll_completion()` returns one completed ID, if available.

## Ownership and progress

`MemoryRequest` contains unsigned 64-bit `id`, byte `address`, byte count `bytes`, and a `write` flag. Core-generated IDs increase during a run; the string serving-request ID is a separate concept.

- Returning false leaves ownership with the frontend. It preserves the request for retry.
- Returning true transfers ownership to the adapter. It is **not** proof of RTL acceptance or completion.
- Each accepted ID must complete exactly once. Out-of-order completions are allowed. Unknown or duplicate IDs and target errors fail the run.
- A write completion must mean that all work needed before its dependent read is finished. Issuing an internal command is not sufficient.
- A read completion must cover the entire descriptor, including all split transfers.
- `max_backend_cycles` stops a non-progressing workload. Failure preserves partial counters and live frontend resources; it never commits unfinished work as successful.

## Standard RTL ports

The public wrapper contract is the top module [`tiny_vllm_target`](../rtl/template/tiny_vllm_target.sv). Only these ports are required:

| Port | Direction at DUT | Meaning |
|---|---|---|
| `clk`, `reset` | input | Rising-edge clock; active-high synchronous reset |
| `req_valid`, `req_ready` | input, output | Request transfers when both are high at a rising edge |
| `req_id[63:0]` | input | ID to return on completion |
| `req_address[63:0]`, `req_bytes[63:0]` | input | Byte-range descriptor |
| `req_write` | input | 1 for write, 0 for read |
| `cmp_valid`, `cmp_ready` | output, input | Completion transfers when both are high at a rising edge |
| `cmp_id[63:0]`, `cmp_error` | output | Completed ID and target-reported failure flag |

Payload must stay stable while valid is high and ready is low. The adapter has one staging slot and holds the request until the DUT handshake. It drives `cmp_ready=1` after reset. A wrapper must still obey the completion hold rule, as the direct template test checks. There is at most one request handshake and one completion handshake per cycle. Same-edge request acceptance and completion are permitted; duplicate IDs are not.

One reset edge precedes workload edge 1. The adapter records pins immediately before each workload rising edge. In a VCD, workload edge N is the rising edge at time `2N+1`; time 1 is reset. These are testbench ticks, not a measured device clock frequency. Runtime reset, multiple clocks, and asynchronous clock crossings are not supplied by this adapter.

The adapter observes accepted IDs, completion IDs, request stalls, and span. It cannot infer hidden FIFO occupancy, read/write engine activity, data correctness, or whether a DUT reported completion too early. The wrapper author must validate those properties against the real DUT. Completion accounting and deasserted `cmp_valid` are checked on exit; hidden internal state is not observable.

The interface carries no numeric data. A data-bearing DUT needs a wrapper or a target-specific C++ backend that supplies payloads and verifies them. The external AXI adapter is a worked example of the latter.

## Reference target controls

`tiny-vllm-rtl` uses a separate instrumentation-aware adapter for the bundled targets. `--rtl single|split-rw` overrides `architecture` in the RTL YAML. `dual-rw` is an accepted alias and the historical canonical value in resolved configurations and study CSVs.

`fifo_depth` is a runtime waiting-capacity limit, excluding active descriptors. Single supports 1..64. Split requires an even total 2..64 and assigns half to each direction. The physical arrays remain 64 entries per engine; matching runtime capacity does not match area.

`service_cycles` is the same fixed descriptor cost for reads/writes. A service started at edge S cannot complete before S+`service_cycles`. Completion backpressure keeps the engine occupied. A full queue refuses admission even when a dequeue is possible on that edge. A blocked staging request can delay the opposite direction.

Split uses round-robin completion arbitration, locking the selected response through stalls. It preserves FIFO order within each direction. Both engines can start in one cycle, so audits use direction-specific start/ID signals; the legacy aggregate `service_id` gives read priority.

`initial_request_pause_cycles` gates acceptance for initial workload edges. `trace_waveform: 1` enables `rtl.vcd` (default 0). These controls are reference-target features, not requirements for a custom DUT.

## External AXI adapter

[`src/backends/axi.cpp`](../src/backends/axi.cpp) drives unmodified [`axi_ram.v`](../rtl/vendor/verilog-axi/axi_ram.v), pinned at commit `516bd5dadc3365b7f9e225d2af8fe0b8d804fe53`. CMake verifies its source SHA256.

| Property | Contract |
|---|---|
| DUT | 64-bit data/ID, 20-bit byte address, 1 MiB, `PIPELINE_OUTPUT=0` |
| Concurrency | One read descriptor and one write descriptor; a full lane rejects another in that direction |
| Splitting | Aligned 1/2/4/8-byte transfers, INCR bursts, at most 256 beats, no 4 KiB crossing |
| Write completion | Every sub-burst B handshake has occurred |
| Read completion | Every sub-burst's final R handshake has occurred |
| Reset | Two reset edges before recording; no runtime reset |

WSTRB selects only requested bytes. The synthetic pattern is `(id*131 + address*17 + (address>>8)) mod 256`. Adapter shadow memory and an independent Python wire replay check readback and reject unwritten reads. These are not numeric KV values.

`max_burst_beats` is 1..256. Response-pause settings gate BREADY/RREADY as a test condition, not a DRAM model. This RAM reports OKAY responses and uses AWLEN rather than WLAST to determine write length; the adapter/checker separately check address bounds and master WLAST. `--inject-fault read-data|drop-completion` changes adapter observation/delivery, not vendor RTL.

See [metrics and output files](metrics.md) and [integration tutorial](bring-your-own-rtl.md).
