#include "tiny_vllm/core.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <deque>
#include <limits>
#include <map>
#include <tuple>

using namespace tiny_vllm;
namespace {
Config small() {
    Config c;
    c.num_layers = 1; c.kv_heads = 1; c.head_dim = 2; c.dtype_bytes = 2;
    c.max_tokens_per_step = 3; c.prefill_chunk_tokens = 3; c.max_active_requests = 1;
    c.block_tokens = 4; c.pool_blocks = 2; c.transaction_bytes = 16; c.max_outstanding = 4;
    c.max_backend_cycles = 10000;
    c.requests = {{"A", 0, 5, 3}};
    return c;
}
struct MemorySink : Sink {
    std::vector<Event> events;
    std::vector<TransactionEvent> transactions;
    void event(const Event& e) override { events.push_back(e); }
    void transaction(const TransactionEvent& e) override { transactions.push_back(e); }
};
Stats trace(const Config& c, MemorySink& sink) {
    Stats stats; TraceBackend backend; run(c, backend, sink, stats); return stats;
}

// Independent transport fixture: can reject, delay, reorder or corrupt completions.
struct TestBackend : Backend {
    U64 cycle{}, delay{3}, reject_until{2};
    bool immediate{}, duplicate{}, drop{};
    std::deque<std::pair<U64, U64>> pending;
    std::deque<U64> ready;
    bool try_submit(const MemoryRequest& r) override {
        if (cycle < reject_until) return false;
        if (immediate) ready.push_back(r.id);
        else pending.emplace_back(cycle + delay, r.id);
        return true;
    }
    void tick() override {
        ++cycle;
        // All pending work completes together at selected boundaries, reversed.
        if (!drop && cycle % delay == 0) {
            while (!pending.empty() && pending.back().first <= cycle) {
                const auto id = pending.back().second;
                pending.pop_back(); ready.push_back(id);
                if (duplicate) ready.push_back(id);
            }
        }
    }
    std::optional<U64> poll_completion() override {
        if (ready.empty()) return std::nullopt;
        auto id = ready.front(); ready.pop_front(); return id;
    }
};
}

TEST_CASE("single request matches independently enumerated addresses and token steps") {
    MemorySink sink;
    const auto s = trace(small(), sink);
    using Row = std::tuple<U64, bool, U64, U64>;
    const std::vector<Row> expected{
        {0,true,0,12}, {0,true,16,12},
        {1,true,12,4}, {1,true,32,4}, {1,true,28,4}, {1,true,48,4},
        {2,true,36,4}, {2,true,52,4}, {2,false,0,16}, {2,false,32,8}, {2,false,16,16}, {2,false,48,8},
        {3,true,40,4}, {3,true,56,4}, {3,false,0,16}, {3,false,32,12}, {3,false,16,16}, {3,false,48,12}};
    std::vector<Row> actual;
    for (const auto& e : sink.transactions) if (e.action == "completed")
        actual.emplace_back(e.step, e.request.write, e.request.address, e.request.bytes);
    REQUIRE(actual == expected);
    std::vector<std::tuple<U64,U64,U64>> commits;
    for (const auto& e : sink.events) if (e.kind == "committed") {
        commits.emplace_back(e.token_begin, e.token_end, e.state.outputs);
        REQUIRE(e.state.outstanding == 0);
    }
    const std::vector<std::tuple<U64,U64,U64>> expected_commits{{0,3,0},{3,5,1},{5,6,2},{6,7,3}};
    REQUIRE(commits == expected_commits);
    REQUIRE(s.write_bytes == 56); REQUIRE(s.read_bytes == 104);
    REQUIRE(s.generated == 18); REQUIRE(s.accepted == 18); REQUIRE(s.completed == 18);
    REQUIRE(s.peak_reserved_bytes == 64); REQUIRE(s.peak_allocated_bytes == 64);
    REQUIRE(s.peak_valid_kv_bytes == 56);
    REQUIRE(s.allocated_bytes == 0); REQUIRE(s.reserved_bytes == 0); REQUIRE(s.valid_kv_bytes == 0);
    REQUIRE(sink.events.back().kind == "released");
    REQUIRE(sink.events.back().state.outstanding == 0);
}

