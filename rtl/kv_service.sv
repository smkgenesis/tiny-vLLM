// Synthetic KV transaction service: descriptor FIFO plus one service engine.
// No data payload/storage or DRAM timing model. Configuration is reset-stable.
module kv_service (
    input logic clk, reset,
    input logic [31:0] fifo_depth, service_cycles,
    input logic [63:0] arena_bytes,
    input logic allow_request,
    input logic [1:0] test_fault, // 0 normal, 2 drop ID 1, 3 duplicate ID 1
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
    // Observability only: these signals do not affect admission or service.
    output logic read_busy, write_busy, read_completion_wait, write_completion_wait,
    output logic [31:0] read_queue_count, write_queue_count,
    output logic read_service_start, write_service_start,
    output logic [63:0] read_service_id, write_service_id
);
    localparam int MAX_DEPTH = 64;
    logic [63:0] ids [MAX_DEPTH];
    logic errors [MAX_DEPTH];
    logic writes [MAX_DEPTH];
    logic active_write;
    logic [5:0] read_ptr, write_ptr;
    logic duplicate_sent;
    logic [31:0] remaining;
    logic enqueue, dequeue, dropping;

    assign req_ready = !reset && allow_request && queue_count < fifo_depth;
    assign enqueue = req_valid && req_ready;
    assign dropping = test_fault == 2 && cmp_id == 1;
    assign cmp_valid = !reset && busy && remaining == 0 && !dropping;
    assign service_start = !reset && queue_count != 0 &&
        (!busy || (cmp_valid && cmp_ready &&
          !(test_fault == 3 && cmp_id == 1 && !duplicate_sent)));
    assign dequeue = service_start;
    assign service_id = queue_count != 0 ? ids[read_ptr] : 64'b0;

    // Both directions still have identical service time; no value model.
    assign read_busy = busy && !active_write;
    assign write_busy = busy && active_write;
    assign read_completion_wait = read_busy && cmp_valid && !cmp_ready;
    assign write_completion_wait = write_busy && cmp_valid && !cmp_ready;
    assign read_service_start = service_start && !writes[read_ptr];
    assign write_service_start = service_start && writes[read_ptr];
    assign read_service_id = read_service_start ? service_id : 64'b0;
    assign write_service_id = write_service_start ? service_id : 64'b0;

    always_ff @(posedge clk) begin
        if (reset) begin
            read_ptr <= 0;
            write_ptr <= 0;
            queue_count <= 0;
            busy <= 0;
            remaining <= 0;
            cmp_id <= 0;
            cmp_error <= 0;
            duplicate_sent <= 0;
            active_write <= 0;
            read_queue_count <= 0;
            write_queue_count <= 0;
        end else begin
            if (enqueue) begin
                ids[write_ptr] <= req_id;
                writes[write_ptr] <= req_write;
                errors[write_ptr] <= req_bytes == 0 || req_address >= arena_bytes ||
                                     req_bytes > arena_bytes - req_address;
                write_ptr <= write_ptr + 6'd1;
            end
            case ({enqueue, dequeue})
                2'b10: queue_count <= queue_count + 1;
                2'b01: queue_count <= queue_count - 1;
                default: ;
            endcase
            case ({enqueue && !req_write, read_service_start})
                2'b10: read_queue_count <= read_queue_count + 1;
                2'b01: read_queue_count <= read_queue_count - 1;
                default: ;
            endcase
            case ({enqueue && req_write, write_service_start})
                2'b10: write_queue_count <= write_queue_count + 1;
                2'b01: write_queue_count <= write_queue_count - 1;
                default: ;
            endcase
            if (busy && remaining != 0) remaining <= remaining - 1;
            if (busy && remaining == 0 && dropping) busy <= 0;
            if (cmp_valid && cmp_ready) begin
                if (test_fault == 3 && cmp_id == 1 && !duplicate_sent)
                    duplicate_sent <= 1;
                else busy <= 0;
            end
            if (dequeue) begin
                cmp_id <= ids[read_ptr];
                cmp_error <= errors[read_ptr];
                active_write <= writes[read_ptr];
                read_ptr <= read_ptr + 6'd1;
                busy <= 1;
                remaining <= service_cycles - 1;
            end
        end
    end
endmodule
