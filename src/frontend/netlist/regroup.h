#ifndef OPENCDC_FRONTEND_NETLIST_REGROUP_H
#define OPENCDC_FRONTEND_NETLIST_REGROUP_H

#include <cstddef>
#include <string>
#include <vector>

namespace opencdc::ir {
class Graph;
}

namespace opencdc::frontend::netlist {

// Multi-bit bus regrouping.
//
// Synthesis bit-blasts every RTL register into per-bit flops named
// `<base>[<bit>]` (yosys emits `\src_reg_reg[0] `; DC names are similar).
// The analyzer's multi-bit reasoning (CDC002, bus_width evidence, per-pair
// counts) expects RTL register granularity: one width-8 node, not 8
// width-1 nodes. This pass re-merges bit flops into a single wide register:
//   - the merged node is named `<base>` and its width is max_bit_index + 1,
//   - clock/reset/polarity/latch properties are copied from the bit flops
//     (they must agree, see below),
//   - every edge of every bit flop is re-pointed at the merged node, so no
//     connectivity is fabricated or lost,
//   - the per-bit data nets are left in place (single-driver/single-consumer
//     ones are later collapsed by ir::compact_graph, which preserves ports,
//     registers, clock/reset cones and hence all findings).
//
// Grouping rule (conservative): flops merge only when the group i) shares
// the same `<base>[<i>]` name and ii) agrees on clock domain, reset signal
// and polarity, synchronicity, and latch-ness. Groups that fail any of
// these, single-bit `<base>[i]` registers, and groups whose merged name
// would collide with an existing node stay bit-blasted (the pre-regroup
// behavior, always correct, just coarser).
//
// Returns the number of bit flops merged into wider registers (0 when no
// group qualified). `warnings` receives one entry per skipped name
// collision, so silent under-regrouping stays visible.
size_t regroup_buses(ir::Graph& graph, std::vector<std::string>& warnings);

// Structural gray-code recovery for gate-level netlists.
//
// RTL pattern recognition marks a register `is_gray_coded` when its
// assignment is the canonical binary→gray transform `x ^ (x >> 1)` (CDC002
// then treats the crossing as safe per-bit-skew-wise). Synthesis destroys
// that AST knowledge, leaving only its circuit shape:
//     reg[i] = base[i] ^ base[i+1]   (adjacent-bit XOR of one common bus)
// This pass re-recognizes that shape conservatively and re-annotates the
// register so netlist analyses reach the same CDC002 verdict as RTL:
//   - every bit of a `<base>[i]` group must be driven by either an
//     adjacent-same-base XOR, a direct reference to that same base, or (for
//     constant-folded boundary bits) a direct register feed, whose net
//     identity is not observable post-synthesis,
//   - at most one bit may be non-XOR (the boundary bit),
//   - all observable bit input pairs must be adjacent indices of the single
//     common base; a group whose bits use two different bases never
//     qualifies (this is how XOR/parity trees are rejected),
//   - single-bit registers never qualify (width-1 crossings are already
//     opaque to CDC002, so honoring them costs nothing and risks nothing).
//
// Must run BEFORE regroup_buses (it annotates the bit flops that regrouping
// folds into the wide register). Returns the number of register groups
// annotated. Netlist-only: the RTL path never sets `is_gray_coded` here.
size_t annotate_gray_registers(ir::Graph& graph);

}  // namespace opencdc::frontend::netlist

#endif  // OPENCDC_FRONTEND_NETLIST_REGROUP_H
