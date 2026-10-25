// A crossing whose synthesized register names (`u_data/ff_reg[i]`,
// `u_dst/ff_reg[i]`) no longer match the RTL names (`data_reg`, `dst_reg`).
// With namemap_crossing.map applied, findings must report the RTL names so
// RTL-authored waivers keep matching netlist runs.
module namemap_crossing (
  input clk_a,
  input clk_b,
  input [1:0] d,
  output [1:0] q
);
  wire [1:0] n7;
  DFF_X1 \u_data/ff_reg[0] (
    .C(clk_a),
    .D(d[0]),
    .Q(n7[0])
  );
  DFF_X1 \u_data/ff_reg[1] (
    .C(clk_a),
    .D(d[1]),
    .Q(n7[1])
  );
  DFF_X1 \u_dst/ff_reg[0] (
    .C(clk_b),
    .D(n7[0]),
    .Q(q[0])
  );
  DFF_X1 \u_dst/ff_reg[1] (
    .C(clk_b),
    .D(n7[1]),
    .Q(q[1])
  );
endmodule
