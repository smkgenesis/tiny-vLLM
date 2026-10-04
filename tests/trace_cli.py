"""Exercise the actual CLI and persisted artifacts; standard library only."""
import csv
import pathlib
import subprocess
import sys
import tempfile


def rows(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def main():
    executable, example = map(pathlib.Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="tiny-vllm-cli-") as temporary:
        root = pathlib.Path(temporary)

        def invoke(config, output, code):
            result = subprocess.run([str(executable), "--config", str(config), "--output", str(output)],
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == code, (result.returncode, result.stdout, result.stderr)

        result = root / "small"
        invoke(example, result, 0)
        summary = rows(result / "summary.csv")[0]
        for key, value in {"status": "ok", "completed_write_bytes": "56", "completed_read_bytes": "104",
                           "completed_transactions": "18", "computed_tokens": "7", "outputs": "3",
                           "final_outstanding": "0", "final_allocated_bytes": "0", "final_reserved_bytes": "0"}.items():
            assert summary[key] == value, (key, summary[key])
        transactions = rows(result / "transactions.csv")
        assert len(transactions) == 54
        completed = [r for r in transactions if r["event"] == "completed"]
        assert len({r["transaction_id"] for r in completed}) == 18
        assert sum(int(r["bytes"]) for r in completed if r["write"] == "1") == 56
        assert sum(int(r["bytes"]) for r in completed if r["write"] == "0") == 104
        commits = [r for r in rows(result / "events.csv") if r["event"] == "committed"]
        assert [(r["token_begin"], r["token_end"], r["outputs"]) for r in commits] == [
            ("0", "3", "0"), ("3", "5", "1"), ("5", "6", "2"), ("6", "7", "3")]
        # Existing evidence cannot be overwritten, including unsuccessful runs.
        original = (result / "summary.csv").read_bytes()
        invoke(example, result, 4)
        assert (result / "summary.csv").read_bytes() == original
        invoke(result / "resolved_config.yaml", root / "resolved-replay", 0)

        for case, peak, steps, transactions_count in [("mixed", "96", "4", "26"), ("pressure", "64", "6", "24")]:
            output = root / case
            invoke(example.parent / ({"mixed": "concurrent", "pressure": "kv_pressure"}[case] + ".yaml"), output, 0)
            measured = rows(output / "summary.csv")[0]
            for key, value in {"completed_write_bytes": "80", "completed_read_bytes": "128",
                               "peak_allocated_bytes": peak, "peak_reserved_bytes": peak,
                               "serving_steps": steps, "completed_transactions": transactions_count,
                               "completed_requests": "2", "final_active_requests": "0"}.items():
                assert measured[key] == value, (case, key, measured[key])
            events = rows(output / "events.csv")
            assert {e["request_id"] for e in events} == {"A", "B"}
            if case == "pressure":
                assert [(e["request_id"], e["step"]) for e in events if e["event"] == "admitted"] == [("A", "0"), ("B", "4")]
            invoke(output / "resolved_config.yaml", root / f"{case}-replay", 0)

        text = example.read_text(encoding="utf-8")
        bad_inputs = [
            text.replace("pool_blocks: 2", "pool_blocks: 1"),
            text.replace("output_tokens: 3", "output_tokens: 0"),
            text.replace("prompt_tokens: 5", "prompt_tokens: -5"),
            text.replace("head_dim: 2", "head_dim: 18446744073709551615"),
            text.replace("transaction_bytes: 16", "transaction_bytes: 3"),
            text.replace("pool_blocks: 2", "pool_blocks: 2\n  pool_blocks: 3"),
            text + "unsupported: true\n",
            text + "  - id: A\n    arrival_step: 0\n    prompt_tokens: 1\n    output_tokens: 1\n",
            text + "---\nother: document\n",
        ]
        for index, contents in enumerate(bad_inputs):
            config = root / f"bad-{index}.yaml"
            config.write_text(contents, encoding="utf-8")
            output = root / f"bad-{index}"
            invoke(config, output, 2)
            assert rows(output / "summary.csv")[0]["status"] == "config_error"
            assert (output / "input.yaml").read_bytes() == config.read_bytes()
        short = root / "timeout.yaml"
        short.write_text(text.replace("max_backend_cycles: 10000", "max_backend_cycles: 1"), encoding="utf-8")
        invoke(short, root / "timeout", 3)
        partial = rows(root / "timeout" / "summary.csv")[0]
        assert partial["status"] == "timeout"
        assert partial["outputs"] == "0"
        assert partial["completion_step"] == ""
        assert partial["final_allocated_bytes"] == "32"
    print("CLI artifacts, replay, invalid configurations, timeout and output preservation passed")


if __name__ == "__main__":
    main()
