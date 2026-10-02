#pragma once
#include "tiny_vllm/core.hpp"
#include <filesystem>
#include <memory>

namespace tiny_vllm {
struct AxiConfig {
    U64 max_burst_beats{256};
    U64 response_pause_period{0}, response_pause_cycles{0};
};
AxiConfig load_axi_config(const std::string&);
std::string resolved_axi_yaml(const AxiConfig&);

// AXI4 RAM: 64-bit data/ID, 20-bit byte address, one descriptor per direction.
// All sub-bursts must respond before the original MemoryRequest completes.
class AxiBackend final : public Backend {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    AxiBackend(const Config&, const AxiConfig&, const std::filesystem::path&, const std::string& fault = "none");
    ~AxiBackend() override;
    bool try_submit(const MemoryRequest&) override;
    void tick() override;
    std::optional<U64> poll_completion() override;
    void verify_drained() const;
    void finish(const std::string&);
};
}
