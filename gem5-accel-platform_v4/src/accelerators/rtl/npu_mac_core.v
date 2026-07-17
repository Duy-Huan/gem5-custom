// npu_mac_core.v
//
// A small, realistic streaming MAC (multiply-accumulate) core, standing in
// for "the real RTL block the hardware team hands over" in this example.
// Interface style is deliberately simple/generic (clk/rst_n, a start pulse,
// a valid/ready streaming input, a done pulse + result register) since this
// is exactly the kind of handshake most real accelerator IP exposes.
//
// Behavior: on `start`, the core accepts `length` pairs of (in_a, in_b) one
// per cycle (subject to in_valid/in_ready handshake), accumulates
// sum(in_a[i] * in_b[i]) into `result`, and pulses `done` for one cycle once
// the last element has been consumed and the final product added.
`timescale 1ns/1ps

module npu_mac_core #(
    parameter DATA_W = 16,
    parameter ACC_W  = 32
) (
    input  wire                  clk,
    input  wire                  rst_n,

    input  wire                  start,   // pulse: begin a new op
    input  wire [15:0]           length,  // number of (a,b) pairs this op

    input  wire                  in_valid,
    output wire                  in_ready,
    input  wire [DATA_W-1:0]     in_a,
    input  wire [DATA_W-1:0]     in_b,

    output reg                   done,
    output reg  [ACC_W-1:0]      result
);

    localparam S_IDLE = 2'd0,
               S_RUN  = 2'd1,
               S_DONE = 2'd2;

    reg [1:0]  state;
    reg [15:0] count;      // elements consumed so far
    reg [15:0] length_r;
    reg [ACC_W-1:0] acc;

    assign in_ready = (state == S_RUN);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state    <= S_IDLE;
            count    <= 16'd0;
            length_r <= 16'd0;
            acc      <= {ACC_W{1'b0}};
            done     <= 1'b0;
            result   <= {ACC_W{1'b0}};
        end else begin
            done <= 1'b0; // done is a 1-cycle pulse unless re-asserted below

            case (state)
                S_IDLE: begin
                    if (start) begin
                        state    <= S_RUN;
                        count    <= 16'd0;
                        length_r <= length;
                        acc      <= {ACC_W{1'b0}};
                    end
                end

                S_RUN: begin
                    if (in_valid && in_ready) begin
                        // Signed 16x16 -> 32-bit multiply-accumulate.
                        acc   <= acc + ($signed(in_a) * $signed(in_b));
                        count <= count + 16'd1;
                        if (count + 16'd1 == length_r) begin
                            state <= S_DONE;
                        end
                    end
                end

                S_DONE: begin
                    result <= acc;
                    done   <= 1'b1;
                    state  <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule
