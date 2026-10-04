#include "Vtiny_vllm_target.h"
#include "tiny_vllm/target_backend.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <verilated.h>

TEST_CASE("template holds 64-bit completion and error under response backpressure") {
    VerilatedContext context;
    Vtiny_vllm_target d{&context};
    const auto tick = [&] { d.clk = 0; d.eval(); d.clk = 1; d.eval(); d.clk = 0; d.eval(); };
    d.reset = 1; tick(); d.reset = 0;
    d.req_valid = 1; d.req_id = (1ULL << 42) + 7;
    d.req_address = 1048575; d.req_bytes = 2; d.req_write = 1; d.cmp_ready = 0; d.eval();
    REQUIRE(d.req_ready); tick();
    d.req_valid = 0;
    for (unsigned i = 0; i < 2; ++i) { REQUIRE_FALSE(d.cmp_valid); tick(); }
    for (unsigned i = 0; i < 4; ++i) {
        REQUIRE_FALSE(d.req_ready); REQUIRE(d.cmp_valid); REQUIRE(d.cmp_error);
        REQUIRE(d.cmp_id == (1ULL << 42) + 7); tick();
    }
    d.cmp_ready = 1; tick(); REQUIRE_FALSE(d.cmp_valid); REQUIRE(d.req_ready);
    d.final();
}

TEST_CASE("standard adapter preserves staging and reports real target errors") {
    using namespace tiny_vllm;
    auto path = std::filesystem::temp_directory_path() / ("tiny-target-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(path);
    TargetBackend b(path, false);
    const U64 id = (1ULL << 40) + 1;
    REQUIRE(b.try_submit({id, 0, 8, true}));
    REQUIRE_FALSE(b.try_submit({id + 1, 8, 8, false}));
    REQUIRE_THROWS_AS(b.verify_drained(), Error);
    for (unsigned edge = 1; edge <= 4; ++edge) {
        b.tick();
        const auto completion = b.poll_completion();
        if (edge < 4) REQUIRE_FALSE(completion);
        else REQUIRE(completion == id);
    }
    b.verify_drained();
    REQUIRE(b.try_submit({id + 1, 0, 0, false}));
    for (unsigned edge = 1; edge <= 3; ++edge) { b.tick(); REQUIRE_FALSE(b.poll_completion()); }
    b.tick(); REQUIRE_THROWS_AS(b.poll_completion(), Error);
    b.finish("protocol_error");
}
