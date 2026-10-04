#include "tiny_vllm/core.hpp"
#if defined(TINY_VLLM_RTL) || defined(TINY_VLLM_AXI)
#define TINY_VLLM_CONFIGURED_HARDWARE
#endif
#if defined(TINY_VLLM_CONFIGURED_HARDWARE) || defined(TINY_VLLM_TARGET)
#define TINY_VLLM_HARDWARE
#endif
#ifdef TINY_VLLM_TARGET
#include "tiny_vllm/target_backend.hpp"
#elif defined(TINY_VLLM_AXI)
#include "tiny_vllm/axi_backend.hpp"
#elif defined(TINY_VLLM_RTL)
#include "tiny_vllm/rtl_backend.hpp"
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>
#include <yaml-cpp/yaml.h>

namespace {
namespace fs = std::filesystem;
using namespace tiny_vllm;
#ifdef TINY_VLLM_TARGET
using HardwareBackend = TargetBackend;
#elif defined(TINY_VLLM_AXI)
using HardwareBackend = AxiBackend;
auto load_hardware_config(const std::string& path) { return load_axi_config(path); }
auto resolved_hardware_yaml(const AxiConfig& c) { return resolved_axi_yaml(c); }
#elif defined(TINY_VLLM_RTL)
using HardwareBackend = RtlBackend;
auto load_hardware_config(const std::string& path) { return load_rtl_config(path); }
auto resolved_hardware_yaml(const RtlConfig& c) { return resolved_rtl_yaml(c); }
#endif

std::string csv(const std::string& text) {
    std::string result = "\"";
    for (char c : text) { if (c == '"') result += '"'; result += c; }
    return result + '"';
}

std::ofstream output_file(const fs::path& path) {
    std::ofstream stream(path);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    return stream;
}

void write_file(const fs::path& path, const std::string& contents) {
    auto file = output_file(path);
    file << contents;
    file.close();
}

class FileSink final : public Sink {
    std::ofstream events_, transactions_, waits_;
public:
    explicit FileSink(const fs::path& directory)
        : events_(output_file(directory / "events.csv")),
          transactions_(output_file(directory / "transactions.csv")),
          waits_(output_file(directory / "driver_waits.csv")) {
        events_ << "event,request_id,step,backend_cycle,token_begin,token_end,computed_tokens,outputs,allocated_bytes,reserved_bytes,valid_kv_bytes,outstanding,logical_blocks,block_table,active_requests,reason\n";
        transactions_ << "event,request_id,step,backend_cycle,transaction_id,phase,layer,address,bytes,write\n";
        waits_ << "backend_cycle,reason\n";
    }
    void event(const Event& e) override {
        const auto& s = e.state;
        std::string blocks;
        for (auto block : e.block_table) {
            if (!blocks.empty()) blocks += ';';
            blocks += std::to_string(block);
        }
        events_ << e.kind << ',' << csv(e.request_id) << ',' << e.step << ',' << s.backend_cycles << ','
                << e.token_begin << ',' << e.token_end << ',' << e.request_computed << ',' << e.request_outputs << ','
                << s.allocated_bytes << ',' << s.reserved_bytes << ',' << s.valid_kv_bytes << ','
                << s.outstanding << ',' << e.block_table.size() << ',' << csv(blocks) << ','
                << s.active_requests << ',' << csv(e.reason) << '\n';
    }
    void transaction(const TransactionEvent& e) override {
        transactions_ << e.action << ',' << csv(e.request_id) << ',' << e.step << ',' << e.cycle << ','
                      << e.request.id << ',' << (e.prefill ? "prefill" : "decode") << ',' << e.layer << ','
                      << e.request.address << ',' << e.request.bytes << ',' << e.request.write << '\n';
    }
    void driver_wait(U64 cycle, const std::string& reason) override { waits_ << cycle << ',' << reason << '\n'; }
    void flush() { events_.flush(); transactions_.flush(); waits_.flush(); }
};

void finish(const fs::path& directory, const std::string& status, const std::string& message,
            const Stats& s, double elapsed, const std::string& fault) {
    YAML::Node metadata;
    metadata["status"] = status;
    metadata["error"] = message;
#ifdef TINY_VLLM_TARGET
    (void)fault;
    metadata["backend"] = "verilator_custom_target";
    metadata["rtl_model"] = "tiny_vllm_target; standard descriptor handshake ports";
    metadata["rtl_source_fingerprint"] = TINY_VLLM_TARGET_SOURCE_HASH;
    metadata["cycle_semantics"] = "post-reset rising edges; actual target acceptance/completion; no inferred queue or compute metrics";
#elif defined(TINY_VLLM_AXI)
    metadata["backend"] = "verilator_external_axi_ram";
    metadata["rtl_model"] = "unmodified alexforencich/verilog-axi axi_ram; 64-bit data/ID, 20-bit address, PIPELINE_OUTPUT=0";
    metadata["rtl_commit"] = "516bd5dadc3365b7f9e225d2af8fe0b8d804fe53";
    metadata["payload"] = "synthetic byte pattern checked against shadow memory; not numeric KV values";
    metadata["test_fault"] = fault;
    metadata["fault_boundary"] = "adapter observer/completion delivery; upstream RTL unmodified";
    metadata["cycle_semantics"] = "post-reset AXI edges; completion after every B or final R handshake of all sub-bursts";
#elif defined(TINY_VLLM_RTL)
    metadata["backend"] = "verilator_reference_rtl";
    if (fs::is_regular_file(directory / "resolved_rtl_config.yaml")) {
        const auto rtl = YAML::LoadFile((directory / "resolved_rtl_config.yaml").string());
        metadata["architecture"] = rtl["architecture"].as<std::string>();
    }
    metadata["rtl_model"] = "synthetic descriptor FIFO(s) and fixed-latency engine(s); no data payload";
    metadata["test_fault"] = fault;
    metadata["cycle_semantics"] = "post-reset rising edges; frontend accepted at edge boundary N, earliest RTL handshake N+1";
#else
    (void)fault;
    metadata["backend"] = "trace";
    metadata["cycle_semantics"] = "virtual trace cycles, not RTL cycles or LLM latency";
#endif
    metadata["access_policy"] = "prefill_store_decode_scan";
    metadata["source_revision_at_configure"] = TINY_VLLM_SOURCE_REVISION;
    metadata["source_dirty_at_configure"] = TINY_VLLM_SOURCE_DIRTY;
    metadata["compiler"] = __VERSION__;
    metadata["host_elapsed_seconds"] = elapsed;
    metadata["summary_semantics"] = status == "ok" ? "complete workload" : "partial counters; failed run";
    metadata["upstream_comparison"] = "not_run";
    write_file(directory / "metadata.yaml", YAML::Dump(metadata) + '\n');
    auto summary = output_file(directory / "summary.csv");
    summary << "status,generated_transactions,accepted_transactions,completed_transactions,completed_read_bytes,completed_write_bytes,peak_allocated_bytes,peak_reserved_bytes,peak_valid_kv_bytes,final_allocated_bytes,final_reserved_bytes,final_valid_kv_bytes,final_outstanding,peak_outstanding,backend_cycles,serving_steps,admission_step,completion_step,computed_tokens,outputs,admitted_requests,completed_requests,final_active_requests,peak_active_requests\n";
    summary << status << ',' << s.generated << ',' << s.accepted << ',' << s.completed << ','
            << s.read_bytes << ',' << s.write_bytes << ',' << s.peak_allocated_bytes << ','
            << s.peak_reserved_bytes << ',' << s.peak_valid_kv_bytes << ',' << s.allocated_bytes << ','
            << s.reserved_bytes << ',' << s.valid_kv_bytes << ',' << s.outstanding << ','
            << s.peak_outstanding << ',' << s.backend_cycles << ',' << s.serving_steps << ',';
    // A failed run has no valid completion step, even when the default counter is zero.
    if (s.peak_reserved_bytes) summary << s.admission_step;
    summary << ',';
    if (status == "ok") summary << s.completion_step;
    summary << ',' << s.computed_tokens << ',' << s.outputs << ',' << s.admitted_requests << ','
            << s.completed_requests << ',' << s.active_requests << ',' << s.peak_active_requests << '\n';
    summary.close();
}
}

