// A vendor flop absent from the cell library. Its name looks sequential
// ("*_FF_*"), so the frontend must flag it: analysis_status becomes
// "incomplete" and --signoff reports INCOMPLETE — never a silent "complete"
// while the flop's crossings are missing from the graph.
module unknown_sequential_cell (
  input clk_a,
  input clk_b,
  input d,
  output q
);
  wire m, n;
  DFF_X1 src (.CK(clk_a), .D(d), .Q(m));
  VENDOR_FF_X1 u_ff (.CLK(clk_a), .D(m), .Q(n));
  DFF_X1 dst (.CK(clk_b), .D(n), .Q(q));
endmodule
