#include "tiny_vllm/rtl_backend.hpp"
#include "Vkv_service.h"
#include <verilated.h>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <set>

namespace tiny_vllm {
namespace {
std::ofstream file(const std::filesystem::path& path) {
    std::ofstream out(path);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    return out;
}
}
RtlConfig load_rtl_config(const std::string& path) {
    try {
        const auto documents = YAML::LoadAllFromFile(path);
        if (documents.size() != 1 || !documents[0].IsMap())
            throw Error("config_error", "RTL config must be one mapping");
        RtlConfig result;
        std::set<std::string> seen;
        for (const auto& item : documents[0]) {
            if (!item.first.IsScalar() || !item.second.IsScalar())
                throw Error("config_error", "RTL config fields must be scalar integers");
            const auto key = item.first.Scalar(), text = item.second.Scalar();
            U64 value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (!seen.insert(key).second || error != std::errc{} || end != text.data() + text.size())
                throw Error("config_error", "Invalid or duplicate RTL field: " + key);
            if (key == "fifo_depth") result.fifo_depth = value;
            else if (key == "service_cycles") result.service_cycles = value;
            else if (key == "initial_request_pause_cycles") result.initial_request_pause_cycles = value;
            else throw Error("config_error", "Unknown RTL field: " + key);
        }
        if (!result.fifo_depth || result.fifo_depth > 64 || !result.service_cycles ||
            result.service_cycles > std::numeric_limits<std::uint32_t>::max())
            throw Error("config_error", "RTL fifo_depth must be 1..64; service_cycles must be 1..2^32-1");
        return result;
    } catch (const YAML::Exception& e) { throw Error("config_error", e.what()); }
}
std::string resolved_rtl_yaml(const RtlConfig& c) {
    YAML::Node root;
    root["fifo_depth"] = c.fifo_depth;
    root["service_cycles"] = c.service_cycles;
    root["initial_request_pause_cycles"] = c.initial_request_pause_cycles;
    return YAML::Dump(root) + '\n';
}

struct RtlBackend::Impl {
    VerilatedContext context;
    Vkv_service dut{&context};
    RtlConfig config;
    std::string fault, error;
    std::filesystem::path output;
    std::ofstream cycles, handshakes;
    std::optional<MemoryRequest> staging;
    std::deque<U64> completed;
    struct Flight { MemoryRequest request; U64 edge; };
    std::map<U64, Flight> inflight;
    U64 edge{}, frontend_count{}, requests{}, completions{}, stalls{}, peak_queue{};
    U64 first_request{}, last_completion{}, latency_sum{}, latency_max{}, valid_completions{};

