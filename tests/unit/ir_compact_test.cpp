#include <gtest/gtest.h>

#include <algorithm>

#include "ir/compact.h"
#include "ir/graph.h"

namespace opencdc::ir {
namespace {

TEST(CompactTest, CollapsesDataBufferChainPreservingConnectivity) {
    // src -> n1 -> Buf -> n2 -> dst. All three intermediate data nodes are
    // removable; the register-to-register reachability must be identical after
    // compaction (a direct src -> dst edge replaces the chain).
    Graph g;
    SourceLoc loc{"t.v", 1, 0};
    uint64_t clk = g.add_port("clk", 1, loc);
    uint64_t src = g.add_register("src", "clk", 1, loc);
    uint64_t dst = g.add_register("dst", "clk", 1, loc);
    uint64_t n1 = g.add_net("n1", 1, loc);
    uint64_t n2 = g.add_net("n2", 1, loc);
    // add_combinational wires n1 -> buf via `inputs`.
    uint64_t buf = g.add_combinational("buf1", LogicType::Buf, {n1}, 1, loc);
    g.add_edge(src, n1);
    g.add_edge(buf, n2);
    g.add_edge(n2, dst);
    ASSERT_TRUE(g.validate().ok());

    auto before = g.register_predecessors(dst);
    ASSERT_EQ(before.size(), 1u);
    EXPECT_EQ(before[0], src);
    size_t nodes_before = g.nodes().size();

    size_t removed = g.nodes().empty() ? 0 : compact_graph(g);
    EXPECT_EQ(removed, 3u) << "expected n1, buf1, n2 to be removed";

    // Connectivity preserved: dst still has exactly src as a register source.
    auto after = g.register_predecessors(dst);
    ASSERT_EQ(after.size(), 1u) << "compaction must not change reachability";
    EXPECT_EQ(after[0], src);
    EXPECT_EQ(g.nodes().size(), nodes_before - removed);
    EXPECT_TRUE(g.validate().ok());
    // The clock port must survive.
    EXPECT_NE(g.find_node_by_name("clk"), nullptr);
}

TEST(CompactTest, PreservesClockBufferAndResetStructure) {
    // The clock path clk -> Buf -> clkn (a register clock domain) must be left
    // intact even though the buffer is an identity cell and `clkn` is a
    // single-driver net: both sit in the backward cone of a clock domain. The
    // unrelated data net between the two registers is still collapsed.
    Graph g;
    SourceLoc loc{"t.v", 1, 0};
    uint64_t clk = g.add_port("clk", 1, loc);
    uint64_t clkbuf = g.add_combinational("clkbuf", LogicType::Buf, {clk}, 1, loc);
    uint64_t clkn = g.add_net("clkn", 1, loc);
    g.add_edge(clkbuf, clkn);
    uint64_t src = g.add_register("src", "clkn", 1, loc);
    uint64_t dst = g.add_register("dst", "clkn", 1, loc);
    uint64_t dn = g.add_net("dn", 1, loc);
    g.add_edge(src, dn);
    g.add_edge(dn, dst);
    ASSERT_TRUE(g.validate().ok());

    size_t removed = compact_graph(g);

    // Clock structure untouched.
    EXPECT_NE(g.find_node_by_name("clk"), nullptr);
    EXPECT_NE(g.find_node_by_name("clkbuf"), nullptr)
        << "clock-path buffer must not be compacted away";
    EXPECT_NE(g.find_node_by_name("clkn"), nullptr) << "clock-domain anchor net must be preserved";
    // Only the data net was removed.
    EXPECT_EQ(removed, 1u);
    EXPECT_EQ(g.find_node_by_name("dn"), nullptr);
    // Data reachability preserved across the collapsed net.
    auto preds = g.register_predecessors(dst);
    EXPECT_TRUE(std::find(preds.begin(), preds.end(), src) != preds.end());
    EXPECT_TRUE(g.validate().ok());
}

TEST(CompactTest, NoOpOnEmptyAndFullyProtectedGraph) {
    Graph empty;
    EXPECT_EQ(compact_graph(empty), 0u);

    // Only ports + registers + a clock net: nothing safe to remove.
    Graph g;
    SourceLoc loc{"t.v", 1, 0};
    g.add_port("clk", 1, loc);
    uint64_t clkn = g.add_net("clkn", 1, loc);
    g.add_register("r", "clkn", 1, loc);
    size_t before = g.nodes().size();
    EXPECT_EQ(compact_graph(g), 0u);
    EXPECT_EQ(g.nodes().size(), before);
    EXPECT_NE(g.find_node_by_name("clkn"), nullptr);
}

}  // namespace
}  // namespace opencdc::ir
