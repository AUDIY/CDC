#include "analysis/analyzer.h"
#include "frontend/frontend.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace opencdc::analysis {
namespace {

using frontend::InputType;

std::string fx(const char* rel) {
    return std::string(FIXTURES_DIR) + "/netlist/" + rel;
}

const cdc::Finding* find_rule(const std::vector<cdc::Finding>& findings, const std::string& id) {
    for (const auto& f : findings) {
        if (f.rule_id == id)
            return &f;
    }
    return nullptr;
}

TEST(NetlistFrontendTest, DetectsNetlistInput) {
    std::vector<std::string> files = {fx("cdc_crossing.v")};
    EXPECT_EQ(frontend::detect_input_type(files), InputType::Netlist);
    files = {fx("sync_2ff.v")};
    EXPECT_EQ(frontend::detect_input_type(files), InputType::Netlist);
    files = {fx("gated_clock.v")};
    EXPECT_EQ(frontend::detect_input_type(files), InputType::Netlist);
}

TEST(NetlistFrontendTest, DetectsRtlInput) {
    // A behavioral RTL fixture must be detected as rtl (preserve default).
    std::vector<std::string> files = {std::string(FIXTURES_DIR) + "/sv/gated_clock.sv"};
    EXPECT_EQ(frontend::detect_input_type(files), InputType::Rtl);
}

TEST(NetlistFrontendTest, UnsyncCrossingReportsCdc001Error) {
    AnalysisRequest req;
    req.input_files = {fx("cdc_crossing.v")};
    req.top_module = "cdc_crossing";
    req.input_type = InputType::Netlist;
    Analyzer analyzer;
    auto result = analyzer.run(req);
    ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_EQ(result.input_type, "netlist");

    const cdc::Finding* f = find_rule(result.findings, "CDC001");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->severity, "error");
    EXPECT_EQ(f->source_domain, "clk_a");
    EXPECT_EQ(f->dest_domain, "clk_b");
}

TEST(NetlistFrontendTest, TwoFfsyncIsStructurallySafe) {
    AnalysisRequest req;
    req.input_files = {fx("sync_2ff.v")};
    req.top_module = "sync_2ff";
    req.input_type = InputType::Netlist;
    Analyzer analyzer;
    auto result = analyzer.run(req);
    ASSERT_TRUE(result.ok);

    const cdc::Finding* f = find_rule(result.findings, "CDC001");
    ASSERT_NE(f, nullptr);
    // Structural 2FF chain detected at the destination -> safe/info.
    EXPECT_EQ(f->severity, "info");
    EXPECT_EQ(f->safety_status, cdc::SafetyStatus::VerifiedSafe);
}

TEST(NetlistFrontendTest, GatedClockCrossingReportsCdc004) {
    AnalysisRequest req;
    req.input_files = {fx("gated_crossing.v")};
    req.top_module = "gated_crossing";
    req.input_type = InputType::Netlist;
    Analyzer analyzer;
    auto result = analyzer.run(req);
    ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors.front());

    const cdc::Finding* f = find_rule(result.findings, "CDC004");
    ASSERT_NE(f, nullptr) << "expected gated-clock crossing finding";
    EXPECT_EQ(f->severity, "warning");
}

TEST(NetlistFrontendTest, BitBlastedBusHasNoCrossBitCrossings) {
    // A 4-bit bus crossing bit-blasted into per-bit flops must yield exactly
    // 4 crossings (u[i] -> v[i]), NOT 16. Guards against collapsing `bus[i]`
    // into one shared net node, which fabricates cross-bit paths.
    AnalysisRequest req;
    req.input_files = {fx("bus_crossing.v")};
    req.top_module = "bus_crossing";
    req.input_type = InputType::Netlist;
    Analyzer analyzer;
    auto result = analyzer.run(req);
    ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors.front());

    int crossings = 0;
    for (const auto& f : result.findings) {
        if (f.rule_id != "CDC001")
            continue;
        ++crossings;
        // u<i> -> v<i>: the trailing bit index must match (no cross-bit paths).
        ASSERT_FALSE(f.source_reg_name.empty());
        ASSERT_FALSE(f.dest_reg_name.empty());
        EXPECT_EQ(f.source_reg_name.back(), f.dest_reg_name.back())
            << "cross-bit crossing: " << f.source_reg_name << " -> "
            << f.dest_reg_name;
        EXPECT_EQ(f.source_reg_name.front(), 'u');
        EXPECT_EQ(f.dest_reg_name.front(), 'v');
    }
    EXPECT_EQ(crossings, 4);
}

TEST(NetlistFrontendTest, AutoDetectionEndToEnd) {
    // InputType::Auto should classify the structural netlist and analyze it.
    AnalysisRequest req;
    req.input_files = {fx("cdc_crossing.v")};
    req.top_module = "cdc_crossing";
    req.input_type = InputType::Auto;
    Analyzer analyzer;
    auto result = analyzer.run(req);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.input_type, "netlist");
    EXPECT_NE(find_rule(result.findings, "CDC001"), nullptr);
}

TEST(NetlistFrontendTest, CellMapClassifiesResetPolarity) {
    // With a cell map classifying DFFCNQ_* as dff_reset (active-low async),
    // a reset net should be recorded on the register (drives CDC009/etc).
    AnalysisRequest req;
    req.input_files = {fx("reset_flop.v")};
    req.top_module = "reset_flop";
    req.input_type = InputType::Netlist;
    req.cell_map_path = fx("cells.yaml");
    Analyzer analyzer;
    auto result = analyzer.run(req);
    ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors.front());
    // Find a register node and check it recorded the reset net.
    bool found_reset = false;
    for (const auto& n : result.graph.nodes()) {
        if (n.kind == ir::NodeKind::Register && !n.reset_signal.empty()) {
            found_reset = true;
            EXPECT_EQ(n.reset_signal, "rst_n");
            EXPECT_EQ(n.reset_pol, ir::ResetPolarity::ActiveLow);
            EXPECT_TRUE(n.is_async_reset);
        }
    }
    EXPECT_TRUE(found_reset);
}

TEST(NetlistFrontendTest, UnknownCellTreatedAsOpaqueNotSafe) {
    // A cell type absent from the library must be classified Unknown (opaque
    // combinational) and produce a warning — never silently assumed safe.
    AnalysisRequest req;
    req.input_files = {fx("unknown_cell.v")};
    req.top_module = "unknown_cell";
    req.input_type = InputType::Netlist;
    Analyzer analyzer;
    auto result = analyzer.run(req);
    ASSERT_TRUE(result.ok);
    bool warned = false;
    for (const auto& w : result.warnings) {
        if (w.find("unmapped cell") != std::string::npos)
            warned = true;
    }
    EXPECT_TRUE(warned);
}

}  // namespace
}  // namespace opencdc::analysis
