// A synthesized (yosys-style) 4-bit bus crossing: the RTL register pair
// `src_reg`/`dst_reg` was bit-blasted into per-bit flops named `name[i]`
// (escaped identifiers, as `write_verilog` emits them), with per-bit Q nets
// `src_reg[i]`. Bus regrouping must re-merge each group into ONE wide
// register, so the crossing is reported exactly as the pre-synthesis RTL
// analysis reports it: one CDC001 and one CDC002 (width 4), not 4 narrow
// crossings.
module bus_regroup (
  input clk_a,
  input clk_b,
  input [3:0] d,
  output [3:0] q
);
  wire [3:0] src_reg;
  DFF_X1 \src_reg_reg[0]  (.C(clk_a), .D(d[0]), .Q(src_reg[0]));
  DFF_X1 \src_reg_reg[1]  (.C(clk_a), .D(d[1]), .Q(src_reg[1]));
  DFF_X1 \src_reg_reg[2]  (.C(clk_a), .D(d[2]), .Q(src_reg[2]));
  DFF_X1 \src_reg_reg[3]  (.C(clk_a), .D(d[3]), .Q(src_reg[3]));
  DFF_X1 \dst_reg_reg[0]  (.C(clk_b), .D(src_reg[0]), .Q(q[0]));
  DFF_X1 \dst_reg_reg[1]  (.C(clk_b), .D(src_reg[1]), .Q(q[1]));
  DFF_X1 \dst_reg_reg[2]  (.C(clk_b), .D(src_reg[2]), .Q(q[2]));
  DFF_X1 \dst_reg_reg[3]  (.C(clk_b), .D(src_reg[3]), .Q(q[3]));
endmodule
