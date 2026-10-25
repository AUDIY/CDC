#ifndef OPENCDC_ANALYSIS_ANALYZER_H
#define OPENCDC_ANALYSIS_ANALYZER_H

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "analysis/coverage.h"
#include "analysis/signoff.h"
#include "cdc/crossing.h"
#include "clock/domain.h"
#include "config/config.h"
#include "frontend/frontend.h"
#include "ir/graph.h"

namespace opencdc::analysis {

struct AnalysisRequest {
    std::vector<std::string> input_files;
    std::string top_module;
    std::string config_path;
    std::string waiver_path;
    std::string constraints_path;
    std::vector<std::pair<std::string, std::string>> false_paths;
    std::vector<std::string> disable_rules;
    std::vector<std::string> severity_overrides;
    std::optional<config::Config> config;
    // Methodology profile name (optional). When set, overrides config settings.
    std::string profile;
    std::vector<std::string> include_dirs;
    std::vector<std::string> defines;
    size_t num_threads = 0;
    // Language frontend selection (Auto = per-file detection). CLI overrides
    // config; config overrides this default.
    frontend::InputType input_type = frontend::InputType::Auto;
    // YAML cell map for the netlist frontend (DFF/Latch/ICG pin mapping).
    std::string cell_map_path;
    // Liberty (.lib) file for the netlist frontend (cell definitions).
    std::string liberty_path;
    // RTL->netlist register name map for the netlist frontend.
    std::string name_map_path;
    // Graph safety limit overrides (0 = frontend defaults).
    size_t max_nodes = 0;
    size_t max_edges = 0;
};

struct AnalysisResult {
    bool ok = false;
    std::string analysis_status = "complete";
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    // Frontend actually used: "rtl" or "netlist" (empty before frontend runs).
    std::string input_type;
    ir::Graph graph;
    clock::DomainResult domains;
    std::vector<cdc::Finding> findings;
    CoverageResult coverage;
    SignoffResult signoff;
};

// Runs the full CDC analysis pipeline. Single entry point shared by the
// CLI, LSP server, and Python bindings so behavior cannot drift.
class Analyzer {
   public:
    AnalysisResult run(const AnalysisRequest& request);

    // Incremental re-analysis: returns previous findings if the graph is clean;
    // otherwise re-runs the analysis stages on a copy of the mutated graph
    // (no re-elaboration from files, so caller-side graph edits take effect).
    AnalysisResult run_incremental(AnalysisResult& previous, const AnalysisRequest& request);

   private:
    // Steps 2-14 of the pipeline on an already-elaborated graph.
    AnalysisResult run_on_graph(ir::Graph graph, const AnalysisRequest& request,
                                config::Config cfg);
};

}  // namespace opencdc::analysis

#endif  // OPENCDC_ANALYSIS_ANALYZER_H
