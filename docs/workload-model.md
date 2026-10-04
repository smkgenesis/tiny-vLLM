# Workload model

Start with [basic.yaml](../examples/basic.yaml). All examples are synthetic length profiles. They do not contain tokens, weights, or measurements from a deployed service.

## Choose and edit an example

| Example | Purpose |
|---|---|
| `basic.yaml` | One small request with independently checked addresses |
| `concurrent.yaml` | Two requests with staggered logical arrivals |
| `kv_pressure.yaml` | The same requests with insufficient space for simultaneous reservation |
| `short_chat.yaml` | Four short prompt/output pairs |
| `mixed_chat.yaml` | Unequal prompt/output lengths and logical arrivals |
| `long_context.yaml` | One longer context with repeated decode scans |
| `case_study.yaml` | Exact mixed C4 input from the architecture study |

Copy an example and edit `requests` plus `scheduler.max_active_requests` first:

```yaml
requests:
  - {id: A, arrival_step: 0, prompt_tokens: 128, output_tokens: 32}
  - {id: B, arrival_step: 1, prompt_tokens: 512, output_tokens: 8}
```

This is a section of a workload, not a complete file. Geometry, KV pool, token budgets, and driver limits remain explicit in the copied preset so a missing field cannot silently change an experiment. Only `max_backend_cycles` has a default (10,000,000). `resolved_config.yaml` always records the full executed configuration. Unknown/duplicate keys, duplicate IDs, nonpositive sizes, overflow, and a single request larger than the pool are rejected.

## Serving progress

- `arrival_step` is a logical scheduler step, independent of backend time. Same-step arrivals follow input order.
- Running requests get budget first; waiting requests are admitted FCFS. A head request that cannot reserve space is not bypassed.
- `max_tokens_per_step` is the shared budget. `prefill_chunk_tokens` caps each prefill chunk. Decode computes one new token per scheduled request.
- Each request reserves enough blocks for its final KV length, `P + O - 1`. Computing the last prompt token produces the first output, so `O=1` adds no decode read.
- Physical blocks are allocated as computation reaches them. Reservation includes allocation; they are not additive memory counters.
- All work in a serving step must complete before commit and release. Returned blocks can be reused in the next step.

`max_active_requests` limits serving requests. `driver.max_outstanding` independently limits accepted, uncompleted **memory** requests. A larger outstanding limit does not enlarge a target queue.

## Access policy and dependencies

The policy is `prefill_store_decode_scan`. Prefill writes new KV but omits attention reads. Decode writes new KV, waits for that request/layer's writes to complete, then scans the valid context including the new token. The next layer waits for the current layer. Different serving requests may progress independently within a step.

The geometry models identical full-causal-attention layers with unquantized KV. `kv_heads` expresses the KV storage shape for MHA/GQA/MQA. Prefix sharing, preemption, sliding windows, MLA, speculative decoding, and distributed serving are outside scope.

## Addresses and independent byte totals

Layout: `[physical block bundle][layer][K/V][KV head][token][dimension]`. Transactions split at block/layout boundaries and the `transaction_bytes` limit, without padding traffic. This is not the vLLM GPU layout or GPU DRAM traffic.

For `L` layers, `H` KV heads, dimension `D`, and dtype size `S`, token storage is `T = 2LHDS` bytes:

```text
Write bytes = T × Σ(P + O − 1)
Read bytes  = T × Σ((O − 1)P + O(O − 1)/2)
```

These formulas independently check the chosen access policy. They do not estimate real inference traffic. A lower prefill chunk changes transaction boundaries and scheduling steps but need not change byte totals.

The supplied interface template has a 1 MiB illustrative address bound. All presets fit within it. A custom target must define and check its own capacity; the standard interface does not negotiate memory size.
