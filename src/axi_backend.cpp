#include "tiny_vllm/axi_backend.hpp"
#include "Vaxi_ram.h"
#include <verilated.h>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <set>

namespace tiny_vllm {
namespace {
constexpr U64 capacity = 1ULL << 20;
std::ofstream file(const std::filesystem::path& path) {
    std::ofstream out(path); out.exceptions(std::ios::failbit | std::ios::badbit); return out;
}
unsigned char pattern(U64 id, U64 address) { return (id * 131 + address * 17 + (address >> 8)) & 255; }
}
AxiConfig load_axi_config(const std::string& path) {
    try {
        const auto docs = YAML::LoadAllFromFile(path);
        if (docs.size() != 1 || !docs[0].IsMap()) throw Error("config_error", "AXI config must be one mapping");
        AxiConfig result;
        std::set<std::string> seen;
        for (const auto& field : docs[0]) {
            if (!field.first.IsScalar() || !field.second.IsScalar()) throw Error("config_error", "AXI fields must be integers");
            const auto key = field.first.Scalar(), value = field.second.Scalar();
            U64 v{};
            const auto [end, ec] = std::from_chars(value.data(), value.data()+value.size(), v);
            if (!seen.insert(key).second || ec != std::errc{} || end != value.data()+value.size())
                throw Error("config_error", "Invalid or duplicate AXI field: " + key);
            if (key == "max_burst_beats") result.max_burst_beats = v;
            else if (key == "response_pause_period") result.response_pause_period = v;
            else if (key == "response_pause_cycles") result.response_pause_cycles = v;
            else throw Error("config_error", "Unknown AXI field: " + key);
        }
        if (!result.max_burst_beats || result.max_burst_beats > 256 ||
            (result.response_pause_cycles && result.response_pause_cycles >= result.response_pause_period))
            throw Error("config_error", "AXI burst beats must be 1..256; pauses must leave response-ready cycles");
        return result;
    } catch (const YAML::Exception& e) { throw Error("config_error", e.what()); }
}
std::string resolved_axi_yaml(const AxiConfig& c) {
    YAML::Node root;
    root["max_burst_beats"] = c.max_burst_beats;
    root["response_pause_period"] = c.response_pause_period;
    root["response_pause_cycles"] = c.response_pause_cycles;
    return YAML::Dump(root) + '\n';
}

struct AxiBackend::Impl {
    struct Transfer {
        MemoryRequest request;
        U64 done{}, address{}, size{}, beats{}, sent{}, received{};
        bool address_sent{};
    };
    VerilatedContext context;
    Vaxi_ram dut{&context};
    AxiConfig config;
    std::filesystem::path output;
    std::string fault;
    std::ofstream wire;
    std::optional<Transfer> wr, rd;
    std::deque<U64> completed;
    std::vector<unsigned char> memory = std::vector<unsigned char>(capacity);
    std::vector<bool> initialized = std::vector<bool>(capacity);
    U64 arena{}, edge{}, accepted{}, returned{}, aw_count{}, ar_count{}, w_count{}, r_count{}, b_count{};
    U64 write_bytes{}, read_bytes{}, b_stall{}, r_stall{}, peak_live{}, read_write_overlap{};
    bool fault_fired{};
    // Payload held while VALID && !READY on every channel, not only responses.
    using Payload = std::array<U64, 9>;
    std::array<std::optional<Payload>, 5> held;

