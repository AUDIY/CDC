// An IP cell absent from the cell library: it must be treated as opaque
// combinational (with a warning), never assumed safe.
module unknown_cell (
  input clk_a,
  input clk_b,
  input d,
  output q
);
  wire m, n;
  DFF_X1 src (.CK(clk_a), .D(d), .Q(m));
  MY_WEIRD_IP_X1 u_ip (.a(m), .y(n));
  DFF_X1 dst (.CK(clk_b), .D(n), .Q(q));
endmodule
