// A synthesized clock mux, the way yosys emits it: `$_MUX_` with
// Y = S ? B : A, so A is the default-path (select = 0) input. dst_ff_reg is
// clocked by the mux output. The representative root clock must be the
// default-path input A (clk_b) — the same root the RTL frontend resolves for
// `assign selected_clk = sel ? clk_a : clk_b` — never the select input.
module mux_clock (
  input clk_a,
  input clk_b,
  input clk_sel,
  input d,
  output q
);
  wire selected_clk;
  wire src_ff;
  $_MUX_ m0 (
    .A(clk_b),
    .B(clk_a),
    .S(clk_sel),
    .Y(selected_clk)
  );
  DFF_X1 src_ff_reg (
    .C(clk_a),
    .D(d),
    .Q(src_ff)
  );
  DFF_X1 dst_ff_reg (
    .C(selected_clk),
    .D(src_ff),
    .Q(q)
  );
endmodule
