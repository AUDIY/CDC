// Hand-written gate-level netlist: proper 2FF synchronizer (CDC001 should be
// downgraded to info / verified-safe by the structural sync-chain detection).
module sync_2ff (
  input clk_a,
  input clk_b,
  input d,
  output q
);
  wire src, meta;
  DFF_X1 src_reg  (.CK(clk_a), .D(d),   .Q(src));
  DFF_X1 meta_reg (.CK(clk_b), .D(src), .Q(meta));
  DFF_X1 dst_reg  (.CK(clk_b), .D(meta), .Q(q));
endmodule
