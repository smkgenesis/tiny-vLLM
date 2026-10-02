#include <catch2/catch_test_macros.hpp>
#include <verilated.h>
#include "Vkv_service.h"
#include <vector>
#include <tuple>

namespace {
struct Fixture {
    VerilatedContext context;
    Vkv_service dut{&context};
    Fixture(unsigned depth = 1, unsigned service = 3) {
        dut.clk = 0; dut.reset = 1;
        dut.fifo_depth = depth; dut.service_cycles = service; dut.arena_bytes = 64;
        dut.allow_request = 1; dut.cmp_ready = 1; dut.req_bytes = 4;
        dut.eval(); tick(); dut.reset = 0; dut.eval();
    }
    ~Fixture() { dut.final(); }
    void tick() { dut.clk = 1; dut.eval(); dut.clk = 0; dut.eval(); }
};
}
TEST_CASE("FIFO full and completion backpressure preserve ID and service timing") {
    Fixture f;
    auto& d = f.dut;
    d.req_valid = 1; d.req_id = 11; d.eval();
    REQUIRE(d.req_ready); f.tick(); // request at edge 1
    REQUIRE(d.queue_count == 1);
    REQUIRE_FALSE(d.req_ready);
    REQUIRE(d.service_start); REQUIRE(d.service_id == 11);
    f.tick(); // service starts at edge 2
    d.req_id = 12; d.eval();
    REQUIRE(d.req_ready); f.tick(); // second request at edge 3
    d.req_valid = 0; d.eval();
    REQUIRE_FALSE(d.cmp_valid); f.tick(); // edge 4
    REQUIRE(d.cmp_valid); REQUIRE(d.cmp_id == 11); // eligible for edge 5 = 2+3
    d.cmp_ready = 0; d.eval();
    for (unsigned i = 0; i < 3; ++i) {
        REQUIRE_FALSE(d.service_start); f.tick(); // edges 5..7
        REQUIRE(d.cmp_valid); REQUIRE(d.cmp_id == 11); REQUIRE_FALSE(d.cmp_error);
        REQUIRE(d.queue_count == 1);
    }
    d.cmp_ready = 1; d.eval();
    REQUIRE(d.service_start); REQUIRE(d.service_id == 12);
    f.tick(); // first completion, second service start at edge 8
    REQUIRE_FALSE(d.cmp_valid); f.tick(); // edge 9
    REQUIRE_FALSE(d.cmp_valid); f.tick(); // edge 10
    REQUIRE(d.cmp_valid); REQUIRE(d.cmp_id == 12);
    f.tick(); // second completion at edge 11 = 8+3
    REQUIRE_FALSE(d.cmp_valid); REQUIRE(d.queue_count == 0);
}
TEST_CASE("continuous request and completion handshakes survive FIFO pointer wrap") {
    Fixture f(2, 1);
    auto& d = f.dut;
    unsigned next = 1, finished = 0, overlaps = 0;
    for (unsigned edge = 1; edge <= 400 && finished < 140; ++edge) {
        d.req_valid = next <= 140; d.req_id = next; d.eval();
        const bool req = d.req_valid && d.req_ready, cmp = d.cmp_valid && d.cmp_ready;
        if (cmp) { ++finished; REQUIRE(d.cmp_id == finished); REQUIRE_FALSE(d.cmp_error); }
        if (req && cmp) ++overlaps;
        if (req) ++next;
        f.tick();
        REQUIRE(d.queue_count <= 2);
    }
    REQUIRE(next == 141); REQUIRE(finished == 140); REQUIRE(overlaps > 100);
    REQUIRE(f.dut.queue_count == 0);
}
TEST_CASE("RTL checks zero length and exclusive arena boundary without overflow") {
    for (const auto [address, bytes, invalid] : std::vector<std::tuple<std::uint64_t, std::uint64_t, bool>>{
            {63, 1, false}, {63, 2, true}, {64, 1, true}, {0, 0, true}, {1, UINT64_MAX, true}}) {
        Fixture f(1, 1);
        auto& d = f.dut;
        d.req_valid = 1; d.req_id = 1; d.req_address = address; d.req_bytes = bytes; d.eval();
        f.tick(); d.req_valid = 0; d.eval(); f.tick();
        REQUIRE(d.cmp_valid); REQUIRE(bool(d.cmp_error) == invalid);
    }
}
