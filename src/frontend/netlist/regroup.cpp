#include "frontend/netlist/regroup.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_set>

#include "ir/graph.h"

namespace opencdc::frontend::netlist {

namespace {

using ir::EdgeRole;
using ir::LogicType;
using ir::Node;
using ir::NodeKind;

// Largest plausible bit index / bus width mirroring an RTL declaration.
// Anything above this is a parse artifact or a pathological name, not a real
// bus, and would allocate a nonsensical width — skip it (the bit flops stay
// bit-blasted, which is always correct, just coarser).
constexpr long kMaxBitIndex = 1000000;

// `dst_reg_reg[12]` -> ("dst_reg_reg", 12). Range selects (`[7:0]`), missing
// indices, non-digit indices, and bare names are not single-bit flops and
// are rejected.
bool split_bit_suffix(const std::string& name, std::string* base, long* index) {
    if (name.size() < 3 || name.back() != ']')
        return false;
    size_t open = name.rfind('[');
    if (open == std::string::npos || open == 0)
        return false;
    size_t first_digit = open + 1;
    if (first_digit + 1 >= name.size())
        return false;  // "[x]" empty or single-char-only-no-digit forms
    for (size_t i = first_digit; i + 1 < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9')
            return false;
    }
    long idx = 0;
    for (size_t i = first_digit; i + 1 < name.size(); ++i) {
        idx = idx * 10 + (name[i] - '0');
        if (idx > kMaxBitIndex)
            return false;
    }
    if (base)
        *base = name.substr(0, open);
    if (index)
        *index = idx;
    return true;
}

// Group identity: same base name and identical clock/reset behaviour. Two
// bit flops that disagree on any of these are separate registers by
// definition and must not be merged (a shared prefix alone proves nothing —
// `data[3]` and `data[7]` could belong to differently-clocked sub-buses).
// Encoded as one string so std::map keeps an order-stable iteration
// (deterministic node-id assignment for merged registers).
std::string group_key(const Node& n, const std::string& base) {
    std::string k = base;
    k.push_back('\x1f');
    k += n.clock_domain;
    k.push_back('\x1f');
    k += n.reset_signal;
    k.push_back('\x1f');
    k.push_back(static_cast<char>(
        (n.reset_pol == ir::ResetPolarity::ActiveLow ? 'l' : 'h') +
        (n.is_async_reset ? 0x10 : 0) + (n.is_latch ? 0x40 : 0)));
    k.push_back('\x1f');
    k += n.module_path;
    return k;
}

struct MergeCandidate {
    std::string base;
    long width = 0;                       // max index + 1
    std::vector<uint64_t> member_ids;     // bit flops to fold in
};

// Build the merge plan (returned in deterministic base-name order). Only
// groups with >= 2 agreeing members qualify; everything else stays
// bit-blasted.
std::map<std::string, MergeCandidate> collect_groups(const ir::Graph& graph) {
    // base -> (key -> members); several clock/reset variants of one base are
    // separate candidates, and only one wins the shared merged name (the
    // rest are rejected later on collision, deterministically by map order).
    std::map<std::string,
             std::map<std::string, std::map<long, uint64_t>>>
        candidates;

    for (const auto& node : graph.nodes()) {
        if (node.kind != NodeKind::Register || node.width != 1)
            continue;
        std::string base;
        long idx = 0;
        if (!split_bit_suffix(node.hier_name, &base, &idx))
            continue;
        auto& variants = candidates[base];
        std::string key = group_key(node, base);
        auto& slots = variants[key];
        auto& existing = slots[idx];
        if (existing != 0) {
            // Duplicate `base[i]` names cannot occur (name_to_idx_ is
            // unique); defensive skip keeps the plan collision-free anyway.
            continue;
        }
        existing = node.id;
    }

    std::map<std::string, MergeCandidate> plan;
    for (auto& [base, variants] : candidates) {
        for (auto& [key, slots] : variants) {
            if (slots.size() < 2)
                continue;
            MergeCandidate mc;
            mc.base = base;
            mc.width = static_cast<long>(slots.rbegin()->first) + 1;
            for (auto& [idx, id] : slots)
                mc.member_ids.push_back(id);
            // Map order (key then base) makes which-variant-wins stable.
            plan.emplace(key + '\x00' + base, std::move(mc));
        }
    }
    return plan;
}

// Fold one group: create the wide register, re-point every member edge at
// it, and return the member ids for batch removal.
MergeCandidate* apply_group(ir::Graph& graph, MergeCandidate& mc,
                            std::unordered_set<uint64_t>& member_set,
                            std::vector<std::string>& warnings) {
    const Node* first = graph.find_node(mc.member_ids[0]);
    if (!first)
        return nullptr;

    if (graph.find_node_by_name(mc.base)) {
        // A whole-bus net/port/other register already owns the merged name
        // (cannot reuse aliased node identity across node kinds). Leave the
        // group bit-blasted and say so.
        warnings.push_back(
            "netlist: skipped bus regrouping of '" + mc.base +
            "[...]' register flops; node name '" + mc.base +
            "' is already in use");
        return nullptr;
    }

    uint64_t merged_id = graph.add_register(
        mc.base, first->clock_domain, static_cast<uint32_t>(mc.width),
        first->loc, first->module_path, first->module_type);
    if (merged_id == 0) {
        warnings.push_back("netlist: graph limit hit while regrouping '" +
                           mc.base + "[...]'");
        return nullptr;
    }
    Node* merged = graph.find_node_mutable(merged_id);
    if (!merged)
        return nullptr;

    // Carry register attributes over: the bit flops of one bus share their
    // reset (the group key guarantees identical clock/reset fields), so they
    // are taken from the lowest-index member; pattern flags are OR-ed so no
    // annotation from any bit is lost.
    bool have_first = false;
    for (uint64_t id : mc.member_ids) {
        const Node* m = graph.find_node(id);
        if (!m)
            continue;
        if (!have_first) {
            merged->reset_signal = m->reset_signal;
            merged->reset_pol = m->reset_pol;
            merged->is_async_reset = m->is_async_reset;
            merged->is_latch = m->is_latch;
            have_first = true;
        }
        merged->is_gray_coded = merged->is_gray_coded || m->is_gray_coded;
        merged->is_handshake_signal =
            merged->is_handshake_signal || m->is_handshake_signal;
        merged->is_async_fifo_ptr =
            merged->is_async_fifo_ptr || m->is_async_fifo_ptr;
        merged->is_control_signal =
            merged->is_control_signal || m->is_control_signal;
    }

    // Re-point edges. Register incident edges are always Data (the builder
    // adds D-input and Q-output data edges only), so roles are preserved by
    // construction. Register-to-register edges cannot occur (registers meet
    // only through data nets); the member guard keeps the rebuild safe if
    // that ever changes.
    for (uint64_t id : mc.member_ids) {
        for (uint64_t p : graph.predecessors(id)) {
            if (member_set.count(p))
                continue;
            graph.add_edge(p, merged_id, EdgeRole::Data);
        }
        for (uint64_t s : graph.successors(id)) {
            if (member_set.count(s))
                continue;
            graph.add_edge(merged_id, s, EdgeRole::Data);
        }
    }
    return &mc;
}

// ---- Structural gray-code recovery -----------------------------------------

// Direct data drive of a flop bit: either a same-base adjacency XOR (gray
// transform shape), a direct reference that pins the common base, or a
// neutral register feed (constant-folded boundary bit whose net identity is
// not observable post-synthesis).
struct GrayDrive {
    bool valid = false;
    bool is_xor = false;
    std::string base;  // contributed common base ("" for neutral feeds)
};

// Max upward walk through identity buffer cells / pass-through nets. Gray
// encoders synthesise as a driver directly on each data pin; anything beyond
// this depth is ordinary logic and must NOT be honoured (never assume safe).
constexpr int kGrayWalkDepth = 4;

// Strip a trailing `[i]` select ("bin[3]" -> "bin"); bare names pass through.
std::string base_of(const std::string& name) {
    std::string base;
    if (split_bit_suffix(name, &base, nullptr))
        return base;
    return name;
}

GrayDrive classify_bit_drive(const ir::Graph& graph, uint64_t flop_id) {
    auto preds = graph.predecessors(flop_id);
    if (preds.size() != 1)
        return {};  // muxed/ambiguous data input is never a clean encoder
    uint64_t cur = preds[0];
    for (int depth = 0; depth <= kGrayWalkDepth; ++depth) {
        const Node* n = graph.find_node(cur);
        if (!n)
            return {};
        if (n->kind == NodeKind::Register)
            return GrayDrive{true, false, ""};  // neutral: identity unobservable
        if (n->kind == NodeKind::Port)
            return GrayDrive{true, false, base_of(n->hier_name)};
        if (n->kind == NodeKind::Net) {
            auto d = graph.predecessors(cur);
            if (d.size() != 1)
                return {};  // multi-driven or undriven net: stop
            cur = d[0];
            continue;
        }
        if (n->kind == NodeKind::Combinational) {
            if (n->logic_type == LogicType::Buf) {
                if (n->logic_inputs.size() != 1)
                    return {};
                cur = n->logic_inputs[0];
                continue;
            }
            if (n->logic_type != LogicType::Xor || n->logic_inputs.size() != 2)
                return {};  // and/or/mux/unknown: not a gray transform — stop
            const Node* a = graph.find_node(n->logic_inputs[0]);
            const Node* b = graph.find_node(n->logic_inputs[1]);
            if (!a || !b)
                return {};
            std::string ba = base_of(a->hier_name);
            std::string bb = base_of(b->hier_name);
            if (ba.empty() || bb.empty() || ba != bb)
                return {};  // mixed-bus XOR (parity trees, adders): reject
            long ia = 0, ib = 0;
            bool ia_ok = split_bit_suffix(a->hier_name, nullptr, &ia);
            bool ib_ok = split_bit_suffix(b->hier_name, nullptr, &ib);
            if (ia_ok && ib_ok && std::abs(ia - ib) != 1)
                return {};  // same base but non-adjacent bits: not `x >> 1`
            // Adjacency is not observable when the inputs are top-level port
            // selects (they collapse to the single port node) — accepted, it
            // is the same structural fact the RTL analysis also cannot
            // disprove.
            return GrayDrive{true, true, ba};
        }
        return {};
    }
    return {};
}

}  // namespace

size_t regroup_buses(ir::Graph& graph, std::vector<std::string>& warnings) {
    auto plan = collect_groups(graph);
    if (plan.empty())
        return 0;

    std::vector<uint64_t> dead;
    size_t merged_bits = 0;
    for (auto& [key, mc] : plan) {
        std::unordered_set<uint64_t> member_set(mc.member_ids.begin(),
                                                mc.member_ids.end());
        if (!apply_group(graph, mc, member_set, warnings))
            continue;
        for (uint64_t id : mc.member_ids) {
            dead.push_back(id);
            ++merged_bits;
        }
    }

    if (!dead.empty())
        graph.remove_nodes(dead);
    return merged_bits;
}

size_t annotate_gray_registers(ir::Graph& graph) {
    // Group bit flops with the exact key regrouping uses, so flags set here
    // survive the merge (regroup_buses ORs member flags into the wide node).
    std::map<std::string, std::vector<uint64_t>> groups;
    for (const auto& node : graph.nodes()) {
        if (node.kind != NodeKind::Register || node.width != 1)
            continue;
        std::string base;
        long idx = 0;
        if (!split_bit_suffix(node.hier_name, &base, &idx))
            continue;
        groups[group_key(node, base)].push_back(node.id);
    }

    size_t flagged = 0;
    for (auto& [key, members] : groups) {
        size_t members_n = members.size();
        if (members_n < 2)
            continue;  // width-1 crossings are CDC002-inert; never annotate
        std::unordered_set<std::string> bases;  // contributed input bases
        size_t xor_bits = 0;
        bool all_valid = true;
        for (uint64_t id : members) {
            GrayDrive d = classify_bit_drive(graph, id);
            if (!d.valid) {
                all_valid = false;
                break;
            }
            if (d.is_xor) {
                ++xor_bits;
                bases.insert(d.base);
            } else if (!d.base.empty()) {
                bases.insert(d.base);
            }
        }
        // The canonical encoder: every bit is an adjacent-same-base XOR
        // except at most the folded boundary bit, and all contributed bases
        // agree on one common bus. Anything wider in shape — parity trees,
        // adders, mixed-bus XOR chains — is rejected here.
        if (all_valid && xor_bits >= 1 && xor_bits >= members_n - 1 &&
            bases.size() == 1) {
            for (uint64_t id : members) {
                if (Node* n = graph.find_node_mutable(id))
                    n->is_gray_coded = true;
            }
            ++flagged;
        }
    }
    return flagged;
}

}  // namespace opencdc::frontend::netlist
