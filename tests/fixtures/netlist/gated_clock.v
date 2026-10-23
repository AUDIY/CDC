// Hand-written gate-level netlist: integrated clock gate (ICG) driving a flop
// in a gated domain. The gated net should be flagged (CDC004 when it crosses).
module gated_clock (
  input clk,
  input en,
  input d,
  output q
);
  wire clk_gated;
  CLKGATE_X1 u_gate (.CLK(clk), .EN(en), .GCK(clk_gated));
  DFF_X1 reg0 (.CK(clk_gated), .D(d), .Q(q));
endmodule
