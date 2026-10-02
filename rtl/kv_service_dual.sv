// Two unchanged service engines, partitioned by direction. fifo_depth is the
// TOTAL waiting capacity (even, 2..64), split equally between the two queues.
// The shared completion port transfers at most one result per cycle.
module kv_service_dual (
    input logic clk, reset,
    input logic [31:0] fifo_depth, service_cycles,
    input logic [63:0] arena_bytes,
    input logic allow_request,
    input logic [1:0] test_fault,
    input logic req_valid,
    output logic req_ready,
    input logic [63:0] req_id, req_address, req_bytes,
    input logic req_write,
    output logic cmp_valid,
    input logic cmp_ready,
    output logic [63:0] cmp_id,
    output logic cmp_error,
    output logic busy,
    output logic [31:0] queue_count,
    output logic service_start,
    output logic [63:0] service_id,
    output logic read_busy, write_busy, read_completion_wait, write_completion_wait,
    output logic [31:0] read_queue_count, write_queue_count,
    output logic read_service_start, write_service_start,
    output logic [63:0] read_service_id, write_service_id
);
    logic r_ready, w_ready, r_valid, w_valid, r_cmp_ready, w_cmp_ready;
    logic [63:0] r_id, w_id;
    logic r_error, w_error;
    logic prefer_write, locked, locked_write, select_write;

    assign req_ready = req_write ? w_ready : r_ready;
    assign busy = read_busy || write_busy;
    assign queue_count = read_queue_count + write_queue_count;
    // Legacy single-engine fields; simultaneous starts are recorded separately.
    assign service_start = read_service_start || write_service_start;
    assign service_id = read_service_start ? read_service_id : write_service_id;
    assign select_write = locked ? locked_write : (w_valid && (!r_valid || prefer_write));
    assign cmp_valid = !reset && (select_write ? w_valid : r_valid);
    assign cmp_id = select_write ? w_id : r_id;
    assign cmp_error = select_write ? w_error : r_error;
    assign r_cmp_ready = !reset && cmp_ready && !select_write;
    assign w_cmp_ready = !reset && cmp_ready && select_write;
    assign read_completion_wait = r_valid && !r_cmp_ready;
    assign write_completion_wait = w_valid && !w_cmp_ready;

    always_ff @(posedge clk) begin
        if (reset) begin
            prefer_write <= 0;
            locked <= 0;
            locked_write <= 0;
        end else if (cmp_valid) begin
            if (cmp_ready) begin
                prefer_write <= !select_write;
                locked <= 0;
            end else begin
                locked <= 1;
                locked_write <= select_write;
            end
        end
    end

    kv_service read_engine (
        .clk(clk), .reset(reset), .fifo_depth(fifo_depth >> 1),
        .service_cycles(service_cycles), .arena_bytes(arena_bytes),
        .allow_request(allow_request), .test_fault(test_fault),
        .req_valid(req_valid && !req_write), .req_ready(r_ready),
        .req_id(req_id), .req_address(req_address), .req_bytes(req_bytes), .req_write(1'b0),
        .cmp_valid(r_valid), .cmp_ready(r_cmp_ready), .cmp_id(r_id), .cmp_error(r_error),
        .busy(read_busy), .queue_count(read_queue_count),
        .service_start(read_service_start), .service_id(read_service_id),
        // The parent uses aggregate ports of each direction-specific child.
        /* verilator lint_off PINCONNECTEMPTY */
        .read_busy(), .write_busy(), .read_completion_wait(), .write_completion_wait(),
        .read_queue_count(), .write_queue_count(), .read_service_start(), .write_service_start(),
        .read_service_id(), .write_service_id()
        /* verilator lint_on PINCONNECTEMPTY */
    );
    kv_service write_engine (
        .clk(clk), .reset(reset), .fifo_depth(fifo_depth >> 1),
        .service_cycles(service_cycles), .arena_bytes(arena_bytes),
        .allow_request(allow_request), .test_fault(test_fault),
        .req_valid(req_valid && req_write), .req_ready(w_ready),
        .req_id(req_id), .req_address(req_address), .req_bytes(req_bytes), .req_write(1'b1),
        .cmp_valid(w_valid), .cmp_ready(w_cmp_ready), .cmp_id(w_id), .cmp_error(w_error),
        .busy(write_busy), .queue_count(write_queue_count),
        .service_start(write_service_start), .service_id(write_service_id),
        /* verilator lint_off PINCONNECTEMPTY */
        .read_busy(), .write_busy(), .read_completion_wait(), .write_completion_wait(),
        .read_queue_count(), .write_queue_count(), .read_service_start(), .write_service_start(),
        .read_service_id(), .write_service_id()
        /* verilator lint_on PINCONNECTEMPTY */
    );
endmodule
