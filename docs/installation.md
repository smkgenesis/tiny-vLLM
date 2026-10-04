# Installation

Run the commands in an Ubuntu 24.04 or WSL2 Ubuntu shell. Native Windows builds have not been validated. Python is used for result checks, not for the C++ workload frontend.

## Dependencies

```bash
sudo apt-get update
sudo apt-get install -y g++ cmake ninja-build libyaml-cpp-dev catch2 verilator python3 python3-venv git
```

Validated versions: GCC 13.3, CMake 3.28.3, Ninja 1.11.1, yaml-cpp 0.8, Catch2 3.4, Verilator 5.020, Python 3.12.3. CMake enforces minimum versions for required libraries. The dependency-install step may need administrator access and network access.

## First build

From the repository root:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The preset builds trace, reference RTL, and the standard-port template. Binaries live in `$HOME/.cache/tiny-vllm/release`. Verilator 5.020 needs an ASCII build path. Different checkouts must use different build directories: override the preset with `cmake --preset release -B /your/ascii/path`, then use `cmake --build /your/ascii/path` and `ctest --test-dir /your/ascii/path`.

For trace only, use a separate directory:

```bash
cmake -S . -B "$HOME/.cache/tiny-vllm/trace" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$HOME/.cache/tiny-vllm/trace" --parallel 4
```

To build the external AXI integration too, add `-DTINY_VLLM_ENABLE_AXI=ON` to the release configure command. `-DBUILD_TESTING=OFF` removes the Catch2/Python test dependency. Plotting and the optional upstream vLLM comparison have separate environments in [validation](validation.md).

## Common setup failures

| Symptom | Action |
|---|---|
| `Could not find yaml-cpp/Catch2/verilator` | Install the packages above in the same Ubuntu environment used for CMake |
| CMake reports an old/different source directory | Use a new ASCII build directory; do not share a preset cache between checkouts |
| Output directory already exists (exit 4) | Choose another `--output`; existing evidence is never overwritten |
| `config_error` (exit 2) | Read `metadata.yaml`; start from a complete example and keep positive budgets |
| `timeout` (exit 3) | Inspect the last wire cycles; check reset, ready, completion ID and workload watchdog |

Next: [run the README example](../README.md#quick-start), then [connect a wrapper](bring-your-own-rtl.md).
