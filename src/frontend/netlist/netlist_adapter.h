#ifndef OPENCDC_FRONTEND_NETLIST_NETLIST_ADAPTER_H
#define OPENCDC_FRONTEND_NETLIST_NETLIST_ADAPTER_H

#include "cell/cell_library.h"
#include "frontend/frontend.h"

namespace opencdc::frontend::netlist {

// Gate-level (post-synthesis) netlist frontend. Parses flat Verilog netlists
// and maps cell instances onto the shared ir::Graph using the cell library, so
// the entire downstream analysis pipeline runs unchanged.
class NetlistAdapter : public IFrontend {
   public:
    FrontendResult elaborate(const std::vector<std::string>& files, const std::string& top_module,
                             const FrontendOptions& options = {}) override;

   private:
    cell::CellLibrary library_;
};

}  // namespace opencdc::frontend::netlist

#endif  // OPENCDC_FRONTEND_NETLIST_NETLIST_ADAPTER_H
