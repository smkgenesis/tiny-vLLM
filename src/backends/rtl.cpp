#include "tiny_vllm/rtl_backend.hpp"
#include "Vsingle_engine.h"
#include "Vsplit_rw.h"
#include <verilated.h>
#include <verilated_vcd_c.h>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <variant>
#include <sstream>
#include <iomanip>

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
                throw Error("config_error", "RTL config fields must be scalars");
            const auto key = item.first.Scalar(), text = item.second.Scalar();
            if (!seen.insert(key).second) throw Error("config_error", "Duplicate RTL field: " + key);
            if (key == "architecture") { result.architecture = text == "split-rw" ? "dual-rw" : text; continue; }
            U64 value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
                throw Error("config_error", "Invalid or duplicate RTL field: " + key);
            if (key == "fifo_depth") result.fifo_depth = value;
            else if (key == "service_cycles") result.service_cycles = value;
            else if (key == "initial_request_pause_cycles") result.initial_request_pause_cycles = value;
            else if (key == "trace_waveform") result.trace_waveform = value;
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
    root["architecture"] = c.architecture;
    root["trace_waveform"] = c.trace_waveform;
    return YAML::Dump(root) + '\n';
}

struct RtlBackend::Impl {
    VerilatedContext context;
    std::variant<std::unique_ptr<Vsingle_engine>, std::unique_ptr<Vsplit_rw>> model;
    std::unique_ptr<VerilatedVcdC> waveform;
    template<class F> void with_dut(F&& f) { std::visit([&](auto& p) { f(*p); }, model); }
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
    U64 idle{}, read_only{}, write_only{}, overlap{}, read_wait{}, write_wait{};
    U64 read_requests{}, write_requests{}, read_queue_sum{}, write_queue_sum{}, read_peak{}, write_peak{};