TEST_CASE("one output and block boundaries do not add decode or read unused tails") {
    for (U64 prompt : {3,4,5}) {
        auto c = small(); c.requests[0].prompt_tokens = prompt; c.requests[0].output_tokens = 1;
        c.requests[0].arrival_step = 7;
        MemorySink sink; const auto s = trace(c, sink);
        REQUIRE(s.write_bytes == prompt * 8); REQUIRE(s.read_bytes == 0);
        REQUIRE(s.outputs == 1); REQUIRE(s.computed_tokens == prompt);
        REQUIRE(s.peak_allocated_bytes == (prompt == 5 ? 64 : 32));
        REQUIRE(s.admission_step == 7);
        REQUIRE(s.completion_step == (prompt == 3 ? 7 : 8));
    }
}

TEST_CASE("chunk and transaction limits change splitting without changing logical bytes") {
    auto c = small(); c.prefill_chunk_tokens = 1; c.transaction_bytes = 4;
    MemorySink sink; const auto s = trace(c, sink);
    REQUIRE(s.serving_steps == 7); REQUIRE(s.completed == 40);
    REQUIRE(s.write_bytes == 56); REQUIRE(s.read_bytes == 104);
    for (const auto& e : sink.transactions) {
        REQUIRE(e.request.bytes <= 4);
        REQUIRE(e.request.address / 4 == (e.request.address + e.request.bytes - 1) / 4);
    }
}

TEST_CASE("layer and head layout stays within independent expected byte regions") {
    auto c = small(); c.num_layers = 2; c.kv_heads = 2; c.head_dim = 1;
    c.block_tokens = 2; c.requests[0].prompt_tokens = 3; c.requests[0].output_tokens = 2;
    MemorySink sink; const auto s = trace(c, sink);
    REQUIRE(s.write_bytes == 64); REQUIRE(s.read_bytes == 64);
    REQUIRE(s.peak_allocated_bytes == 64);
    // Each byte of the two fully populated bundles is written once.
    std::vector<unsigned> writes(64), reads(64);
    for (const auto& e : sink.transactions) if (e.action == "completed") {
        REQUIRE(e.request.address + e.request.bytes <= 64);
        for (U64 byte = e.request.address; byte < e.request.address + e.request.bytes; ++byte) {
            REQUIRE((byte % 32) / 16 == e.layer);
            ++(e.request.write ? writes : reads)[byte];
        }
    }
    REQUIRE(std::all_of(writes.begin(), writes.end(), [](auto n) { return n == 1; }));
    REQUIRE(std::all_of(reads.begin(), reads.end(), [](auto n) { return n == 1; }));
}

TEST_CASE("rejection and reordered delayed completions preserve phase barriers") {
    auto c = small(); c.max_outstanding = 2;
    TestBackend backend; MemorySink sink; Stats s;
    run(c, backend, sink, s);
    REQUIRE(s.write_bytes == 56); REQUIRE(s.read_bytes == 104);
    REQUIRE(s.generated == 18); REQUIRE(s.accepted == 18); REQUIRE(s.completed == 18);
    REQUIRE(s.peak_outstanding <= c.max_outstanding);
    REQUIRE(s.peak_outstanding > 1);
    U64 pending_writes = 0;
    bool saw_reordering = false;
    U64 last_id = 0;
    for (const auto& e : sink.transactions) {
        if (e.action == "accepted" && e.request.write) ++pending_writes;
        if (e.action == "accepted" && !e.request.write) REQUIRE(pending_writes == 0);
        if (e.action == "completed") {
            saw_reordering |= e.request.id < last_id;
            last_id = e.request.id;
            if (e.request.write) --pending_writes;
        }
    }
    REQUIRE(saw_reordering);
    REQUIRE(s.outstanding == 0);
}

TEST_CASE("immediate completion is supported without an extra token generation") {
    auto c = small(); TestBackend backend; backend.immediate = true; backend.reject_until = 0;
    MemorySink sink; Stats s; run(c, backend, sink, s);
    REQUIRE(s.outputs == 3); REQUIRE(s.generated == 18); REQUIRE(s.completed == 18);
    REQUIRE(s.outstanding == 0);
}

