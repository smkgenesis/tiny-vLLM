#include "tiny_vllm/core.hpp"

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <set>
#include <string_view>
#include <yaml-cpp/yaml.h>

namespace tiny_vllm {
namespace {
[[noreturn]] void invalid(const std::string& message) {
    throw Error("config_error", message);
}

void keys(const YAML::Node& node, std::initializer_list<std::string_view> allowed,
          const std::string& path) {
    if (!node.IsMap()) invalid(path + " must be a mapping");
    const std::set<std::string_view> known(allowed);
    std::set<std::string> seen;
    for (const auto& item : node) {
        if (!item.first.IsScalar()) invalid(path + " has a non-scalar key");
        const auto key = item.first.Scalar();
        if (!known.contains(key)) invalid(path + ": unknown field " + key);
        if (!seen.insert(key).second) invalid(path + ": duplicate field " + key);
    }
}

U64 number(const YAML::Node& node, const std::string& key) {
    const auto value = node[key];
    if (!value.IsScalar()) invalid("Missing integer field: " + key);
    const auto text = value.Scalar();
    U64 result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size())
        invalid(key + " must be an unsigned decimal integer");
    return result;
}

U64 add(U64 a, U64 b) {
    if (b > std::numeric_limits<U64>::max() - a) invalid("Derived integer overflow");
    return a + b;
}

U64 multiply(U64 a, U64 b) {
    if (b && a > std::numeric_limits<U64>::max() / b) invalid("Derived size overflow");
    return a * b;
}

U64 ceil_div(U64 a, U64 b) { return a / b + (a % b != 0); }
}

void validate(const Config& c) {
    for (const auto value : {c.num_layers, c.kv_heads, c.head_dim, c.dtype_bytes,
             c.max_tokens_per_step, c.prefill_chunk_tokens, c.max_active_requests,
             c.block_tokens, c.pool_blocks, c.transaction_bytes, c.max_outstanding,
             c.max_backend_cycles})
        if (!value) invalid("Geometry, budgets, limits and token lengths must be positive");
    if (c.dtype_bytes != 2 && c.dtype_bytes != 4) invalid("dtype_bytes must be 2 or 4");
    if ((c.transaction_bytes & (c.transaction_bytes - 1)) != 0)
        invalid("transaction_bytes must be a power of two");
    U64 token_bytes = multiply(multiply(multiply(2, c.num_layers), c.kv_heads),
                               multiply(c.head_dim, c.dtype_bytes));
    const U64 bundle_bytes = multiply(token_bytes, c.block_tokens);
    multiply(bundle_bytes, c.pool_blocks);
    if (c.requests.empty()) invalid("requests must not be empty");
    std::set<std::string> ids;
    U64 total_tokens = 0, total_outputs = 0, latest_arrival = 0;
    for (const auto& r : c.requests) {
        if (r.id.empty() || !ids.insert(r.id).second) invalid("Request ids must be nonempty and unique");
        if (!r.prompt_tokens || !r.output_tokens) invalid("Request token lengths must be positive");
        const U64 final_tokens = add(r.prompt_tokens, r.output_tokens - 1);
        if (ceil_div(final_tokens, c.block_tokens) > c.pool_blocks)
            invalid("Request " + r.id + " final KV reservation exceeds the pool");
        total_tokens = add(total_tokens, final_tokens);
        total_outputs = add(total_outputs, r.output_tokens);
        latest_arrival = std::max(latest_arrival, r.arrival_step);
    }
    // Conservative bound: after the latest arrival, every nonempty step
    // computes at least one token. Also validate aggregate summary counters.
    add(latest_arrival, total_tokens - 1);
    multiply(total_tokens, token_bytes);
}

Config load_config(const std::string& path) {
    try {
        const auto documents = YAML::LoadAllFromFile(path);
        if (documents.size() != 1) invalid("Expected exactly one YAML document");
        const auto root = documents.front();
        keys(root, {"model", "scheduler", "kv_cache", "driver", "requests"}, "root");
        const auto model = root["model"], scheduler = root["scheduler"];
        const auto kv = root["kv_cache"], driver = root["driver"];
        keys(model, {"num_layers", "kv_heads", "head_dim", "dtype_bytes"}, "model");
        keys(scheduler, {"max_tokens_per_step", "prefill_chunk_tokens", "max_active_requests"}, "scheduler");
        keys(kv, {"block_tokens", "pool_blocks"}, "kv_cache");
        keys(driver, {"transaction_bytes", "max_outstanding", "max_backend_cycles"}, "driver");
        const auto requests = root["requests"];
        if (!requests.IsSequence() || requests.size() == 0) invalid("requests must be a nonempty sequence");
        Config c;
        c.num_layers = number(model, "num_layers");
        c.kv_heads = number(model, "kv_heads");
        c.head_dim = number(model, "head_dim");
        c.dtype_bytes = number(model, "dtype_bytes");
        c.max_tokens_per_step = number(scheduler, "max_tokens_per_step");
        c.prefill_chunk_tokens = number(scheduler, "prefill_chunk_tokens");
        c.max_active_requests = number(scheduler, "max_active_requests");
        c.block_tokens = number(kv, "block_tokens");
        c.pool_blocks = number(kv, "pool_blocks");
        c.transaction_bytes = number(driver, "transaction_bytes");
        c.max_outstanding = number(driver, "max_outstanding");
        if (driver["max_backend_cycles"]) c.max_backend_cycles = number(driver, "max_backend_cycles");
        for (const auto& request : requests) {
            keys(request, {"id", "arrival_step", "prompt_tokens", "output_tokens"}, "request");
            if (!request["id"].IsScalar()) invalid("Request id must be a string scalar");
            c.requests.push_back({request["id"].Scalar(), number(request, "arrival_step"),
                                  number(request, "prompt_tokens"), number(request, "output_tokens")});
        }
        validate(c);
        return c;
    } catch (const YAML::Exception& error) {
        invalid(error.what());
    }
}

std::string resolved_yaml(const Config& c) {
    YAML::Node root;
    auto model = root["model"], scheduler = root["scheduler"];
    auto kv = root["kv_cache"], driver = root["driver"];
    model["num_layers"] = c.num_layers; model["kv_heads"] = c.kv_heads;
    model["head_dim"] = c.head_dim; model["dtype_bytes"] = c.dtype_bytes;
    scheduler["max_tokens_per_step"] = c.max_tokens_per_step;
    scheduler["prefill_chunk_tokens"] = c.prefill_chunk_tokens;
    scheduler["max_active_requests"] = c.max_active_requests;
    kv["block_tokens"] = c.block_tokens; kv["pool_blocks"] = c.pool_blocks;
    driver["transaction_bytes"] = c.transaction_bytes;
    driver["max_outstanding"] = c.max_outstanding;
    driver["max_backend_cycles"] = c.max_backend_cycles;
    for (const auto& r : c.requests) {
        YAML::Node request;
        request["id"] = r.id; request["arrival_step"] = r.arrival_step;
        request["prompt_tokens"] = r.prompt_tokens;
        request["output_tokens"] = r.output_tokens;
        root["requests"].push_back(request);
    }
    return YAML::Dump(root) + '\n';
}
}
