// A crossing hidden behind module boundaries: the two flops live in a
// submodule instantiated twice on different clocks. Hierarchical elaboration
// must flatten them to `u_src.ff0` -> `u_dst.ff0` so the crossing is visible.
// `u_src` uses positional port connections (child port order: clk, d, q);
// `u_dst` uses named connections.
module hier_leaf (
  input clk,
  input d,
  output q
);
  DFF_X1 ff0 (
    .C(clk),
    .D(d),
    .Q(q)
  );
endmodule

module hier_crossing (
  input clk_a,
  input clk_b,
  input d,
  output q
);
  wire n1;
  hier_leaf u_src (
    clk_a,
    d,
    n1
  );
  hier_leaf u_dst (
    .clk(clk_b),
    .d(n1),
    .q(q)
  );
endmodule
