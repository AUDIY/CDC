#include "frontend/netlist/flatten.h"

#include <unordered_map>

namespace opencdc::frontend::netlist {

namespace {

// Real hierarchies sit far below this; deeper is almost certainly a broken
// or (partially) recursive netlist.
constexpr int kMaxDepth = 64;

using ModuleMap = std::unordered_map<std::string, const NetlistModule*>;

ModuleMap make_module_map(const std::vector<NetlistModule>& submodules) {
    ModuleMap map;
    for (const auto& m : submodules)
        map.emplace(m.name, &m);
    return map;
}

// A name inside a child module resolves either through the parent-side port
// connection (a reference already resolved in the parent context) or through
// the instance prefix (plain prefixed name).
struct Ctx {
    std::string prefix;  // "u_src." — empty at the top level
    std::unordered_map<std::string, NetRef> conns;
};

bool is_unbound(const NetRef& ref) {
    return ref.kind == NetRef::Kind::Net && ref.net.empty();
}

// String form of a resolved reference (matches GraphBuilder signal
// semantics: bit selects and concats keep their shape).
std::string ref_to_string(const NetRef& ref) {
    if (ref.kind == NetRef::Kind::Concat) {
        std::string s = "{";
        for (size_t i = 0; i < ref.concat.size(); ++i) {
            if (i)
                s += ",";
            s += ref.concat[i];
        }
        return s + "}";
    }
    if (ref.kind == NetRef::Kind::Const)
        return std::string("1'b") + ref.const_val;
    if (ref.has_bit)
        return ref.net + "[" + std::to_string(ref.bit) + "]";
    return ref.net;
}

// Resolve a reference in the given context: upward-bound names become the
// parent-side reference; everything else gets the instance prefix.
NetRef resolve_ref(const NetRef& ref, const Ctx& ctx) {
    switch (ref.kind) {
        case NetRef::Kind::Net: {
            auto it = ctx.conns.find(ref.net);
            if (it != ctx.conns.end())
                return it->second;
            NetRef out = ref;
            out.net = ctx.prefix + ref.net;
            return out;
        }
        case NetRef::Kind::Const:
            return ref;
        case NetRef::Kind::Concat: {
            NetRef out = ref;
            for (auto& member : out.concat) {
                auto it = ctx.conns.find(member);
                if (it != ctx.conns.end())
                    member = ref_to_string(it->second);
                else
                    member = ctx.prefix + member;
            }
            return out;
        }
    }
    return ref;
}

struct Elaborator {
    const ModuleMap& modules;
    NetlistModule& out;
    std::vector<std::string>& errors;
    std::vector<std::string>& warnings;
    std::vector<std::string> active{};  // module-name stack (cycle detection)

    bool emit(const NetlistModule& m, const Ctx& ctx) {
        if (static_cast<int>(active.size()) >= kMaxDepth) {
            errors.push_back("module hierarchy deeper than " + std::to_string(kMaxDepth) +
                             " levels at instantiation of '" + m.name + "'");
            return false;
        }
        active.push_back(m.name);

        // Only the top module's ports become graph Port nodes; child ports
        // are pure binding targets resolved through Ctx::conns.
        if (ctx.prefix.empty())
            out.ports = m.ports;

        // Internal nets. A child net bound upward through a port IS the
        // parent's net — do not redeclare it under a prefix.
        for (const auto& nd : m.nets) {
            if (!ctx.prefix.empty() && ctx.conns.count(nd.name))
                continue;
            NetDecl d = nd;
            d.name = ctx.prefix + nd.name;
            out.nets.push_back(d);
        }

        // Continuous assigns, both sides resolved.
        for (const auto& a : m.assigns) {
            AssignStmt s = a;
            s.lhs = resolve_ref(a.lhs, ctx);
            s.rhs = resolve_ref(a.rhs, ctx);
            out.assigns.push_back(s);
        }

        for (const auto& inst : m.instances) {
            auto it = modules.find(inst.cell_type);
            if (it == modules.end()) {
                // Library/opaque cell: keep it as a leaf, renamed + rebound.
                Instance flat = inst;
                flat.inst_name = ctx.prefix + inst.inst_name;
                for (auto& c : flat.conns)
                    c.net = resolve_ref(c.net, ctx);
                out.instances.push_back(flat);
                continue;
            }

            const NetlistModule& child = *it->second;
            for (const std::string& name : active) {
                if (name == child.name) {
                    errors.push_back("recursive module instantiation: module '" + child.name +
                                     "' instantiated as '" + inst.inst_name + "'");
                    return false;
                }
            }

            Ctx child_ctx;
            child_ctx.prefix = ctx.prefix + inst.inst_name + ".";
            if (inst.positional()) {
                for (size_t i = 0; i < inst.conns.size(); ++i) {
                    if (i >= child.ports.size())
                        break;
                    NetRef bound = resolve_ref(inst.conns[i].net, ctx);
                    if (is_unbound(bound))
                        continue;
                    child_ctx.conns[child.ports[i].name] = bound;
                }
                if (inst.conns.size() != child.ports.size())
                    warnings.push_back("module '" + inst.cell_type + "' instantiated as '" +
                                       inst.inst_name + "' with " +
                                       std::to_string(inst.conns.size()) +
                                       " positional connections but " +
                                       std::to_string(child.ports.size()) + " ports");
            } else {
                for (const auto& c : inst.conns) {
                    NetRef bound = resolve_ref(c.net, ctx);
                    if (is_unbound(bound))
                        continue;  // `.q()` — leave the port isolated
                    child_ctx.conns[c.pin] = bound;
                }
            }
            if (!emit(child, child_ctx))
                return false;
        }

        active.pop_back();
        return true;
    }
};

}  // namespace

bool references_submodules(const NetlistModule& top,
                           const std::vector<NetlistModule>& submodules) {
    if (submodules.empty())
        return false;
    // The top module joins the lookup map so an instance that refers back to
    // the top (direct self-instantiation or an indirect cycle through a
    // child) is recognized as a submodule reference and rejected during
    // flattening instead of degrading to an opaque library cell.
    ModuleMap map = make_module_map(submodules);
    map.emplace(top.name, &top);
    for (const auto& inst : top.instances) {
        if (map.count(inst.cell_type))
            return true;
    }
    return false;
}

bool flatten_hierarchy(const NetlistModule& top,
                       const std::vector<NetlistModule>& submodules,
                       NetlistModule& out,
                       std::vector<std::string>& errors,
                       std::vector<std::string>& warnings) {
    out = NetlistModule{};
    out.name = top.name;
    ModuleMap map = make_module_map(submodules);
    map.emplace(top.name, &top);  // cycle detection sees the top too
    Elaborator elab{map, out, errors, warnings};
    return elab.emit(top, Ctx{});
}

}  // namespace opencdc::frontend::netlist
