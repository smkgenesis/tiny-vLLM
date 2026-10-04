# Standard RTL wrapper

Copy `tiny_vllm_target.sv`, retain its top module and ports, and replace its body with your DUT integration. The bundled body is only a runnable descriptor acknowledger with a 1 MiB address bound.

Set CMake's `TINY_VLLM_ENABLE_TARGET=ON` and `TINY_VLLM_TARGET_SOURCES` to your source list. No C++ changes are required. Follow [the complete tutorial](../../docs/bring-your-own-rtl.md) and [the completion contract](../../docs/backend-interface.md).
