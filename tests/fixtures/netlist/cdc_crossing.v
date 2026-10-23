// Hand-written gate-level netlist: unsynchronized CDC crossing (CDC001).
// Source flop in clk_a domain drives a dest flop in clk_b with no synchronizer.
module cdc_crossing (
  input clk_a,
  input clk_b,
  input d,
  output q
);
  wire src;
  DFF_X1 src_reg (.CK(clk_a), .D(d), .Q(src));
  DFF_X1 dst_reg (.CK(clk_b), .D(src), .Q(q));
endmodule