    Impl(const Config& c, const RtlConfig& rc, const std::filesystem::path& path, const std::string& f)
        : config(rc), fault(f), output(path), cycles(file(path / "rtl_cycles.csv")),
          handshakes(file(path / "rtl_handshakes.csv")) {
        if (fault != "none" && fault != "address" && fault != "drop-completion" && fault != "duplicate-completion")
            throw Error("config_error", "Unknown test fault: " + fault);
        cycles << "edge,req_valid,req_ready,req_id,req_address,req_bytes,req_write,cmp_valid,cmp_ready,cmp_id,cmp_error,queue_count,service_start,service_id\n";
        handshakes << "event,edge,transaction_id,address,bytes,write,error,latency_cycles\n";
        dut.fifo_depth = config.fifo_depth;
        dut.service_cycles = config.service_cycles;
        dut.arena_bytes = 2 * c.num_layers * c.kv_heads * c.head_dim * c.dtype_bytes * c.block_tokens * c.pool_blocks;
        dut.test_fault = fault == "drop-completion" ? 2 : fault == "duplicate-completion" ? 3 : 0;
        dut.reset = 1;
        dut.clk = 0;
        dut.req_valid = 0;
        dut.cmp_ready = 0;
        dut.allow_request = 0;
        dut.eval();
        clock(); // Reset edge excluded from every workload edge/cycle metric.
        dut.reset = 0;
        dut.cmp_ready = 1;
        dut.eval();
    }
    ~Impl() { dut.final(); }
    void clock() {
        dut.clk = 0; dut.eval(); context.timeInc(1);
        dut.clk = 1; dut.eval(); context.timeInc(1);
        dut.clk = 0; dut.eval();
    }
    void fail(const std::string& message) { if (error.empty()) error = message; }
    void tick() {
        ++edge;
        dut.allow_request = edge > config.initial_request_pause_cycles;
        dut.req_valid = staging.has_value();
        dut.req_id = staging ? staging->id : 0;
        dut.req_address = staging ? staging->address : 0;
        dut.req_bytes = staging ? staging->bytes : 0;
        dut.req_write = staging && staging->write;
        if (staging && staging->id == 1 && fault == "address") dut.req_address = dut.arena_bytes;
        dut.eval();
        const bool req = dut.req_valid && dut.req_ready;
        const bool cmp = dut.cmp_valid && dut.cmp_ready;
        const U64 cmp_id = dut.cmp_id;
        const bool cmp_error = dut.cmp_error;
        cycles << edge << ',' << +dut.req_valid << ',' << +dut.req_ready << ',' << dut.req_id << ','
               << dut.req_address << ',' << dut.req_bytes << ',' << +dut.req_write << ','
               << +dut.cmp_valid << ',' << +dut.cmp_ready << ',' << cmp_id << ',' << cmp_error << ','
               << dut.queue_count << ',' << +dut.service_start << ',' << dut.service_id << '\n';
        if (dut.req_valid && !dut.req_ready) ++stalls;
        peak_queue = std::max(peak_queue, U64(dut.queue_count));
        if (req) {
            if (requests++ == 0) first_request = edge;
            const MemoryRequest wire{dut.req_id, dut.req_address, dut.req_bytes, bool(dut.req_write)};
            if (!inflight.emplace(wire.id, Flight{wire, edge}).second) fail("Duplicate RTL request ID");
            handshakes << "request," << edge << ',' << wire.id << ',' << wire.address << ',' << wire.bytes << ',' << wire.write << ",0,\n";
        }
        if (cmp) {
            ++completions;
            last_completion = edge;
            const auto it = inflight.find(cmp_id);
            handshakes << "completion," << edge << ',' << cmp_id << ',';
            if (it == inflight.end()) {
                handshakes << ",,," << cmp_error << ",\n";
                fail("Unknown or duplicate RTL completion ID " + std::to_string(cmp_id));
            } else {
                const auto& r = it->second.request;
                const auto latency = edge - it->second.edge;
                handshakes << r.address << ',' << r.bytes << ',' << r.write << ',' << cmp_error << ',' << latency << '\n';
                if (cmp_error) fail("RTL rejected address/length for transaction " + std::to_string(cmp_id));
                else {
                    ++valid_completions;
                    latency_sum += latency;
                    latency_max = std::max(latency_max, latency);
                    completed.push_back(cmp_id);
                }
                inflight.erase(it);
            }
        }
        clock();
        peak_queue = std::max(peak_queue, U64(dut.queue_count));
        if (req) staging.reset();
    }
};
RtlBackend::RtlBackend(const Config& c, const RtlConfig& r, const std::filesystem::path& path, const std::string& fault)
    : impl_(std::make_unique<Impl>(c, r, path, fault)) {}
RtlBackend::~RtlBackend() = default;
bool RtlBackend::try_submit(const MemoryRequest& request) {
    if (impl_->staging) return false;
    impl_->staging = request;
    ++impl_->frontend_count;
    return true;
}
void RtlBackend::tick() { impl_->tick(); }
std::optional<U64> RtlBackend::poll_completion() {
    if (!impl_->error.empty()) throw Error("protocol_error", impl_->error);
    if (impl_->completed.empty()) return std::nullopt;
    const auto id = impl_->completed.front(); impl_->completed.pop_front(); return id;
}
void RtlBackend::verify_drained() const {
    const auto& s = *impl_;
    if (s.staging || !s.inflight.empty() || !s.completed.empty() || s.dut.queue_count ||
        s.dut.busy || s.dut.cmp_valid || s.frontend_count != s.requests || s.requests != s.valid_completions)
        throw Error("protocol_error", "RTL/adapter still live after frontend completion");
}
void RtlBackend::finish(const std::string& status) {
    auto& s = *impl_;
    s.cycles.flush(); s.handshakes.flush();
    auto out = file(s.output / "rtl_summary.csv");
    out << "status,frontend_accepted,request_handshakes,completion_handshakes,valid_completions,request_stall_cycles,peak_queue_count,first_request_edge,last_completion_edge,completion_span_cycles,latency_sum_cycles,latency_max_cycles,final_staging,final_inflight,final_queue_count,final_service_busy\n";
    out << status << ',' << s.frontend_count << ',' << s.requests << ',' << s.completions << ',' << s.valid_completions << ','
        << s.stalls << ',' << s.peak_queue << ',';
    if (s.requests) out << s.first_request;
    out << ',';
    if (s.completions) out << s.last_completion;
    out << ',';
    if (status == "ok" && s.requests && s.completions) out << s.last_completion - s.first_request;
    out << ',' << s.latency_sum << ',' << s.latency_max << ',' << bool(s.staging) << ',' << s.inflight.size() << ',' << s.dut.queue_count << ',' << +s.dut.busy << '\n';
    out.close();
}
}