TEST_CASE("lost and duplicate completions fail without committing or releasing live KV") {
    for (bool duplicate : {false, true}) {
        auto c = small(); c.max_backend_cycles = 12;
        TestBackend backend; backend.duplicate = duplicate; backend.drop = !duplicate;
        MemorySink sink; Stats s;
        try { run(c, backend, sink, s); FAIL("Corrupt backend incorrectly succeeded"); }
        catch (const Error& error) { REQUIRE(error.status == (duplicate ? "protocol_error" : "timeout")); }
        REQUIRE(s.computed_tokens == 0); REQUIRE(s.outputs == 0);
        REQUIRE(s.allocated_bytes == 32); REQUIRE(s.reserved_bytes == 64);
        REQUIRE(std::none_of(sink.events.begin(), sink.events.end(),
                            [](const auto& e) { return e.kind == "released"; }));
    }
}

TEST_CASE("invalid sizes and overflow are rejected before execution") {
    auto c = small(); c.pool_blocks = 1; REQUIRE_THROWS_AS(validate(c), Error);
    c = small(); c.requests[0].output_tokens = 0; REQUIRE_THROWS_AS(validate(c), Error);
    c = small(); c.transaction_bytes = 3; REQUIRE_THROWS_AS(validate(c), Error);
    c = small(); c.head_dim = std::numeric_limits<U64>::max(); REQUIRE_THROWS_AS(validate(c), Error);
    c = small(); c.requests[0].arrival_step = std::numeric_limits<U64>::max(); REQUIRE_THROWS_AS(validate(c), Error);
}

TEST_CASE("mixed requests match the predefined per-request token schedule and interleave") {
    auto c = small(); c.pool_blocks = 3; c.max_active_requests = 2;
    c.requests.push_back({"B", 1, 2, 2});
    MemorySink sink; const auto s = trace(c, sink);
    using Row = std::tuple<std::string,U64,U64,U64,U64>;
    std::vector<Row> actual;
    std::map<U64,U64> scheduled;
    for (const auto& e : sink.events) {
        if (e.kind == "committed") actual.emplace_back(e.request_id,e.step,e.token_begin,e.token_end,e.request_outputs);
        if (e.kind == "scheduled") scheduled[e.step] += e.token_end - e.token_begin;
    }
    const std::vector<Row> expected{{"A",0,0,3,0},{"A",1,3,5,1},{"B",1,0,1,0},
        {"A",2,5,6,2},{"B",2,1,2,1},{"A",3,6,7,3},{"B",3,2,3,2}};
    REQUIRE(actual == expected);
    for (const auto& [step, tokens] : scheduled) { (void)step; REQUIRE(tokens <= 3); }
    REQUIRE(s.write_bytes == 80); REQUIRE(s.read_bytes == 128); REQUIRE(s.completed == 26);
    REQUIRE(s.peak_allocated_bytes == 96); REQUIRE(s.peak_reserved_bytes == 96);
    REQUIRE(s.peak_valid_kv_bytes == 80); REQUIRE(s.completed_requests == 2);
    REQUIRE(s.computed_tokens == 10); REQUIRE(s.outputs == 5); REQUIRE(s.serving_steps == 4);
    REQUIRE(s.active_requests == 0); REQUIRE(s.allocated_bytes == 0); REQUIRE(s.reserved_bytes == 0);
    std::vector<std::string> order;
    for (const auto& e : sink.transactions) if (e.action == "accepted" && e.step == 1) order.push_back(e.request_id);
    REQUIRE(order == std::vector<std::string>{"A","B","A","B","A","A"});
}

TEST_CASE("reservation pressure delays admission and reuses completed blocks") {
    auto c = small(); c.max_active_requests = 2; c.requests.push_back({"B",1,2,2});
    MemorySink sink; const auto s = trace(c, sink);
    REQUIRE(s.write_bytes == 80); REQUIRE(s.read_bytes == 128); REQUIRE(s.completed == 24);
    REQUIRE(s.serving_steps == 6); REQUIRE(s.completion_step == 5);
    REQUIRE(s.peak_allocated_bytes == 64); REQUIRE(s.peak_reserved_bytes == 64);
    std::vector<U64> blocked;
    bool reused = false;
    for (const auto& e : sink.events) if (e.request_id == "B") {
        if (e.kind == "blocked") { REQUIRE(e.reason == "reservation"); blocked.push_back(e.step); }
        if (e.kind == "admitted") REQUIRE(e.step == 4);
        if (e.kind == "allocated") { REQUIRE(e.block_table == std::vector<U64>{0}); reused = true; }
    }
    REQUIRE(blocked == std::vector<U64>{1,2,3}); REQUIRE(reused);
}

