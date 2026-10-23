#ifndef OPENCDC_IR_COMPACT_H
#define OPENCDC_IR_COMPACT_H

#include <cstddef>

namespace opencdc::ir {

class Graph;

// Post-frontend compaction for gate-level netlist graphs (netlist input only;
// the RTL frontend never calls this, so golden RTL behaviour is untouched).
//
// Large synthesized netlists are full of identity buffer cells and
// single-driver / single-consumer "pass-through" nets that only add nodes and
// edges without changing reachability. Crossing analysis walks these chains
// repeatedly (one backward traversal per destination register), so collapsing
// them directly reduces traversal cost and graph memory on big designs.
//
// What it removes:
//   - Combinational nodes with LogicType::Buf (pure buffers / identity cells).
//   - Net nodes with exactly one driver and exactly one consumer (pass-through).
//
// What it never touches:
//   - Ports and registers (kept by kind).
//   - Any node reachable *backward* from a register's clock-domain or reset
//     signal (plus those anchor nets themselves): the whole clock / reset
//     structure — ICG AND gates, clock muxes, enables, clock ports — is left
//     byte-for-byte intact so ClockResolver, gated detection, and root-clock
//     tracing behave exactly as before.
//   - Non-buffer combinational logic (AND/OR/XOR/NOT/...): pattern detectors
//     (gray encoder, handshake) rely on it, and inverters in particular are
//     kept because removing a lone inverter flips polarity unless even-chain
//     parity is tracked.
//
// The operation is connectivity-preserving: every path between surviving nodes
// that used to pass through a removed node is replaced by a direct edge, so
// register_predecessors / register_successors / find_register_paths reach the
// same registers and CDC findings are unchanged. Returns the number of nodes
// removed (0 when there was nothing safe to collapse).
size_t compact_graph(Graph& graph);

}  // namespace opencdc::ir

#endif  // OPENCDC_IR_COMPACT_H
