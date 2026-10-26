// Low-power netlist: a 2-bit bus crosses clk_a -> clk_b through isolation
// cells, and the receiving side uses retention (save/restore) flops.
//   - Retention flops must be modeled as registers: they are the real
//     sequential elements; treating them as Unknown (opaque combinational)
//     would hide the crossing entirely.
//   - CDC001 is downgraded to info/VerifiedSafe with provenance because the
//     path crosses a low-power isolation cell (methodology-managed
//     power-domain boundary; UPF is out of scope), while CDC002 stays a full
//     error because it is independent of synchronizer presence.
//   - Both cell types must be recognized: no "unmapped cell" warning.
module low_power (
  input clk_a,
  input clk_b,
  input iso_en,
  input [1:0] d,
  output [1:0] q
);
  wire [1:0] src_q;
  wire [1:0] iso_out;

  DFF_X1 \src_reg[0] (
    .C(clk_a),
    .D(d[0]),
    .Q(src_q[0])
  );
  DFF_X1 \src_reg[1] (
    .C(clk_a),
    .D(d[1]),
    .Q(src_q[1])
  );

  ISO_X1 \iso[0] (
    .A(src_q[0]),
    .SE(iso_en),
    .Z(iso_out[0])
  );
  ISO_X1 \iso[1] (
    .A(src_q[1]),
    .SE(iso_en),
    .Z(iso_out[1])
  );

  RSDFFQ_X1 \dst_reg[0] (
    .CK(clk_b),
    .D(iso_out[0]),
    .Q(q[0])
  );
  RSDFFQ_X1 \dst_reg[1] (
    .CK(clk_b),
    .D(iso_out[1]),
    .Q(q[1])
  );
endmodule