    Impl(const Config& c, const AxiConfig& a, const std::filesystem::path& out, const std::string& f)
        : config(a), output(out), fault(f), wire(file(out / "axi_cycles.csv")) {
        arena = 2*c.num_layers*c.kv_heads*c.head_dim*c.dtype_bytes*c.block_tokens*c.pool_blocks;
        if (arena > capacity) throw Error("config_error", "AXI RAM capacity is 1048576 bytes; arena is larger");
        if (f != "none" && f != "read-data" && f != "drop-completion")
            throw Error("config_error", "Unknown AXI observer fault: " + f);
        if (!a.max_burst_beats || a.max_burst_beats > 256 ||
            (a.response_pause_cycles && a.response_pause_cycles >= a.response_pause_period))
            throw Error("config_error", "Invalid AXI runtime configuration");
        wire << "edge,channel,valid,ready,id,address,len,size,data,strb,last,resp,burst\n";
        dut.clk=0; dut.rst=1;
        dut.s_axi_awvalid=0; dut.s_axi_wvalid=0; dut.s_axi_arvalid=0;
        dut.s_axi_bready=0; dut.s_axi_rready=0;
        dut.s_axi_awlock=0; dut.s_axi_awcache=0; dut.s_axi_awprot=0;
        dut.s_axi_arlock=0; dut.s_axi_arcache=0; dut.s_axi_arprot=0;
        dut.eval(); clock(); clock(); dut.rst=0; dut.eval();
    }
    ~Impl() { dut.final(); }
    void clock() {
        dut.clk=0; dut.eval(); context.timeInc(1);
        dut.clk=1; dut.eval(); context.timeInc(1);
        dut.clk=0; dut.eval();
    }
    void prepare(Transfer& t) {
        t.address = t.request.address + t.done;
        const U64 remaining = t.request.bytes - t.done;
        // Aligned narrow prefix/tail; full-width INCR middle. Never overread,
        // cross 4 KiB, or exceed AXI's 256-beat limit.
        t.size = 0;
        while (t.size < 3 && t.address % (1ULL << (t.size+1)) == 0 && remaining >= (1ULL << (t.size+1))) ++t.size;
        t.beats = t.size == 3 ? std::min({remaining/8, config.max_burst_beats, (4096-t.address%4096)/8}) : 1;
        t.sent=0; t.received=0; t.address_sent=false;
    }
    void observe(unsigned channel, const char* name, bool valid, bool ready, const Payload& p) {
        wire << edge << ',' << name << ',' << valid << ',' << ready;
        for (auto field : p) wire << ',' << field;
        wire << '\n';
        if (held[channel] && (!valid || *held[channel] != p))
            throw Error("protocol_error", std::string("AXI payload changed under stall on ")+name);
        held[channel] = valid && !ready ? std::optional<Payload>(p) : std::nullopt;
    }
    void finish_burst(std::optional<Transfer>& lane) {
        auto& t=*lane;
        t.done += t.beats * (1ULL<<t.size);
        if (t.done != t.request.bytes) { prepare(t); return; }
        if (fault == "drop-completion" && !fault_fired) fault_fired=true;
        else { completed.push_back(t.request.id); ++returned; }
        lane.reset();
    }
    void tick() {
        ++edge;
        if (wr && rd) ++read_write_overlap;
        const bool ready = !config.response_pause_period || edge%config.response_pause_period >= config.response_pause_cycles;
        dut.s_axi_bready=ready; dut.s_axi_rready=ready;
        dut.s_axi_awvalid=wr && !wr->address_sent;
        dut.s_axi_wvalid=wr && wr->address_sent && wr->sent < wr->beats;
        dut.s_axi_arvalid=rd && !rd->address_sent;
        dut.s_axi_awid=wr ? wr->request.id : 0; dut.s_axi_awaddr=wr ? wr->address : 0;
        dut.s_axi_awlen=wr ? wr->beats-1 : 0; dut.s_axi_awsize=wr ? wr->size : 0; dut.s_axi_awburst=1;
        dut.s_axi_arid=rd ? rd->request.id : 0; dut.s_axi_araddr=rd ? rd->address : 0;
        dut.s_axi_arlen=rd ? rd->beats-1 : 0; dut.s_axi_arsize=rd ? rd->size : 0; dut.s_axi_arburst=1;
        dut.s_axi_wdata=0; dut.s_axi_wstrb=0; dut.s_axi_wlast=0;
        if (dut.s_axi_wvalid) {
            const U64 address=wr->address+(wr->sent<<wr->size);
            for (U64 i=0; i<(1ULL<<wr->size); ++i) {
                const U64 lane=(address+i)%8;
                dut.s_axi_wdata |= U64(pattern(wr->request.id,address+i))<<(lane*8);
                dut.s_axi_wstrb |= 1<<lane;
            }
            dut.s_axi_wlast=wr->sent+1==wr->beats;
        }
        dut.eval();
        observe(0,"AW",dut.s_axi_awvalid,dut.s_axi_awready,{dut.s_axi_awid,dut.s_axi_awaddr,dut.s_axi_awlen,dut.s_axi_awsize,0,0,0,0,dut.s_axi_awburst});
        observe(1,"W",dut.s_axi_wvalid,dut.s_axi_wready,{0,0,0,0,dut.s_axi_wdata,dut.s_axi_wstrb,dut.s_axi_wlast,0});
        observe(2,"B",dut.s_axi_bvalid,dut.s_axi_bready,{dut.s_axi_bid,0,0,0,0,0,0,dut.s_axi_bresp});
        observe(3,"AR",dut.s_axi_arvalid,dut.s_axi_arready,{dut.s_axi_arid,dut.s_axi_araddr,dut.s_axi_arlen,dut.s_axi_arsize,0,0,0,0,dut.s_axi_arburst});
        observe(4,"R",dut.s_axi_rvalid,dut.s_axi_rready,{dut.s_axi_rid,0,0,0,dut.s_axi_rdata,0,dut.s_axi_rlast,dut.s_axi_rresp});
        const bool aw=dut.s_axi_awvalid && dut.s_axi_awready, w=dut.s_axi_wvalid && dut.s_axi_wready;
        const bool ar=dut.s_axi_arvalid && dut.s_axi_arready, r=dut.s_axi_rvalid && dut.s_axi_rready;
        const bool b=dut.s_axi_bvalid && dut.s_axi_bready;
        if (dut.s_axi_bvalid && !ready) ++b_stall;
        if (dut.s_axi_rvalid && !ready) ++r_stall;
        if (aw) { wr->address_sent=true; ++aw_count; }
        if (ar) { rd->address_sent=true; ++ar_count; }
        if (w) {
            const U64 address=wr->address+(wr->sent<<wr->size);
            for (U64 i=0; i<(1ULL<<wr->size); ++i) {
                memory[address+i]=pattern(wr->request.id,address+i); initialized[address+i]=true;
            }
            write_bytes+=1ULL<<wr->size; ++wr->sent; ++w_count;
        }
        bool read_done=false;
        if (r) {
            if (!rd || !rd->address_sent || rd->received >= rd->beats || dut.s_axi_rid != rd->request.id || dut.s_axi_rresp != 0 ||
                bool(dut.s_axi_rlast) != (rd->received+1==rd->beats)) throw Error("protocol_error", "Invalid AXI read response");
            const U64 address=rd->address+(rd->received<<rd->size);
            U64 data=dut.s_axi_rdata;
            if (fault == "read-data" && !fault_fired) { data ^= 1ULL<<((address%8)*8); fault_fired=true; }
            for (U64 i=0; i<(1ULL<<rd->size); ++i) {
                if (!initialized[address+i] || ((data>>(((address+i)%8)*8))&255) != memory[address+i])
                    throw Error("protocol_error", "AXI read data mismatch/uninitialized byte at "+std::to_string(address+i));
            }
            read_bytes+=1ULL<<rd->size; ++rd->received; ++r_count;
            read_done=rd->received==rd->beats;
        }
        if (b) {
            if (!wr || !wr->address_sent || wr->sent != wr->beats || dut.s_axi_bid != wr->request.id || dut.s_axi_bresp != 0)
                throw Error("protocol_error", "Invalid AXI write response");
            ++b_count;
        }
        clock();
        if (b) finish_burst(wr);
        if (read_done) finish_burst(rd);
    }
};
AxiBackend::AxiBackend(const Config& c, const AxiConfig& a, const std::filesystem::path& p, const std::string& f)
    : impl_(std::make_unique<Impl>(c,a,p,f)) {}
AxiBackend::~AxiBackend() = default;
bool AxiBackend::try_submit(const MemoryRequest& r) {
    auto& s=*impl_;
    if (!r.id || !r.bytes || r.address>=s.arena || r.bytes>s.arena-r.address)
        throw Error("protocol_error", "AXI descriptor is outside configured arena");
    auto& lane=r.write ? s.wr : s.rd;
    if (lane) return false;
    lane=Impl::Transfer{r}; s.prepare(*lane); ++s.accepted;
    s.peak_live=std::max(s.peak_live,U64(s.wr.has_value())+U64(s.rd.has_value()));
    return true;
}
void AxiBackend::tick() { impl_->tick(); }
std::optional<U64> AxiBackend::poll_completion() {
    if (impl_->completed.empty()) return {};
    const auto id=impl_->completed.front(); impl_->completed.pop_front(); return id;
}
void AxiBackend::verify_drained() const {
    const auto& s=*impl_;
    if (s.wr || s.rd || !s.completed.empty() || s.dut.s_axi_bvalid || s.dut.s_axi_rvalid || s.accepted!=s.returned)
        throw Error("protocol_error", "AXI adapter/RTL not drained");
}
void AxiBackend::finish(const std::string& status) {
    auto& s=*impl_; s.wire.flush();
    auto out=file(s.output/"axi_summary.csv");
    out << "status,descriptors_accepted,descriptors_completed,aw_bursts,w_beats,b_responses,ar_bursts,r_beats,write_bytes,read_bytes,b_stall_cycles,r_stall_cycles,peak_live_descriptors,read_write_overlap_cycles,backend_cycles,fault_fired\n";
    out << status << ',' << s.accepted << ',' << s.returned << ',' << s.aw_count << ',' << s.w_count << ',' << s.b_count << ','
        << s.ar_count << ',' << s.r_count << ',' << s.write_bytes << ',' << s.read_bytes << ',' << s.b_stall << ',' << s.r_stall << ','
        << s.peak_live << ',' << s.read_write_overlap << ',' << s.edge << ',' << s.fault_fired << '\n';
    out.close();
}
}
