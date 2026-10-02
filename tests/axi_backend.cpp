#include "tiny_vllm/axi_backend.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <set>

using namespace tiny_vllm;
namespace {
Config geometry() {
    Config c;
    c.num_layers=1; c.kv_heads=1; c.head_dim=1024; c.dtype_bytes=2;
    c.block_tokens=4; c.pool_blocks=4;
    return c;
}
std::filesystem::path folder() {
    auto p=std::filesystem::temp_directory_path()/std::filesystem::path("tiny-axi-direct-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(p); return p;
}
void wait_for(AxiBackend& b, std::set<U64> ids) {
    for (unsigned n=0; n<20000 && !ids.empty(); ++n) {
        b.tick();
        while (auto id=b.poll_completion()) REQUIRE(ids.erase(*id)==1);
    }
    REQUIRE(ids.empty());
    b.verify_drained();
}
}
TEST_CASE("External AXI preserves odd bytes, 4KiB splits and 64-bit IDs") {
    AxiBackend b(geometry(), AxiConfig{256,7,4},folder());
    const U64 id=1ULL<<40;
    REQUIRE(b.try_submit({id,4093,8197,true}));
    wait_for(b,{id});
    REQUIRE(b.try_submit({id+1,4093,8197,false}));
    wait_for(b,{id+1});
    REQUIRE(b.try_submit({id+2,4101,2,true}));
    wait_for(b,{id+2});
    REQUIRE(b.try_submit({id+3,4093,8197,false}));
    wait_for(b,{id+3});
    b.finish("ok");
}
TEST_CASE("External AXI read and write lanes overlap without premature completion") {
    AxiBackend b(geometry(), AxiConfig{2,7,4},folder());
    REQUIRE(b.try_submit({1,0,64,true})); wait_for(b,{1});
    REQUIRE(b.try_submit({2,1024,128,true}));
    REQUIRE_FALSE(b.try_submit({3,2048,8,true}));
    REQUIRE(b.try_submit({4,0,64,false}));
    wait_for(b,{2,4});
    REQUIRE(b.try_submit({5,1024,128,false})); wait_for(b,{5});
    b.finish("ok");
}
TEST_CASE("External AXI refuses out-of-arena descriptors and unwritten reads") {
    AxiBackend b(geometry(), AxiConfig{},folder());
    REQUIRE_THROWS_AS(b.try_submit({1,65535,2,true}),Error);
    REQUIRE(b.try_submit({2,0,8,false}));
    REQUIRE_THROWS_AS(wait_for(b,{2}),Error);
}
