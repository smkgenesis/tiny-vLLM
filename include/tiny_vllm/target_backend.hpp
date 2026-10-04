#pragma once
#include "tiny_vllm/core.hpp"
#include <filesystem>
#include <memory>

namespace tiny_vllm {
// Standard-port adapter. No queue/busy/service instrumentation is required.
class TargetBackend final : public Backend {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    TargetBackend(const std::filesystem::path& output, bool waveform);
    ~TargetBackend() override;
    bool try_submit(const MemoryRequest&) override;
    void tick() override;
    std::optional<U64> poll_completion() override;
    void verify_drained() const;
    void finish(const std::string& status);
    std::string console_summary() const;
};
}
