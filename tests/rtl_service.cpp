#include <catch2/catch_test_macros.hpp>
#include <verilated.h>
#include "Vsingle_engine.h"
#include "Vsplit_rw.h"
#include <deque>
#include <vector>
#include <tuple>

namespace {
template<class Model = Vsingle_engine> struct Fixture {
    VerilatedContext context;
    Model dut{&context};
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
TEST_CASE("dual engines progress independently and hold arbitration through backpressure") {
    Fixture<Vsplit_rw> f(4, 1);
    auto& d = f.dut;
    // Start the write first, with the completion sink stopped. A read becomes
    // ready later, but must not replace the already presented write response.
    d.cmp_ready = 0; d.req_valid = 1; d.req_write = 1; d.req_id = 11; d.eval(); f.tick();
    d.req_write = 0; d.req_id = 12; d.eval(); f.tick();
    d.req_valid = 0; d.eval();
    REQUIRE(d.cmp_valid); REQUIRE(d.cmp_id == 11);
    REQUIRE(d.read_service_start); f.tick();
    for (unsigned i = 0; i < 4; ++i) {
        REQUIRE(d.read_busy); REQUIRE(d.write_busy);
        REQUIRE(d.cmp_valid); REQUIRE(d.cmp_id == 11);
        REQUIRE(d.read_completion_wait); REQUIRE(d.write_completion_wait);
        f.tick();
    }
    // Queue another descriptor in each direction while both engines are held.
    d.req_valid = 1; d.req_write = 0; d.req_id = 13; d.eval(); f.tick();
    d.req_write = 1; d.req_id = 14; d.eval(); f.tick();
    d.req_valid = 0; d.cmp_ready = 1; d.eval();
    REQUIRE(d.cmp_id == 11); REQUIRE(d.write_service_start); f.tick();
    REQUIRE(d.cmp_id == 12); REQUIRE(d.read_service_start); f.tick();
    REQUIRE(d.cmp_id == 14); f.tick();
    REQUIRE(d.cmp_id == 13); f.tick();
    REQUIRE_FALSE(d.busy); REQUIRE(d.queue_count == 0); REQUIRE_FALSE(d.cmp_valid);
}

TEST_CASE("dual queues retain per-direction order and drain across pointer wrap") {
    Fixture<Vsplit_rw> f(4, 3);
    auto& d = f.dut;
    std::deque<unsigned> pending[2];
    unsigned next = 1, done = 0, overlap = 0, stalls = 0;
    for (unsigned edge = 1; edge <= 2500 && done < 300; ++edge) {
        d.req_valid = next <= 300; d.req_id = next; d.req_write = (next % 3) != 0;
        d.cmp_ready = (edge % 11) >= 3; d.eval();
        if (d.read_busy && d.write_busy) ++overlap;
        if (d.req_valid && !d.req_ready) ++stalls;
        if (d.cmp_valid && d.cmp_ready) {
            auto& q = pending[(d.cmp_id % 3) != 0];
            REQUIRE_FALSE(q.empty()); REQUIRE(q.front() == d.cmp_id); REQUIRE_FALSE(d.cmp_error);
            q.pop_front(); ++done;
        }
        if (d.req_valid && d.req_ready) { pending[d.req_write].push_back(next); ++next; }
        f.tick();
        REQUIRE(d.read_queue_count <= 2); REQUIRE(d.write_queue_count <= 2);
    }
    REQUIRE(done == 300); REQUIRE(next == 301); REQUIRE(overlap > 0); REQUIRE(stalls > 0);
    REQUIRE(pending[0].empty()); REQUIRE(pending[1].empty()); REQUIRE_FALSE(d.busy); REQUIRE(d.queue_count == 0);
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
