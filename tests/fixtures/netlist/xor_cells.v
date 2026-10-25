// Regression fixture for combinational logic-type inference: pattern names
// are matched best-effort, and XOR/XNOR contain "OR" (and "NAND" contains
// "AND") as substrings, so the match order decides. A cell named `XOR2_X1`
// must classify as LogicType::Xor (not Or) and `NAND2_X1` as And.
module xor_cells (
  input a,
  input b,
  output y,
  output shifted
);
  wire n1;
  XOR2_X1 g (.A(a), .B(b), .Y(n1));
  BUF_X1 hb (.A(n1), .Y(y));
  NAND2_X1 hn (.A(a), .B(b), .Y(shifted));
endmodule
