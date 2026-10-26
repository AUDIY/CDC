#ifndef OPENCDC_FRONTEND_NETLIST_FLATTEN_H
#define OPENCDC_FRONTEND_NETLIST_FLATTEN_H

#include <string>
#include <vector>

#include "frontend/netlist/netlist_parser.h"

namespace opencdc::frontend::netlist {

// Hierarchical netlist elaboration (flattening).
//
// Synthesized netlists are usually flat, but hand-written and some vendor
// netlists keep module boundaries: a parent instantiates child modules whose
// contents (registers, gates, nets) must still participate in the analysis.
// This pass elaborates the hierarchy into ONE flat module before graph
// building:
//
//   - instance names become name prefixes: a child instance `u_src`
//     containing `ff0` yields the flat register `u_src.ff0`,
//   - child ports are substituted through the parent-side pin connections
//     (named or positional, mapped in child port declaration order),
//   - names not bound upward keep their prefixed form (unconnected ports
//     become isolated `<prefix><port>` nets),
//   - sub-module definitions shadow CellLibrary cells of the same name
//     (an explicit definition in the design wins).
//
// Instantiation cycles are rejected with an error (module-name stack; this
// includes direct self-instantiation of the top module and indirect cycles
// back through the top), nesting deeper than 64 levels is an error, and
// positional connection counts that disagree with the child's port count
// warn. Flat netlists never copy anything: the caller skips this pass when
// references_submodules() returns false.

bool references_submodules(const NetlistModule& top, const std::vector<NetlistModule>& submodules);

// Produce the flattened module. Returns false and fills `errors` on
// instantiation cycles or hierarchy-depth overflow. `submodules` must NOT
// contain `top` itself.
bool flatten_hierarchy(const NetlistModule& top, const std::vector<NetlistModule>& submodules,
                       NetlistModule& out, std::vector<std::string>& errors,
                       std::vector<std::string>& warnings);

}  // namespace opencdc::frontend::netlist

#endif  // OPENCDC_FRONTEND_NETLIST_FLATTEN_H
