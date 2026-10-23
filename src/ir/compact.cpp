#include "ir/compact.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/graph.h"

namespace opencdc::ir {
namespace {

// Nodes that must never be removed: ports, registers, the clock/reset anchor
// nets themselves, and everything reachable backward from those anchors (the
// clock / reset structure: gating ANDs, clock muxes, enables, clock ports).
// Backward traversal preserves exactly what ClockResolver and the CDC rules
// need to trace clocks and resets, while leaving pure data logic compactable.
std::unordered_set<uint64_t> compute_protected(const Graph& g) {
    std::unordered_set<uint64_t> prot;
    std::vector<uint64_t> stack;

    for (const auto& n : g.nodes()) {
        if (n.kind == NodeKind::Port || n.kind == NodeKind::Register) {
            prot.insert(n.id);
        }
    }

    // Seed the backward walk from every clock-domain and reset anchor net.
    for (const auto& n : g.nodes()) {
        for (const std::string* anchor : {&n.clock_domain, &n.reset_signal}) {
            if (anchor->empty())
                continue;
            if (const Node* a = g.find_node_by_name(*anchor)) {
                if (prot.insert(a->id).second)
                    stack.push_back(a->id);
            }
        }
    }

    // Backward closure: every ancestor of a clock / reset anchor is protected.
    while (!stack.empty()) {
        uint64_t id = stack.back();
        stack.pop_back();
        for (uint64_t p : g.predecessors(id)) {
            if (prot.insert(p).second)
                stack.push_back(p);
        }
    }
    return prot;
}

}  // namespace

size_t compact_graph(Graph& graph) {
    const auto& nodes = graph.nodes();
    if (nodes.empty())
        return 0;

    const std::unordered_set<uint64_t> prot = compute_protected(graph);

    // Collect removable candidates: identity buffers and pass-through nets,
    // never a protected node (which is also guaranteed not to be a port or
    // register by kind).
    std::vector<uint64_t> dead;
    for (const auto& n : nodes) {
        if (prot.count(n.id))
            continue;
        if (n.kind == NodeKind::Combinational && n.logic_type == LogicType::Buf) {
            dead.push_back(n.id);
        } else if (n.kind == NodeKind::Net) {
            if (graph.predecessors(n.id).size() == 1 && graph.successors(n.id).size() == 1)
                dead.push_back(n.id);
        }
    }
    if (dead.empty())
        return 0;

    const std::unordered_set<uint64_t> deadset(dead.begin(), dead.end());

    // Bypass edges: for every surviving node, connect across dead chains to the
    // first alive node(s) reachable forward. Computed against the *current*
    // edges (before removal) so connectivity is fully preserved. Memoized; an
    // in-progress set guards against pathological combinational loops. Returned
    // by value so no reference into the memo can dangle across an emplace.
    std::unordered_map<uint64_t, std::vector<uint64_t>> memo;
    std::unordered_set<uint64_t> inprog;

    std::function<std::vector<uint64_t>(uint64_t)> alive_fwd =
        [&](uint64_t start) -> std::vector<uint64_t> {
        auto it = memo.find(start);
        if (it != memo.end())
            return it->second;
        if (inprog.count(start))
            return {};  // cycle: break without memoizing a partial result
        inprog.insert(start);

        std::vector<uint64_t> out;
        for (uint64_t s : graph.successors(start)) {
            if (!deadset.count(s)) {
                out.push_back(s);
            } else {
                auto sub = alive_fwd(s);
                out.insert(out.end(), sub.begin(), sub.end());
            }
        }
        // Dedup while preserving order.
        std::unordered_set<uint64_t> seen;
        std::vector<uint64_t> dedup;
        dedup.reserve(out.size());
        for (uint64_t id : out) {
            if (seen.insert(id).second)
                dedup.push_back(id);
        }
        inprog.erase(start);
        memo.emplace(start, dedup);
        return dedup;
    };

    // Gather bypass edges first (structure referenced by id stays valid through
    // the removal step below because surviving node ids are stable).
    std::vector<std::pair<uint64_t, uint64_t>> bypass;
    for (const auto& n : nodes) {
        if (deadset.count(n.id))
            continue;
        for (uint64_t s : graph.successors(n.id)) {
            if (!deadset.count(s))
                continue;  // alive -> alive edge survives as-is
            for (uint64_t w : alive_fwd(s)) {
                if (w != n.id)
                    bypass.emplace_back(n.id, w);
            }
        }
    }

    // Drop the dead nodes, then add the bypass edges (both endpoints alive, so
    // add_edge's existence check passes). Removing first also keeps the edge
    // count from transiently exceeding the safety limit while bypasses queue up.
    graph.remove_nodes(dead);
    for (const auto& [from, to] : bypass)
        graph.add_edge(from, to);

    return dead.size();
}

}  // namespace opencdc::ir
