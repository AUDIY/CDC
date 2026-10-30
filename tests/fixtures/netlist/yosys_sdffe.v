// Minimal post-yosys netlist using the sync-reset-with-enable flops that
// previously fell through to Unknown ($_SDFFE_PP0P_, $_SDFFCE_PN0P_).
// Both must classify as sequential and appear as registers in the graph.
module yosys_sdffe (
  input clk,
  input en,
  input rst,
  input d,
  output q1,
  output q2
);
  wire n1, n2;
  \$_SDFFE_PP0P_  ff0 (.D(d), .C(clk), .R(rst), .E(en), .Q(n1));
  \$_SDFFCE_PN0P_ ff1 (.D(n1), .C(clk), .R(rst), .E(en), .Q(n2));
  assign q1 = n1;
  assign q2 = n2;
endmodule
