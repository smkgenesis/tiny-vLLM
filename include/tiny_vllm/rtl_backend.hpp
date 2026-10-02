#pragma once
#include "tiny_vllm/core.hpp"
#include <filesystem>
#include <memory>

namespace tiny_vllm {
struct RtlConfig {
    U64 fifo_depth{2}, service_cycles{3}, initial_request_pause_cycles{};
};
RtlConfig load_rtl_config(const std::string& path);
std::string resolved_rtl_yaml(const RtlConfig&);

// Owns one staging slot. Acceptance transfers ownership to the adapter;
// the following rising-edge handshake transfers it to the RTL FIFO.
class RtlBackend final : public Backend {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    RtlBackend(const Config&, const RtlConfig&, const std::filesystem::path& output,
               const std::string& fault = "none");
    ~RtlBackend() override;
    bool try_submit(const MemoryRequest&) override;
    void tick() override;
    std::optional<U64> poll_completion() override;
    void verify_drained() const;
    void finish(const std::string& status);
};
}
