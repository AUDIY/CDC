#include "frontend/netlist/name_map.h"

#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>

#include "ir/graph.h"

namespace opencdc::frontend::netlist {

using ir::NodeKind;

namespace {

// Split "name[7]" into ("name", 7). Returns false when the name has no
// "[<digits>]" suffix (bitless register names are matched directly).
bool split_bit(const std::string& name, std::string* base, long* idx) {
    size_t open = name.rfind('[');
    if (open == std::string::npos || name.back() != ']')
        return false;
    size_t first_digit = open + 1;
    size_t end = name.size() - 1;  // ']' sits here
    if (first_digit >= end)        // "name[]" — no digits, treat as plain
        return false;
    for (size_t i = first_digit; i < end; ++i) {
        if (!isdigit(static_cast<unsigned char>(name[i])))
            return false;
    }
    *base = name.substr(0, open);
    *idx = std::stol(name.substr(first_digit, end - first_digit));
    return true;
}

}  // namespace

bool load_name_map(const std::string& path,
                   std::vector<std::pair<std::string, std::string>>& entries, std::string* error) {
    if (error)
        error->clear();
    std::ifstream in(path);
    if (!in) {
        if (error)
            *error = "cannot open name map: " + path;
        return false;
    }
    std::string line;
    size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        size_t comment = line.find('#');
        if (comment != std::string::npos)
            line = line.substr(0, comment);
        std::istringstream ss(line);
        std::string rtl, netlist, extra;
        if (!(ss >> rtl))
            continue;  // blank line
        if (!(ss >> netlist)) {
            if (error)
                *error =
                    "name map:" + std::to_string(lineno) + ": expected '<rtl_name> <netlist_name>'";
            return false;
        }
        if (ss >> extra) {
            if (error)
                *error = "name map:" + std::to_string(lineno) + ": unexpected extra token '" +
                         extra + "'";
            return false;
        }
        entries.emplace_back(rtl, netlist);
    }
    return true;
}

void apply_name_map(ir::Graph& graph,
                    const std::vector<std::pair<std::string, std::string>>& entries,
                    std::vector<std::string>* warnings) {
    if (entries.empty())
        return;

    // netlist base name -> RTL name (first entry wins on duplicates).
    std::unordered_map<std::string, std::string> netlist_to_rtl;
    for (const auto& e : entries)
        netlist_to_rtl.emplace(e.second, e.first);

    // Names currently present in the graph (collision detection).
    std::set<std::string> taken;
    for (const auto& node : graph.nodes())
        taken.insert(node.hier_name);

    std::set<std::string> matched;  // netlist names seen at least once
    for (auto& node : graph.nodes_mutable()) {
        if (node.kind != NodeKind::Register)
            continue;
        std::string base;
        long idx = 0;
        bool has_bit = split_bit(node.hier_name, &base, &idx);
        const std::string& lookup = has_bit ? base : node.hier_name;
        auto it = netlist_to_rtl.find(lookup);
        if (it == netlist_to_rtl.end())
            continue;
        matched.insert(lookup);

        std::string target = it->second + (has_bit ? "[" + std::to_string(idx) + "]" : "");
        if (target == node.hier_name)
            continue;
        if (taken.count(target)) {
            if (warnings)
                warnings->push_back("name map: register '" + node.hier_name + "' not renamed to '" +
                                    target + "': target name already exists");
            continue;
        }
        taken.erase(node.hier_name);
        taken.insert(target);
        node.hier_name = target;
    }

    if (warnings) {
        for (const auto& e : entries) {
            if (!matched.count(e.second))
                warnings->push_back("name map: entry '" + e.first + "' -> '" + e.second +
                                    "' matched no registers (stale map?)");
        }
    }
}

}  // namespace opencdc::frontend::netlist
