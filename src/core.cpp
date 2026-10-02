#include "tiny_vllm/core.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <span>

namespace tiny_vllm {
bool TraceBackend::try_submit(const MemoryRequest& r) {
    if (pending_) return false;
    pending_ = r.id;
    return true;
}
void TraceBackend::tick() {
    if (pending_) { completed_.push_back(*pending_); pending_.reset(); }
}
std::optional<U64> TraceBackend::poll_completion() {
    if (completed_.empty()) return std::nullopt;
    const auto id = completed_.front(); completed_.pop_front(); return id;
}

namespace {
U64 plus(U64 a, U64 b) {
    if (b > std::numeric_limits<U64>::max() - a)
        throw Error("protocol_error", "Counter overflow");
    return a + b;
}
U64 ceil_div(U64 a, U64 b) { return a / b + (a % b != 0); }

struct StepPlan {
    std::size_t owner;
    U64 step, begin, end, frontier;
    bool prefill;
    std::span<const U64> blocks;
};
struct RequestState {
    U64 computed{}, outputs{}, reservation{};
    std::vector<U64> blocks;
    void commit(const StepPlan& plan) {
        if (computed != plan.begin) throw Error("protocol_error", "Stale StepPlan");
        computed = plan.end;
        if (computed == plan.frontier) ++outputs;
    }
};
class TokenBudgetScheduler {
    const Config& c_;
public:
    explicit TokenBudgetScheduler(const Config& c) : c_(c) {}
    StepPlan schedule(std::size_t owner, const RequestState& state, U64 step, U64& budget) const {
        const auto frontier = c_.requests[owner].prompt_tokens + state.outputs;
        const bool prefill = state.computed < c_.requests[owner].prompt_tokens;
        U64 n = std::min(frontier - state.computed, budget);
        if (prefill) n = std::min(n, c_.prefill_chunk_tokens);
        if (!n) throw Error("protocol_error", "Scheduler made no progress");
        budget -= n;
        return {owner, step, state.computed, state.computed + n, frontier, prefill, {}};
    }
};

// Only allocated/recycled blocks occupy host bookkeeping memory.
class PagedKVManager {
    const Config& c_;
    U64 next_unused_{};
    std::set<U64> free_;
    std::map<U64, std::size_t> owners_;
public:
    U64 reserved_blocks{};
    const U64 bundle_bytes;
    explicit PagedKVManager(const Config& c) : c_(c),
        bundle_bytes(2 * c.num_layers * c.kv_heads * c.head_dim * c.dtype_bytes * c.block_tokens) {}
    U64 needed(std::size_t owner) const {
        const auto& r = c_.requests[owner];
        return ceil_div(r.prompt_tokens + (r.output_tokens - 1), c_.block_tokens);
    }
    bool can_admit(std::size_t owner) const { return needed(owner) <= c_.pool_blocks - reserved_blocks; }
    void reserve(std::size_t owner, RequestState& state) {
        if (state.reservation || !can_admit(owner)) throw Error("protocol_error", "Invalid reservation");
        state.reservation = needed(owner);
        reserved_blocks += state.reservation;
    }
    void ensure(std::size_t owner, RequestState& state, U64 tokens) {
        const auto required = ceil_div(tokens, c_.block_tokens);
        if (required > state.reservation) throw Error("protocol_error", "Allocation exceeded reservation");
        while (state.blocks.size() < required) {
            U64 physical;
            if (!free_.empty()) { physical = *free_.begin(); free_.erase(free_.begin()); }
            else {
                if (next_unused_ >= c_.pool_blocks) throw Error("protocol_error", "No physical block for reserved request");
                physical = next_unused_++;
            }
            if (!owners_.emplace(physical, owner).second) throw Error("protocol_error", "Block already owned");
            state.blocks.push_back(physical);
        }
    }
    U64 allocated_bytes() const { return owners_.size() * bundle_bytes; }
    void release(std::size_t owner, RequestState& state, U64 outstanding) {
        if (outstanding) throw Error("protocol_error", "Attempted free before step completion");
        for (auto physical : state.blocks) {
            const auto found = owners_.find(physical);
            if (found == owners_.end() || found->second != owner)
                throw Error("protocol_error", "Invalid block owner on release");
            owners_.erase(found);
            free_.insert(physical);
        }
        reserved_blocks -= state.reservation;
        state.reservation = 0;
        state.blocks.clear();
    }
};

// A lazy, per-request phase cursor. Reads and later layers become eligible only
// when every accepted transaction in the previous phase has completed.
class AccessCursor {
    const Config& c_;
    const StepPlan& plan_;
    U64 kv_{}, head_{}, token_{}, address_{}, remaining_{};
    bool exhausted_{};
    U64 begin() const { return write ? plan_.begin : 0; }
    void reset_range() { kv_ = head_ = 0; token_ = begin(); remaining_ = 0; exhausted_ = false; }
public:
    U64 layer{}, pending{};
    bool write{true}, done{};
    AccessCursor(const Config& c, const StepPlan& plan) : c_(c), plan_(plan), token_(plan.begin) {}
    std::optional<MemoryRequest> next() {
        while (!done) {
            if (exhausted_) {
                if (pending) return std::nullopt;
                if (write && !plan_.prefill) write = false;
                else { write = true; ++layer; }
                if (layer == c_.num_layers) { done = true; return std::nullopt; }
                reset_range();
            }
            if (!remaining_) {
                if (token_ == plan_.end) {
                    token_ = begin();
                    if (++head_ == c_.kv_heads) { head_ = 0; ++kv_; }
                }
                if (kv_ == 2) { exhausted_ = true; continue; }
                const auto offset = token_ % c_.block_tokens;
                const auto count = std::min(plan_.end - token_, c_.block_tokens - offset);
                const auto physical = plan_.blocks[token_ / c_.block_tokens];
                address_ = ((((physical * c_.num_layers + layer) * 2 + kv_) * c_.kv_heads + head_)
                            * c_.block_tokens + offset) * c_.head_dim * c_.dtype_bytes;
                remaining_ = count * c_.head_dim * c_.dtype_bytes;
                token_ += count;
            }
            const U64 page = 2 * c_.kv_heads * c_.block_tokens * c_.head_dim * c_.dtype_bytes;
            const auto bytes = std::min({remaining_, c_.transaction_bytes - address_ % c_.transaction_bytes,
                                         page - address_ % page});
            MemoryRequest result{0, address_, bytes, write};
            address_ += bytes; remaining_ -= bytes;
            return result;
        }
        return std::nullopt;
    }
};

class TransactionDriver {
    const Config& c_;
    Backend& backend_;
    Sink& sink_;
    Stats& stats_;
public:
    TransactionDriver(const Config& c, Backend& backend, Sink& sink, Stats& stats)
        : c_(c), backend_(backend), sink_(sink), stats_(stats) {}
    void execute(const std::vector<StepPlan>& plans) {
        std::vector<AccessCursor> cursors;
        cursors.reserve(plans.size());
        for (const auto& plan : plans) cursors.emplace_back(c_, plan);
        struct InFlight { TransactionEvent event; std::size_t cursor; };
        std::map<U64, InFlight> pending;
        std::optional<InFlight> offered;
        std::size_t round_robin = 0;
        auto collect = [&] {
            while (const auto id = backend_.poll_completion()) {
                const auto found = pending.find(*id);
                if (found == pending.end()) throw Error("protocol_error", "Unknown or duplicate completion id " + std::to_string(*id));
                auto event = found->second.event;
                --cursors[found->second.cursor].pending;
                pending.erase(found);
                event.action = "completed"; event.cycle = stats_.backend_cycles;
                stats_.completed = plus(stats_.completed, 1);
                auto& bytes = event.request.write ? stats_.write_bytes : stats_.read_bytes;
                bytes = plus(bytes, event.request.bytes);
                stats_.outstanding = pending.size();
                sink_.transaction(event);
            }
        };
        while (true) {
            collect();
            if (!offered) {
                for (std::size_t i = 0; i < cursors.size(); ++i) {
                    const auto index = (round_robin + i) % cursors.size();
                    auto request = cursors[index].next();
                    if (!request) continue;
                    request->id = stats_.generated = plus(stats_.generated, 1);
                    const auto& plan = plans[index];
                    offered = InFlight{{"generated", stats_.backend_cycles, plan.step, cursors[index].layer,
                                        plan.prefill, *request, c_.requests[plan.owner].id}, index};
                    sink_.transaction(offered->event);
                    break;
                }
            }
            if (!offered && pending.empty() && std::all_of(cursors.begin(), cursors.end(),
                                                           [](const auto& cursor) { return cursor.done; })) break;
            if (stats_.backend_cycles >= c_.max_backend_cycles)
                throw Error("timeout", "max_backend_cycles reached before completion");
            const bool limit_wait = offered && pending.size() >= c_.max_outstanding;
            const bool accepted = offered && !limit_wait && backend_.try_submit(offered->event.request);
            if (offered && !accepted)
                sink_.driver_wait(stats_.backend_cycles, limit_wait ? "outstanding_limit" : "backend_staging");
            if (accepted) {
                auto item = *offered;
                item.event.action = "accepted"; item.event.cycle = stats_.backend_cycles;
                ++cursors[item.cursor].pending;
                pending.emplace(item.event.request.id, item);
                stats_.accepted = plus(stats_.accepted, 1);
                stats_.outstanding = pending.size();
                stats_.peak_outstanding = std::max(stats_.peak_outstanding, stats_.outstanding);
                sink_.transaction(item.event);
                round_robin = (item.cursor + 1) % cursors.size();
                offered.reset();
                collect(); // Immediate completions are allowed by the backend contract.
            }
            backend_.tick();
            ++stats_.backend_cycles;
        }
    }
};
}

void run(const Config& c, Backend& backend, Sink& sink, Stats& stats) {
    validate(c);
    stats = {};
    std::vector<RequestState> states(c.requests.size());
    std::vector<std::size_t> arrivals(c.requests.size()), active;
    std::iota(arrivals.begin(), arrivals.end(), 0);
    std::stable_sort(arrivals.begin(), arrivals.end(), [&](auto a, auto b) {
        return c.requests[a].arrival_step < c.requests[b].arrival_step;
    });
    std::deque<std::size_t> waiting;
    std::size_t next_arrival = 0;
    PagedKVManager memory(c);
    TokenBudgetScheduler scheduler(c);
    TransactionDriver driver(c, backend, sink, stats);
    const U64 token_bytes = 2 * c.num_layers * c.kv_heads * c.head_dim * c.dtype_bytes;
    U64 step = c.requests[arrivals.front()].arrival_step;
    auto event = [&](const std::string& kind, std::size_t owner, U64 begin = 0, U64 end = 0,
                     const std::string& reason = "") {
        stats.allocated_bytes = memory.allocated_bytes();
        stats.reserved_bytes = memory.reserved_blocks * memory.bundle_bytes;
        stats.peak_allocated_bytes = std::max(stats.peak_allocated_bytes, stats.allocated_bytes);
        stats.peak_reserved_bytes = std::max(stats.peak_reserved_bytes, stats.reserved_bytes);
        stats.peak_valid_kv_bytes = std::max(stats.peak_valid_kv_bytes, stats.valid_kv_bytes);
        stats.peak_active_requests = std::max(stats.peak_active_requests, stats.active_requests);
        const auto& state = states[owner];
        sink.event({kind, step, begin, end, stats, c.requests[owner].id, reason,
                    state.computed, state.outputs, state.blocks});
    };
    while (stats.completed_requests < c.requests.size()) {
        if (active.empty() && waiting.empty() && next_arrival < arrivals.size())
            step = std::max(step, c.requests[arrivals[next_arrival]].arrival_step);
        while (next_arrival < arrivals.size() && c.requests[arrivals[next_arrival]].arrival_step <= step) {
            const auto owner = arrivals[next_arrival++];
            waiting.push_back(owner); event("arrived", owner);
        }
        U64 budget = c.max_tokens_per_step;
        std::vector<StepPlan> plans;
        auto schedule = [&](std::size_t owner) {
            auto plan = scheduler.schedule(owner, states[owner], step, budget);
            memory.ensure(owner, states[owner], plan.end);
            plan.blocks = states[owner].blocks;
            plans.push_back(plan);
            event("allocated", owner, plan.begin, plan.end);
            event("scheduled", owner, plan.begin, plan.end);
        };
        for (auto owner : active) { if (!budget) break; schedule(owner); }
        while (!waiting.empty()) {
            const auto owner = waiting.front();
            std::string reason;
            if (!budget) reason = "token_budget";
            else if (active.size() >= c.max_active_requests) reason = "active_limit";
            else if (!memory.can_admit(owner)) reason = "reservation";
            if (!reason.empty()) { event("blocked", owner, 0, 0, reason); break; }
            waiting.pop_front();
            memory.reserve(owner, states[owner]);
            active.push_back(owner);
            if (stats.admitted_requests++ == 0) stats.admission_step = step;
            ++stats.active_requests;
            event("admitted", owner);
            schedule(owner);
        }
        if (plans.empty()) throw Error("protocol_error", "No scheduled work with unfinished requests");
        driver.execute(plans);
        stats.serving_steps = plus(stats.serving_steps, 1);
        // Commit every plan at the barrier before releasing any finished request.
        for (const auto& plan : plans) {
            auto& state = states[plan.owner];
            const auto old_outputs = state.outputs;
            state.commit(plan);
            stats.computed_tokens = plus(stats.computed_tokens, plan.end - plan.begin);
            stats.outputs = plus(stats.outputs, state.outputs - old_outputs);
            stats.valid_kv_bytes += (plan.end - plan.begin) * token_bytes;
            event("committed", plan.owner, plan.begin, plan.end);
        }
        for (const auto& plan : plans) {
            auto& state = states[plan.owner];
            if (state.outputs != c.requests[plan.owner].output_tokens) continue;
            ++stats.completed_requests;
            stats.completion_step = step;
            event("finished", plan.owner);
            memory.release(plan.owner, state, stats.outstanding);
            stats.valid_kv_bytes -= state.computed * token_bytes;
            --stats.active_requests;
            event("released", plan.owner);
        }
        std::erase_if(active, [&](auto owner) { return states[owner].outputs == c.requests[owner].output_tokens; });
        if (stats.completed_requests < c.requests.size()) step = plus(step, 1);
    }
    if (stats.generated != stats.accepted || stats.accepted != stats.completed || stats.outstanding ||
        stats.allocated_bytes || stats.reserved_bytes || stats.valid_kv_bytes || stats.active_requests)
        throw Error("protocol_error", "Final request/transaction/resource accounting mismatch");
}
}
