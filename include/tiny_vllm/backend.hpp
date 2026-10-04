#pragma once
#include <cstdint>
#include <optional>

namespace tiny_vllm {
using U64 = std::uint64_t;

struct MemoryRequest {
    U64 id{}, address{}, bytes{};
    bool write{};
};

class Backend {
public:
    virtual ~Backend() = default;
    // true transfers ownership to the adapter; it is not a DUT handshake.
    virtual bool try_submit(const MemoryRequest&) = 0;
    virtual void tick() = 0;
    virtual std::optional<U64> poll_completion() = 0;
};
}
