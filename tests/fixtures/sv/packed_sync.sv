// Packed-vector 2FF synchronizer: one 2-bit register models both stages
// (`sync_b <= {sync_b[0], reg_a}`) and must be recognized exactly like the
// equivalent scalar two-register form. The reversed `hold_b` assignment is
// NOT a shift chain (bit 1 samples reg_a directly, bit 0 only holds) and
// must keep reporting CDC001 as an error.
`default_nettype none

module packed_sync (
    input  wire clk_a,
    input  wire clk_b,
    input  wire rst_n,
    input  wire data_in,
    output wire data_out,
    output wire hold_out
);

    reg       reg_a = 1'b0;
    reg [1:0] sync_b = 2'b00;
    reg [1:0] hold_b = 2'b00;

    always @(posedge clk_a or negedge rst_n) begin
        if (!rst_n)
            reg_a <= 1'b0;
        else
            reg_a <= data_in;
    end

    always @(posedge clk_b or negedge rst_n) begin
        if (!rst_n) begin
            sync_b <= 2'b00;
            hold_b <= 2'b00;
        end else begin
            sync_b <= {sync_b[0], reg_a};
            hold_b <= {reg_a, hold_b[0]};
        end
    end

    assign data_out = sync_b[1];
    assign hold_out = hold_b[1];

endmodule

`default_nettype wire