    Impl(const Config& c, const RtlConfig& rc, const std::filesystem::path& path, const std::string& f)
        : config(rc), fault(f), output(path), cycles(file(path / "rtl_cycles.csv")),
          handshakes(file(path / "rtl_handshakes.csv")) {
        if (fault != "none" && fault != "address" && fault != "drop-completion" && fault != "duplicate-completion")
            throw Error("config_error", "Unknown test fault: " + fault);
        if ((config.architecture != "single" && config.architecture != "dual-rw") || config.trace_waveform > 1 ||
            !config.fifo_depth || config.fifo_depth > 64 || !config.service_cycles ||
            config.service_cycles > std::numeric_limits<std::uint32_t>::max() ||
            (config.architecture == "dual-rw" && config.fifo_depth % 2))
            throw Error("config_error", "architecture must be single or dual-rw; dual-rw needs even total fifo_depth 2..64; trace_waveform must be 0 or 1");
        context.traceEverOn(bool(config.trace_waveform));
        if (config.architecture == "dual-rw") model = std::make_unique<Vsplit_rw>(&context);
        else model = std::make_unique<Vsingle_engine>(&context);
        cycles << "edge,req_valid,req_ready,req_id,req_address,req_bytes,req_write,cmp_valid,cmp_ready,cmp_id,cmp_error,queue_count,service_start,service_id,read_busy,write_busy,read_completion_wait,write_completion_wait,read_queue_count,write_queue_count,read_service_start,read_service_id,write_service_start,write_service_id\n";
        handshakes << "event,edge,transaction_id,address,bytes,write,error,latency_cycles\n";
        with_dut([&](auto& dut) {
        if (config.trace_waveform) {
            waveform = std::make_unique<VerilatedVcdC>();
            dut.trace(waveform.get(), 8);
            waveform->open((path / "rtl.vcd").string().c_str());
        }
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
        });
    }
    ~Impl() { with_dut([](auto& dut) { dut.final(); }); }
    void clock() {
        with_dut([&](auto& dut) {
        dut.clk = 0; dut.eval();
        if (waveform) waveform->dump(context.time());
        context.timeInc(1);
        dut.clk = 1; dut.eval();
        if (waveform) waveform->dump(context.time());
        context.timeInc(1);
        dut.clk = 0; dut.eval();
        });
    }
    void fail(const std::string& message) { if (error.empty()) error = message; }
    void tick() {
        with_dut([&](auto& dut) {
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
               << dut.queue_count << ',' << +dut.service_start << ',' << dut.service_id << ','
               << +dut.read_busy << ',' << +dut.write_busy << ',' << +dut.read_completion_wait << ',' << +dut.write_completion_wait << ','
               << dut.read_queue_count << ',' << dut.write_queue_count << ',' << +dut.read_service_start << ',' << dut.read_service_id << ','
               << +dut.write_service_start << ',' << dut.write_service_id << '\n';
        if (dut.read_busy && dut.write_busy) ++overlap;
        else if (dut.read_busy) ++read_only;
        else if (dut.write_busy) ++write_only;
        else ++idle;
        read_wait += dut.read_completion_wait; write_wait += dut.write_completion_wait;
        read_queue_sum += dut.read_queue_count; write_queue_sum += dut.write_queue_count;
        read_peak = std::max(read_peak, U64(dut.read_queue_count));
        write_peak = std::max(write_peak, U64(dut.write_queue_count));
        if (dut.req_valid && !dut.req_ready) ++stalls;
        peak_queue = std::max(peak_queue, U64(dut.queue_count));
        if (req) {
            if (dut.req_write) ++write_requests; else ++read_requests;
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
        });
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
    auto& s = *impl_;
    s.with_dut([&](auto& dut) {
    if (s.staging || !s.inflight.empty() || !s.completed.empty() || dut.queue_count ||
        dut.busy || dut.cmp_valid || s.frontend_count != s.requests || s.requests != s.valid_completions)
        throw Error("protocol_error", "RTL/adapter still live after frontend completion");
    });
}
void RtlBackend::finish(const std::string& status) {
    auto& s = *impl_;
    s.cycles.flush(); s.handshakes.flush();
    if (s.waveform) s.waveform->flush();
    s.with_dut([&](auto& dut) {
    auto out = file(s.output / "rtl_summary.csv");
    out << "status,frontend_accepted,request_handshakes,completion_handshakes,valid_completions,request_stall_cycles,peak_queue_count,first_request_edge,last_completion_edge,completion_span_cycles,latency_sum_cycles,latency_max_cycles,final_staging,final_inflight,final_queue_count,final_service_busy,observed_cycles,idle_cycles,read_only_cycles,write_only_cycles,overlap_cycles,read_completion_wait_cycles,write_completion_wait_cycles,read_requests,write_requests,read_queue_sum,write_queue_sum,read_peak_queue_count,write_peak_queue_count\n";
    out << status << ',' << s.frontend_count << ',' << s.requests << ',' << s.completions << ',' << s.valid_completions << ','
        << s.stalls << ',' << s.peak_queue << ',';
    if (s.requests) out << s.first_request;
    out << ',';
    if (s.completions) out << s.last_completion;
    out << ',';
    if (status == "ok" && s.requests && s.completions) out << s.last_completion - s.first_request;
    out << ',' << s.latency_sum << ',' << s.latency_max << ',' << bool(s.staging) << ',' << s.inflight.size() << ',' << dut.queue_count << ',' << +dut.busy
        << ',' << s.edge << ',' << s.idle << ',' << s.read_only << ',' << s.write_only << ',' << s.overlap << ',' << s.read_wait << ',' << s.write_wait
        << ',' << s.read_requests << ',' << s.write_requests << ',' << s.read_queue_sum << ',' << s.write_queue_sum << ',' << s.read_peak << ',' << s.write_peak << '\n';
    out.close();
    });
}
std::string RtlBackend::console_summary() const {
    const auto& s = *impl_;
    const auto busy = s.read_only + s.write_only + s.overlap;
    std::ostringstream out;
    out << "Completion span: " << s.last_completion - s.first_request << " RTL cycles\n"
        << "Peak queue: " << s.peak_queue << " waiting descriptors\n"
        << "R/W overlap: " << std::fixed << std::setprecision(2) << (busy ? 100.0 * s.overlap / busy : 0.0) << "% of busy cycles\n"
        << "Request stalls: " << s.stalls << " cycles\n";
    if (s.waveform) out << "Waveform: " << (s.output / "rtl.vcd").string() << '\n';
    return out.str();
}
}
