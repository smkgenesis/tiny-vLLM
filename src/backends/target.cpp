#include "tiny_vllm/target_backend.hpp"
#include "Vtiny_vllm_target.h"
#include <verilated.h>
#include <verilated_vcd_c.h>
#include <fstream>
#include <map>
#include <sstream>

namespace tiny_vllm {
namespace {
std::ofstream file(const std::filesystem::path& path) {
    std::ofstream stream(path);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    return stream;
}
}
struct TargetBackend::Impl {
    VerilatedContext context;
    std::unique_ptr<Vtiny_vllm_target> dut;
    std::unique_ptr<VerilatedVcdC> waveform;
    std::filesystem::path output;
    std::ofstream cycles;
    std::optional<MemoryRequest> staging;
    std::map<U64, MemoryRequest> inflight;
    std::deque<U64> completed;
    U64 edge{}, accepted{}, requests{}, completions{}, stalls{}, first{}, last{};
    std::string error;

    Impl(const std::filesystem::path& path, bool trace) : output(path), cycles(file(path / "target_cycles.csv")) {
        context.traceEverOn(trace);
        dut = std::make_unique<Vtiny_vllm_target>(&context);
        if (trace) {
            waveform = std::make_unique<VerilatedVcdC>();
            dut->trace(waveform.get(), 8);
            waveform->open((path / "waveform.vcd").string().c_str());
            if (!waveform->isOpen()) throw Error("output_error", "Cannot open waveform.vcd");
        }
        cycles << "edge,req_valid,req_ready,req_id,req_address,req_bytes,req_write,cmp_valid,cmp_ready,cmp_id,cmp_error\n";
        dut->clk = 0; dut->reset = 1; dut->req_valid = 0; dut->cmp_ready = 0;
        clock();
        dut->reset = 0; dut->cmp_ready = 1; dut->eval();
    }
    ~Impl() { dut->final(); }
    void clock() {
        dut->clk = 0; dut->eval();
        if (waveform) waveform->dump(context.time());
        context.timeInc(1);
        dut->clk = 1; dut->eval();
        if (waveform) waveform->dump(context.time());
        context.timeInc(1);
        dut->clk = 0; dut->eval();
    }
    void fail(const std::string& message) { if (error.empty()) error = message; }
    void tick() {
        ++edge;
        auto& d = *dut;
        d.req_valid = staging.has_value();
        d.req_id = staging ? staging->id : 0;
        d.req_address = staging ? staging->address : 0;
        d.req_bytes = staging ? staging->bytes : 0;
        d.req_write = staging && staging->write;
        d.eval();
        const bool req = d.req_valid && d.req_ready;
        const bool cmp = d.cmp_valid && d.cmp_ready;
        cycles << edge << ',' << +d.req_valid << ',' << +d.req_ready << ',' << d.req_id << ',' << d.req_address << ','
               << d.req_bytes << ',' << +d.req_write << ',' << +d.cmp_valid << ',' << +d.cmp_ready << ',' << d.cmp_id << ',' << +d.cmp_error << '\n';
        if (d.req_valid && !d.req_ready) ++stalls;
        if (req) {
            if (requests++ == 0) first = edge;
            if (!inflight.emplace(d.req_id, *staging).second) fail("Duplicate target request ID");
        }
        // Same-edge acceptance/completion is permitted by the standard contract.
        if (cmp) {
            ++completions; last = edge;
            if (!inflight.erase(d.cmp_id)) fail("Unknown or duplicate target completion ID " + std::to_string(d.cmp_id));
            else if (d.cmp_error) fail("Target rejected transaction " + std::to_string(d.cmp_id));
            else completed.push_back(d.cmp_id);
        }
        clock();
        if (req) staging.reset();
    }
};
TargetBackend::TargetBackend(const std::filesystem::path& output, bool waveform)
    : impl_(std::make_unique<Impl>(output, waveform)) {}
TargetBackend::~TargetBackend() = default;
bool TargetBackend::try_submit(const MemoryRequest& request) {
    if (impl_->staging) return false;
    impl_->staging = request; ++impl_->accepted; return true;
}
void TargetBackend::tick() { impl_->tick(); }
std::optional<U64> TargetBackend::poll_completion() {
    if (!impl_->error.empty()) throw Error("protocol_error", impl_->error);
    if (impl_->completed.empty()) return std::nullopt;
    const auto id = impl_->completed.front(); impl_->completed.pop_front(); return id;
}
void TargetBackend::verify_drained() const {
    const auto& s = *impl_;
    if (s.staging || !s.inflight.empty() || !s.completed.empty() || s.dut->cmp_valid ||
        s.accepted != s.requests || s.requests != s.completions)
        throw Error("protocol_error", "Target/adapter still live after frontend completion");
}
void TargetBackend::finish(const std::string& status) {
    auto& s = *impl_;
    s.cycles.flush();
    if (s.waveform) s.waveform->flush();
    auto out = file(s.output / "target_summary.csv");
    out << "status,frontend_accepted,request_handshakes,completion_handshakes,request_stall_cycles,first_request_edge,last_completion_edge,completion_span_cycles,observed_cycles,final_staging,final_inflight\n";
    out << status << ',' << s.accepted << ',' << s.requests << ',' << s.completions << ',' << s.stalls << ',';
    if (s.requests) out << s.first;
    out << ',';
    if (s.completions) out << s.last;
    out << ',';
    if (status == "ok" && s.requests && s.completions) out << s.last - s.first;
    out << ',' << s.edge << ',' << bool(s.staging) << ',' << s.inflight.size() << '\n';
}
std::string TargetBackend::console_summary() const {
    const auto& s = *impl_;
    std::ostringstream out;
    out << "Completion span: " << s.last - s.first << " RTL cycles\nRequest stalls: " << s.stalls << " cycles\n"
        << "Queue occupancy / R/W overlap: not exposed by standard ports\n";
    if (s.waveform) out << "Waveform: " << (s.output / "waveform.vcd").string() << '\n';
    return out.str();
}
}
