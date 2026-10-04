// Runnable interface example, NOT a memory or accelerator implementation.
// Replace the body with a wrapper around your DUT. Complete an ID only after
// every operation belonging to that descriptor has actually finished.
module tiny_vllm_target (
    input  logic        clk, reset,
    input  logic        req_valid,
    output logic        req_ready,
    input  logic [63:0] req_id, req_address, req_bytes,
    input  logic        req_write,
    output logic        cmp_valid,
    input  logic        cmp_ready,
    output logic [63:0] cmp_id,
    output logic        cmp_error
);
    localparam logic [63:0] ARENA_BYTES = 64'd1048576;
    logic busy;
    logic [1:0] remaining;
    // This descriptor-only example deliberately treats both directions alike.
    wire unused_req_write = req_write;
    assign req_ready = !reset && !busy;
    assign cmp_valid = !reset && busy && remaining == 0;

    always_ff @(posedge clk) begin
        if (reset) begin
            busy <= 0;
            remaining <= 0;
            cmp_id <= 0;
            cmp_error <= 0;
        end else begin
            if (req_valid && req_ready) begin
                busy <= 1;
                remaining <= 2; // completion at acceptance edge + 3
                cmp_id <= req_id;
                cmp_error <= req_bytes == 0 || req_address >= ARENA_BYTES ||
                             req_bytes > ARENA_BYTES - req_address;
            end
            if (busy && remaining != 0) remaining <= remaining - 1;
            if (cmp_valid && cmp_ready) busy <= 0;
        end
    end
endmodule
