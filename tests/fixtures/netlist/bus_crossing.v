// A bit-blasted 4-bit bus crossing (as yosys emits after `synth`). Each source
// bit must reach only the matching destination bit: 4 crossings, not 16. This
// guards against collapsing `bus[i]` into a single shared net node, which would
// fabricate cross-bit paths.
module bus_crossing (
  input clk_a,
  input clk_b,
  input [3:0] d,
  output [3:0] q
);
  wire [3:0] m;
  DFF_X1 u0 (.CK(clk_a), .D(d[0]), .Q(m[0]));
  DFF_X1 u1 (.CK(clk_a), .D(d[1]), .Q(m[1]));
  DFF_X1 u2 (.CK(clk_a), .D(d[2]), .Q(m[2]));
  DFF_X1 u3 (.CK(clk_a), .D(d[3]), .Q(m[3]));
  DFF_X1 v0 (.CK(clk_b), .D(m[0]), .Q(q[0]));
  DFF_X1 v1 (.CK(clk_b), .D(m[1]), .Q(q[1]));
  DFF_X1 v2 (.CK(clk_b), .D(m[2]), .Q(q[2]));
  DFF_X1 v3 (.CK(clk_b), .D(m[3]), .Q(q[3]));
endmodule
