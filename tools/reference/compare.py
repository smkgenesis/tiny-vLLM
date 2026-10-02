"""Compare completed scheduler steps with an unmodified, pinned vLLM checkout.

No model weights, GPU kernels, or vLLM scheduler mocks are used. Synthetic model
outputs advance the real scheduler, following upstream scheduler unit tests.
"""
import argparse
import csv
import inspect
import importlib.metadata
import json
import os
from pathlib import Path
import subprocess
import sys
import traceback

PIN = "b558f160a2c0abcb5902acc3c91a14c38a4af173"


def read_csv(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def reference(config, source, output):
    os.environ["VLLM_TARGET_DEVICE"] = "cpu"
    os.environ["VLLM_USE_V2_MODEL_RUNNER"] = "0"
    sys.path.insert(0, str(source))
    import torch
    from transformers import OPTConfig
    from tests.v1.core.utils import create_requests
    from vllm.config import CacheConfig, ModelConfig, ParallelConfig, SchedulerConfig, VllmConfig
    from vllm.v1.core.sched.scheduler import Scheduler
    from vllm.v1.core.single_type_kv_cache_manager import register_all_kvcache_specs
    from vllm.v1.kv_cache_interface import FullAttentionSpec, KVCacheConfig, KVCacheGroupSpec
    from vllm.v1.outputs import ModelRunnerOutput
    from vllm.v1.structured_output import StructuredOutputManager

    scheduler_file = Path(inspect.getfile(Scheduler)).resolve()
    if scheduler_file != source / "vllm/v1/core/sched/scheduler.py":
        raise RuntimeError(f"Wrong Scheduler import: {scheduler_file}")
    # Local architecture config only: no tokenizer, model weights or network fetch.
    model_path = output / "model-config"
    max_length = max(r["prompt_tokens"] + r["output_tokens"] for r in config["requests"]) + 32
    OPTConfig(hidden_size=64, ffn_dim=128, num_hidden_layers=1, num_attention_heads=1,
              word_embed_proj_dim=64, vocab_size=128, max_position_embeddings=max_length).save_pretrained(model_path)
    block = config["kv_cache"]["block_tokens"]
    scheduling = config["scheduler"]
    model_config = ModelConfig(model=str(model_path), max_model_len=max_length,
                               dtype="float16", seed=0, skip_tokenizer_init=True)
    scheduler_config = SchedulerConfig(
        max_model_len=max_length,
        max_num_seqs=scheduling["max_active_requests"],
        max_num_active_seqs=scheduling["max_active_requests"],
        max_num_batched_tokens=max(scheduling["max_tokens_per_step"], scheduling["max_active_requests"]),
        max_num_scheduled_tokens=scheduling["max_tokens_per_step"],
        long_prefill_token_threshold=scheduling["prefill_chunk_tokens"],
        long_prefill_token_threshold_adaptive=False,
        enable_chunked_prefill=True, async_scheduling=False,
        is_encoder_decoder=False, watermark=0.0, policy="fcfs",
    )
    cache_config = CacheConfig(block_size=block, enable_prefix_caching=False)
    cache_config.num_gpu_blocks = config["kv_cache"]["pool_blocks"] + 1
    vllm_config = VllmConfig(model_config=model_config, scheduler_config=scheduler_config,
                             cache_config=cache_config, parallel_config=ParallelConfig())
    kv_config = KVCacheConfig(num_blocks=cache_config.num_gpu_blocks, kv_cache_tensors=[],
        kv_cache_groups=[KVCacheGroupSpec(["layer"], FullAttentionSpec(
            block_size=block, num_kv_heads=1, head_size=2, dtype=torch.float16))])
    register_all_kvcache_specs(vllm_config)
    scheduler = Scheduler(vllm_config=vllm_config, kv_cache_config=kv_config, block_size=block,
                          structured_output_manager=StructuredOutputManager(vllm_config), log_stats=False)
    settings = dict(max_num_batched_tokens=scheduler.scheduler_config.max_num_batched_tokens,
                    max_num_scheduled_tokens=scheduler.max_num_scheduled_tokens,
                    max_num_active_requests=scheduler.max_num_active_reqs,
                    long_prefill_token_threshold=scheduler.scheduler_config.long_prefill_token_threshold,
                    threshold_ignored_when_only_one_eligible_request=True)
    write_json(output / "upstream-settings.json", settings)
    if scheduler.max_num_scheduled_tokens != scheduling["max_tokens_per_step"]:
        raise RuntimeError("Effective upstream token budget differs from requested budget")
    initial_free = scheduler.kv_cache_manager.block_pool.get_num_free_blocks()
    requests = {}
    for row in config["requests"]:
        requests[row["id"]] = create_requests(
            num_requests=1, num_tokens=row["prompt_tokens"], max_tokens=row["output_tokens"],
            ignore_eos=True, block_size=block, req_ids=[row["id"]],
        )[0]
    arrivals = sorted(enumerate(config["requests"]), key=lambda item: (item[1]["arrival_step"], item[0]))
    next_arrival = 0
    step = arrivals[0][1]["arrival_step"]
    rows = []
    while not all(request.is_finished() for request in requests.values()):
        if step > max(r["arrival_step"] for r in config["requests"]) + sum(
                r["prompt_tokens"] + r["output_tokens"] for r in config["requests"]):
            raise RuntimeError("Reference made insufficient progress")
        while next_arrival < len(arrivals) and arrivals[next_arrival][1]["arrival_step"] <= step:
            row = arrivals[next_arrival][1]
            scheduler.add_request(requests[row["id"]])
            next_arrival += 1
        before = {rid: (r.num_computed_tokens, r.num_tokens) for rid, r in requests.items()}
        scheduled = scheduler.schedule()
        ids = list(scheduled.num_scheduled_tokens)
        block_counts = {rid: len(scheduler.kv_cache_manager.get_block_ids(rid)[0]) for rid in ids}
        outputs = [[1] if before[rid][0] + scheduled.num_scheduled_tokens[rid] == before[rid][1] else [] for rid in ids]
        scheduler.update_from_output(scheduled, ModelRunnerOutput(
            req_ids=ids, req_id_to_index={rid: i for i, rid in enumerate(ids)}, sampled_token_ids=outputs))
        for rid in ids:
            request = requests[rid]
            finished = request.is_finished()
            freed = not scheduler.kv_cache_manager.get_block_ids(rid)[0] if finished else False
            rows.append(dict(request_id=rid, step=step, begin=before[rid][0],
                             end=before[rid][0] + scheduled.num_scheduled_tokens[rid],
                             computed=request.num_computed_tokens, outputs=request.num_output_tokens,
                             logical_blocks=block_counts[rid], finished=finished, freed=freed))
        write_json(output / "upstream-steps.json", rows)
        step += 1
    final_free = scheduler.kv_cache_manager.block_pool.get_num_free_blocks()
    if final_free != initial_free:
        raise RuntimeError(f"Upstream leaked blocks: {initial_free} initially free, {final_free} finally free")
    return rows, str(scheduler_file), dict(initial_free_blocks=initial_free, final_free_blocks=final_free)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vllm-source", type=Path, required=True)
    parser.add_argument("--tiny-executable", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--inject-tiny-output-error", action="store_true",
                        help="Negative control: corrupt one observed output counter before comparison")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    report = dict(status="error", reference_commit=PIN, scope="scheduler completed-step state; ample pool only")
    try:
        source = args.vllm_source.resolve()
        actual = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        if actual != PIN:
            raise RuntimeError(f"Reference commit mismatch: {actual}")
        if subprocess.run(["git", "-C", str(source), "diff", "--quiet", "HEAD"]).returncode:
            raise RuntimeError("Reference has tracked changes")
        import yaml
        report["packages"] = {package: importlib.metadata.version(package)
                              for package in ("torch", "numpy", "transformers", "vllm")}
        config = yaml.safe_load(args.config.read_text(encoding="utf-8"))
        block = config["kv_cache"]["block_tokens"]
        required = sum((r["prompt_tokens"] + r["output_tokens"] - 2) // block + 1 for r in config["requests"])
        if config["kv_cache"]["pool_blocks"] < required:
            raise RuntimeError("Reference comparison requires enough KV for all requests; pressure policies differ")
        run = subprocess.run([str(args.tiny_executable.resolve()), "--config", str(args.config.resolve()),
                              "--output", str(args.output / "tiny")], capture_output=True, text=True)
        (args.output / "tiny-stdout.txt").write_text(run.stdout + run.stderr, encoding="utf-8")
        if run.returncode:
            raise RuntimeError(f"tiny-vLLM failed: {run.returncode}")
        events = read_csv(args.output / "tiny/events.csv")
        finished = {(e["request_id"], int(e["step"])) for e in events if e["event"] == "finished"}
        freed = {(e["request_id"], int(e["step"])) for e in events if e["event"] == "released" and e["logical_blocks"] == "0"}
        tiny = [dict(request_id=e["request_id"], step=int(e["step"]), begin=int(e["token_begin"]),
                     end=int(e["token_end"]), computed=int(e["computed_tokens"]), outputs=int(e["outputs"]),
                     logical_blocks=int(e["logical_blocks"]), finished=(e["request_id"], int(e["step"])) in finished,
                     freed=(e["request_id"], int(e["step"])) in freed)
                for e in events if e["event"] == "committed"]
        if args.inject_tiny_output_error:
            tiny[0]["outputs"] += 1
        report["injected_observation_error"] = args.inject_tiny_output_error
        write_json(args.output / "tiny-steps.json", tiny)
        upstream, imported_file, pool = reference(config, source, args.output)
        # Canonical order ignores different ordering of independent request records.
        key = lambda row: (row["step"], row["request_id"])
        tiny.sort(key=key); upstream.sort(key=key)
        report.update(status="matched" if tiny == upstream else "mismatch", imported_scheduler=imported_file,
                      compared_rows=len(tiny), upstream_rows=len(upstream), model_execution=False,
                      upstream_pool=pool,
                      note="Synthetic model outputs; physical IDs and hardware transactions are not compared.")
        if tiny != upstream:
            report["tiny"] = tiny; report["upstream"] = upstream
        code = 0 if tiny == upstream else 1
    except Exception as error:
        report["error"] = str(error)
        (args.output / "failure.txt").write_text(traceback.format_exc(), encoding="utf-8")
        code = 2
    write_json(args.output / "comparison.json", report)
    print(json.dumps(report, indent=2))
    return code


if __name__ == "__main__":
    sys.exit(main())