int main(int argc, char** argv) {
#ifdef TINY_VLLM_TARGET
    const std::string usage = "Usage: tiny-vllm-target --config FILE --output NEW_DIRECTORY [--waveform]\n";
#elif defined(TINY_VLLM_AXI)
    const std::string usage = "Usage: tiny-vllm-axi --config FILE --rtl-config FILE --output NEW_DIRECTORY [--inject-fault read-data|drop-completion]\n";
#elif defined(TINY_VLLM_RTL)
    const std::string usage = "Usage: tiny-vllm-rtl --config FILE --rtl-config FILE --output NEW_DIRECTORY [--rtl single|split-rw] [--inject-fault address|drop-completion|duplicate-completion]\n";
#else
    const std::string usage = "Usage: tiny-vllm-trace --config FILE --output NEW_DIRECTORY\n";
#endif
    if (argc == 2 && std::string_view(argv[1]) == "--help") { std::cout << usage; return 0; }
    fs::path config_path, output_path;
    std::string fault = "none";
#ifdef TINY_VLLM_TARGET
    bool waveform = false;
#endif
#ifdef TINY_VLLM_RTL
    std::string architecture;
#endif
#ifdef TINY_VLLM_CONFIGURED_HARDWARE
    fs::path rtl_config_path;
    bool fault_set = false;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
#ifdef TINY_VLLM_TARGET
        if (option == "--waveform" && !waveform) { waveform = true; continue; }
#endif
        if (i + 1 >= argc) { std::cerr << usage; return 2; }
        if (option == "--config" && config_path.empty()) config_path = argv[++i];
        else if (option == "--output" && output_path.empty()) output_path = argv[++i];
#ifdef TINY_VLLM_RTL
        else if (option == "--rtl" && architecture.empty()) architecture = argv[++i];
#endif
#ifdef TINY_VLLM_CONFIGURED_HARDWARE
        else if (option == "--rtl-config" && rtl_config_path.empty()) rtl_config_path = argv[++i];
        else if (option == "--inject-fault" && !fault_set) { fault = argv[++i]; fault_set = true; }
#endif
        else { std::cerr << usage; return 2; }
    }
    if (config_path.empty() || output_path.empty()) { std::cerr << usage; return 2; }
