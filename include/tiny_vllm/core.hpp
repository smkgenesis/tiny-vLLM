#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tiny_vllm {
using U64 = std::uint64_t;

struct Error : std::runtime_error {
    std::string status;
    Error(std::string kind, const std::string& message)
        : std::runtime_error(message), status(std::move(kind)) {}
};

struct Request {
    std::string id;
    U64 arrival_step{}, prompt_tokens{}, output_tokens{};
};

struct Config {
    U64 num_layers{}, kv_heads{}, head_dim{}, dtype_bytes{};
    U64 max_tokens_per_step{}, prefill_chunk_tokens{}, max_active_requests{};
    U64 block_tokens{}, pool_blocks{}, transaction_bytes{}, max_outstanding{};
    U64 max_backend_cycles{10000000};
    std::vector<Request> requests;
};

Config load_config(const std::string& path);
std::string resolved_yaml(const Config& config);
void validate(const Config& config);

struct MemoryRequest {
    U64 id{}, address{}, bytes{};
    bool write{};
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual bool try_submit(const MemoryRequest&) = 0;
    virtual void tick() = 0;
    virtual std::optional<U64> poll_completion() = 0;
};

// A one-entry backend with completion on the next virtual cycle.
class TraceBackend final : public Backend {
    std::optional<U64> pending_;
    std::deque<U64> completed_;
public:
    bool try_submit(const MemoryRequest& request) override;
    void tick() override;
    std::optional<U64> poll_completion() override;
};

struct Stats {
    U64 generated{}, accepted{}, completed{}, read_bytes{}, write_bytes{};
    U64 allocated_bytes{}, reserved_bytes{}, valid_kv_bytes{};
    U64 peak_allocated_bytes{}, peak_reserved_bytes{}, peak_valid_kv_bytes{};
    U64 outstanding{}, peak_outstanding{}, backend_cycles{}, serving_steps{};
    U64 admission_step{}, completion_step{}, outputs{}, computed_tokens{};
    U64 admitted_requests{}, completed_requests{}, active_requests{}, peak_active_requests{};
};

struct Event {
    std::string kind;
    U64 step{}, token_begin{}, token_end{};
    Stats state; // Whole-run counters and live global occupancy.
    std::string request_id, reason;
    U64 request_computed{}, request_outputs{}; // This request only.
    std::vector<U64> block_table;
};

struct TransactionEvent {
    std::string action;
    U64 cycle{}, step{}, layer{};
    bool prefill{};
    MemoryRequest request;
    std::string request_id;
};

class Sink {
public:
    virtual ~Sink() = default;
    virtual void event(const Event&) = 0;
    virtual void transaction(const TransactionEvent&) = 0;
    virtual void driver_wait(U64, const std::string&) {}
};

// Statistics remain available on failure; only a normal return means all
// requests completed and their resources were released.
void run(const Config&, Backend&, Sink&, Stats&);
}
