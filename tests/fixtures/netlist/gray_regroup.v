// A synthesized gray-coded bus crossing. The RTL `gray_src_reg <= d ^ (d>>1)`
// encodes adjacent-bit XORs (d[i] ^ d[i+1]) into the flops of
// \gray_src_reg_reg[i]  (the top bit folds to a direct port feed exactly the
// way synthesis folds it); \dst_reg_reg[i]  captures the crossing untouched.
//
// Structural gray recovery must re-annotate the source register as
// gray-coded so CDC002 is NOT emitted — the same verdict the RTL analysis
// reaches from the gray assignment — while CDC001 still reports the (safe)
// crossing once, mirrored by the merged wide register.
module gray_regroup (
  input clk_a,
  input clk_b,
  input [3:0] d,
  output [3:0] q
);
  wire [3:0] gray_enc;
  wire [3:0] gray_src_reg;
  XOR2_X1 e0 (.A(d[0]), .B(d[1]), .Y(gray_enc[0]));
  XOR2_X1 e1 (.A(d[1]), .B(d[2]), .Y(gray_enc[1]));
  XOR2_X1 e2 (.A(d[2]), .B(d[3]), .Y(gray_enc[2]));
  DFF_X1 \gray_src_reg_reg[0]  (.C(clk_a), .D(gray_enc[0]), .Q(gray_src_reg[0]));
  DFF_X1 \gray_src_reg_reg[1]  (.C(clk_a), .D(gray_enc[1]), .Q(gray_src_reg[1]));
  DFF_X1 \gray_src_reg_reg[2]  (.C(clk_a), .D(gray_enc[2]), .Q(gray_src_reg[2]));
  DFF_X1 \gray_src_reg_reg[3]  (.C(clk_a), .D(d[3]), .Q(gray_src_reg[3]));
  DFF_X1 \dst_reg_reg[0]  (.C(clk_b), .D(gray_src_reg[0]), .Q(q[0]));
  DFF_X1 \dst_reg_reg[1]  (.C(clk_b), .D(gray_src_reg[1]), .Q(q[1]));
  DFF_X1 \dst_reg_reg[2]  (.C(clk_b), .D(gray_src_reg[2]), .Q(q[2]));
  DFF_X1 \dst_reg_reg[3]  (.C(clk_b), .D(gray_src_reg[3]), .Q(q[3]));
endmodule
