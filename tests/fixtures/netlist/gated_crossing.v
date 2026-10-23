// Gated-clock domain crossing: ICG-gated source domain drives a flop in a
// second domain. CDC004 (gated_clock_crossing) should fire alongside CDC001.
module gated_crossing (
  input clk,
  input en,
  input clk_b,
  input d,
  output q
);
  wire clk_gated, g;
  CLKGATE_X1 u_gate (.CLK(clk), .EN(en), .GCK(clk_gated));
  DFF_X1 greg (.CK(clk_gated), .D(d), .Q(g));
  DFF_X1 dst  (.CK(clk_b),     .D(g), .Q(q));
endmodule