#ifdef TINY_VLLM_CONFIGURED_HARDWARE
    if (rtl_config_path.empty()) { std::cerr << usage; return 2; }
#endif
    try {
        if (!output_path.parent_path().empty()) fs::create_directories(output_path.parent_path());
        if (!fs::create_directory(output_path)) {
            std::cerr << "Output directory already exists; choose a new directory.\n";
            return 4;
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 4; }

    Stats stats;
    std::unique_ptr<FileSink> sink;
#ifdef TINY_VLLM_HARDWARE
    std::unique_ptr<HardwareBackend> backend;
#endif
    std::string status = "ok", message;
    int exit_code = 0;
    const auto start = std::chrono::steady_clock::now();
    try {
        // Preserve the original input, including invalid configurations.
        if (fs::is_regular_file(config_path)) fs::copy_file(config_path, output_path / "input.yaml");
#ifdef TINY_VLLM_CONFIGURED_HARDWARE
        if (fs::is_regular_file(rtl_config_path)) fs::copy_file(rtl_config_path, output_path / "rtl_input.yaml");
#endif
        const auto config = load_config(config_path.string());
        write_file(output_path / "resolved_config.yaml", resolved_yaml(config));
        sink = std::make_unique<FileSink>(output_path);
#ifdef TINY_VLLM_TARGET
        backend = std::make_unique<HardwareBackend>(output_path, waveform);
        run(config, *backend, *sink, stats);
        backend->verify_drained();
#elif defined(TINY_VLLM_CONFIGURED_HARDWARE)
        auto rtl_config = load_hardware_config(rtl_config_path.string());
#ifdef TINY_VLLM_RTL
        if (!architecture.empty()) rtl_config.architecture = architecture == "split-rw" ? "dual-rw" : architecture;
#endif
        write_file(output_path / "resolved_rtl_config.yaml", resolved_hardware_yaml(rtl_config));
        backend = std::make_unique<HardwareBackend>(config, rtl_config, output_path, fault);
        run(config, *backend, *sink, stats);
        backend->verify_drained();
#else
        TraceBackend trace;
        run(config, trace, *sink, stats);
#endif
    } catch (const Error& error) {
        status = error.status; message = error.what(); exit_code = status == "config_error" ? 2 : 3;
    } catch (const std::exception& error) {
        status = "runtime_error"; message = error.what(); exit_code = 3;
    }
    try {
        if (sink) sink->flush();
#ifdef TINY_VLLM_HARDWARE
        if (backend) backend->finish(status);
#endif
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        finish(output_path, status, message, stats, elapsed, fault);
    } catch (const std::exception& error) {
        std::cerr << "Could not save run results: " << error.what() << '\n'; return 4;
    }
    if (exit_code) std::cerr << status << ": " << message << '\n';
    else {
        std::cout << "Completed " << stats.serving_steps << " serving steps; write=" << stats.write_bytes
                   << " B, read=" << stats.read_bytes << " B, transactions=" << stats.completed << '\n'
                   << "Serving requests: " << stats.completed_requests << '\n'
                   << "Memory requests accepted/completed: " << stats.accepted << '/' << stats.completed << '\n'
                   << "Results: " << output_path.string() << '\n';
#if defined(TINY_VLLM_RTL) || defined(TINY_VLLM_TARGET)
        std::cout << backend->console_summary();
#endif
    }
    return exit_code;
}
