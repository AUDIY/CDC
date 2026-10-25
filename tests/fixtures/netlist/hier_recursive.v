// Illegal: two modules instantiate each other. Hierarchical elaboration must
// reject the design with a 'recursive module instantiation' error instead of
// recursing forever.
module hier_a (
  input clk,
  input d,
  output q
);
  wire n1;
  DFF_X1 ff0 (
    .C(clk),
    .D(d),
    .Q(n1)
  );
  hier_b u_b (
    .clk(clk),
    .d(n1),
    .q(q)
  );
endmodule

module hier_b (
  input clk,
  input d,
  output q
);
  hier_a u_a (
    .clk(clk),
    .d(d),
    .q(q)
  );
endmodule