TEST_CASE("active and token limits are effective and FCFS does not bypass a blocked head") {
    auto c = small(); c.pool_blocks = 3; c.requests.push_back({"B",1,2,2});
    MemorySink first; auto s = trace(c, first);
    REQUIRE(s.peak_active_requests == 1); REQUIRE(s.completion_step == 5);
    c.max_active_requests = 2; c.max_tokens_per_step = 1;
    MemorySink second; s = trace(c, second);
    REQUIRE(s.serving_steps == 10);
    for (const auto& e : second.events) if (e.kind == "scheduled") REQUIRE(e.token_end - e.token_begin == 1);

    c = small(); c.max_active_requests = 3;
    c.requests = {{"A",0,1,3},{"B",0,5,1},{"C",0,1,1}};
    MemorySink third; s = trace(c, third);
    std::vector<std::pair<std::string,U64>> admissions;
    for (const auto& e : third.events) if (e.kind == "admitted") admissions.emplace_back(e.request_id,e.step);
    REQUIRE(admissions == std::vector<std::pair<std::string,U64>>{{"A",0},{"B",3},{"C",5}});
    REQUIRE(s.completed_requests == 3);
}

TEST_CASE("noncontiguous recycled mappings and unsorted arrivals remain correct") {
    auto c = small(); c.pool_blocks = 3; c.max_active_requests = 2;
    c.requests = {{"future",10,1,1},{"A",0,1,1},{"B",0,5,2}};
    MemorySink sink; const auto s = trace(c, sink);
    bool mapping_seen = false;
    for (const auto& e : sink.events) {
        if (e.kind == "committed" && e.request_id == "B" && e.step == 1) {
            REQUIRE(e.block_table == std::vector<U64>{1,0}); mapping_seen = true;
        }
        if (e.kind == "admitted" && e.request_id == "future") REQUIRE(e.step == 10);
    }
    REQUIRE(mapping_seen); REQUIRE(s.write_bytes == 64); REQUIRE(s.read_bytes == 48);
    REQUIRE(s.completion_step == 10); REQUIRE(s.serving_steps == 4);
    std::vector<std::pair<U64,U64>> decode_reads;
    for (const auto& e : sink.transactions)
        if (e.action == "completed" && e.request_id == "B" && !e.request.write)
            decode_reads.emplace_back(e.request.address,e.request.bytes);
    REQUIRE(decode_reads == std::vector<std::pair<U64,U64>>{{32,16},{0,8},{48,16},{16,8}});
}

TEST_CASE("multiple delayed requests preserve per-layer dependencies and the step barrier") {
    auto c = small(); c.pool_blocks = 3; c.max_active_requests = 2; c.max_outstanding = 2;
    c.num_layers = 2; c.requests.push_back({"B",1,2,2});
    TestBackend backend; MemorySink sink; Stats s; run(c,backend,sink,s);
    REQUIRE(s.write_bytes == 160); REQUIRE(s.read_bytes == 256); REQUIRE(s.completed_requests == 2);
    std::map<std::pair<std::string,U64>,U64> pending;
    for (const auto& e : sink.transactions) {
        auto& count = pending[{e.request_id,e.layer}];
        if (e.action == "accepted") {
            if (e.layer) REQUIRE(pending[std::make_pair(e.request_id,e.layer-1)] == 0);
            if (!e.request.write) {
                // Reads may overlap other reads; all writes of this phase must have completed.
                for (const auto& p : sink.transactions) {
                    if (p.action == "accepted" && p.step == e.step && p.request_id == e.request_id &&
                        p.layer == e.layer && p.request.write) {
                        const auto completion = std::find_if(sink.transactions.begin(), sink.transactions.end(),
                            [&](const auto& q) { return q.action == "completed" && q.request.id == p.request.id; });
                        REQUIRE(completion != sink.transactions.end()); REQUIRE(completion->cycle <= e.cycle);
                    }
                }
            }
            ++count;
        }
        if (e.action == "completed") { REQUIRE(count > 0); --count; }
    }
    for (const auto& e : sink.events) if (e.kind == "committed" || e.kind == "released") REQUIRE(e.state.outstanding == 0);
    REQUIRE(s.peak_outstanding == 2);
}
