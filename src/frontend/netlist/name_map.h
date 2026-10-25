#ifndef OPENCDC_FRONTEND_NETLIST_NAME_MAP_H
#define OPENCDC_FRONTEND_NETLIST_NAME_MAP_H

#include <string>
#include <utility>
#include <vector>

namespace opencdc::ir {
class Graph;
}

namespace opencdc::frontend::netlist {

// RTL-to-netlist register name mapping (netlist frontend only).
//
// Synthesis renames registers (`src_reg` -> `u_0/ff`), so RTL-authored
// waivers and reports no longer match netlist findings. A name map restores
// the RTL names so signoff artifacts authored on RTL keep applying:
//
//     # rtl_name netlist_name      (comments start with '#'; blank lines ok)
//     data_reg   u_core/ff
//     dst_reg    u_top/ff
//
// apply_name_map() renames every register whose `<base>[<bit>]` name matches
// an entry's netlist side to the entry's RTL side (the `[<bit>]` suffix is
// preserved, and renaming happens BEFORE bus regrouping so each group merges
// under its RTL name). A rename is skipped with a warning when the target
// name is already taken, and entries that match no register are reported so
// stale maps surface instead of silently passing.
bool load_name_map(const std::string& path,
                   std::vector<std::pair<std::string, std::string>>& entries,
                   std::string* error = nullptr);

void apply_name_map(ir::Graph& graph,
                    const std::vector<std::pair<std::string, std::string>>& entries,
                    std::vector<std::string>* warnings = nullptr);

}  // namespace opencdc::frontend::netlist

#endif  // OPENCDC_FRONTEND_NETLIST_NAME_MAP_H
