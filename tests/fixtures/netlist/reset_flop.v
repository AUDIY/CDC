// Async active-low reset flop. Classified as dff_reset via the cell map so the
// register records reset_signal=rst_n (ActiveLow, async).
module reset_flop (
  input clk,
  input rst_n,
  input d,
  output q
);
  DFCNQ_X1 u_reg (.CK(clk), .CDN(rst_n), .D(d), .Q(q));
endmodule
