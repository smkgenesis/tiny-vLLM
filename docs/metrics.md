# Outputs and metrics

Every run gets a new directory. Existing files are never overwritten. `metadata.yaml` gives status, error, backend, source revision at configure time, host elapsed time, and cycle semantics. Host elapsed seconds are not modeled hardware cycles.

## Start with these metrics

| Metric | Source | Meaning |
|---|---|---|
| Completion span | `rtl_summary.csv` or `target_summary.csv` | Last completion edge − first **RTL** request handshake edge; reset excluded |
| Accepted/completed memory requests | `summary.csv` | Adapter ownership transfers / IDs retired by the frontend |
| Queue occupancy | `rtl_summary.csv` | Waiting reference descriptors, excluding active engines |
| Read/write overlap | `rtl_summary.csv`, derived | `overlap_cycles / (read_only_cycles + write_only_cycles + overlap_cycles)` |
| Request stalls | Reference or target summary | Edges where request valid is high but ready is low |

The standard wrapper exposes no queue/engine instrumentation; occupancy and overlap are **unavailable**, not zero. `completed_requests` in the common summary counts serving requests; `completed_transactions` counts memory requests. Console output distinguishes them.

## Files

```text
run/
  input.yaml                 original workload, including invalid input
  resolved_config.yaml       full executed workload
  metadata.yaml              status and provenance
  summary.csv                workload counters and final resources
  events.csv                 scheduling, KV allocation/commit/release
  transactions.csv           generated/accepted/completed memory requests
  driver_waits.csv            frontend wait reasons
```

Target-specific detail remains separate:

| Backend | Additional files |
|---|---|
| Reference RTL | `rtl_input.yaml`, `resolved_rtl_config.yaml`, `rtl_cycles.csv`, `rtl_handshakes.csv`, `rtl_summary.csv`, optional `rtl.vcd` |
| Standard wrapper | `target_cycles.csv`, `target_summary.csv`, optional `waveform.vcd` |
| External AXI | RTL input/resolved config, `axi_cycles.csv`, `axi_summary.csv` |
| Trace | No physical RTL wire log; cycles are virtual |

For failures, summaries are partial. A failed run has no valid completion span or completion step. Exit codes: 0 success, 2 usage/configuration, 3 backend construction/protocol/runtime/timeout, 4 output-directory creation or final result-saving failure. Output creation errors cannot leave a complete metadata record.

## Advanced reference metrics

- `observed_cycles`: all post-reset edges, including idle edges. It is usually one greater than span when the first handshake is edge 1.
- `idle_cycles`, `read_only_cycles`, `write_only_cycles`, `overlap_cycles`: mutually exclusive states sampled before each edge; their sum is `observed_cycles`.
- Direction utilization: `(direction_only + overlap) / observed_cycles`.
- Busy means engine occupied, including waiting for completion transfer. Direction-specific `completion_wait_cycles` separates this wait from fixed-cost service.
- FIFO sums and peaks count waiting descriptors before edges. Mean queue occupancy is `queue_sum / observed_cycles`. The aggregate legacy peak also considers post-edge occupancy.
- Request latency: completion edge − that descriptor's RTL acceptance edge. It includes queue wait, service, and response wait.
- Driver `backend_staging` and `outstanding_limit` waits describe frontend blocking. They may overlap other stall metrics; do not add the categories to reconstruct span.

Use **completed** transaction rows for byte totals. Do not sum generated, accepted, and completed copies of one operation. Event token/output fields are per serving request; live allocation/outstanding fields are global. Reserved bytes include allocated bytes.

AXI `backend_cycles` covers all post-reset AXI edges and is not the reference completion-span boundary. AXI `read_write_overlap_cycles` measures simultaneous live adapter lanes, not reference-engine busy state. Do not directly compare these unlike metrics as speedups. [Case-study figures](case-study.md) use consistent reference boundaries.
