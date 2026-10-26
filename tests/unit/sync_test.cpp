#include <gtest/gtest.h>

#include "cdc/crossing.h"
#include "cdc/synchronizer.h"
#include "clock/domain.h"
#include "ir/graph.h"

using namespace opencdc::ir;
using namespace opencdc::clock;
using namespace opencdc::cdc;

class SyncTest : public ::testing::Test {
   protected:
    Graph graph;
    SynchronizerMatcher matcher;
};

TEST_F(SyncTest, Detect2FFChain) {
    uint64_t src = graph.add_register("mod.src", "clk_a", 1, {"mod.sv", 5, 5});
    uint64_t meta = graph.add_register("mod.meta", "clk_b", 1, {"mod.sv", 8, 5});
    uint64_t sync = graph.add_register("mod.sync", "clk_b", 1, {"mod.sv", 9, 5});

    graph.add_edge(src, meta);
    graph.add_edge(meta, sync);

    auto pat = matcher.find_pattern_for_dest(meta, graph);
    EXPECT_EQ(pat, SyncPattern::TwoFF);
}

TEST_F(SyncTest, Detect3FFChain) {
    uint64_t src = graph.add_register("mod.src", "clk_a", 1, {"mod.sv", 5, 5});
    uint64_t m1 = graph.add_register("mod.m1", "clk_b", 1, {"mod.sv", 8, 5});
    uint64_t m2 = graph.add_register("mod.m2", "clk_b", 1, {"mod.sv", 9, 5});
    uint64_t sync = graph.add_register("mod.sync", "clk_b", 1, {"mod.sv", 10, 5});

    graph.add_edge(src, m1);
    graph.add_edge(m1, m2);
    graph.add_edge(m2, sync);

    auto pat = matcher.find_pattern_for_dest(m1, graph);
    EXPECT_EQ(pat, SyncPattern::ThreeFF);
}

TEST_F(SyncTest, RejectChainWithDifferentClocks) {
    uint64_t src = graph.add_register("mod.src", "clk_a", 1, {"mod.sv", 5, 5});
    uint64_t meta = graph.add_register("mod.meta", "clk_b", 1, {"mod.sv", 8, 5});
    uint64_t sync = graph.add_register("mod.sync", "clk_c", 1, {"mod.sv", 9, 5});

    graph.add_edge(src, meta);
    graph.add_edge(meta, sync);

    auto pat = matcher.find_pattern_for_dest(meta, graph);
    EXPECT_EQ(pat, SyncPattern::None);
}

TEST_F(SyncTest, NoChainReturnsNone) {
    uint64_t src = graph.add_register("mod.src", "clk_a", 1, {"mod.sv", 5, 5});
    uint64_t dst = graph.add_register("mod.dst", "clk_b", 1, {"mod.sv", 6, 5});

    graph.add_edge(src, dst);

    auto pat = matcher.find_pattern_for_dest(dst, graph);
    EXPECT_EQ(pat, SyncPattern::None);
}

TEST_F(SyncTest, MatchFindsChains) {
    uint64_t src = graph.add_register("mod.src", "clk_a", 1, {"mod.sv", 5, 5});
    uint64_t meta = graph.add_register("mod.meta", "clk_b", 1, {"mod.sv", 8, 5});
    uint64_t sync = graph.add_register("mod.sync", "clk_b", 1, {"mod.sv", 9, 5});

    graph.add_edge(src, meta);
    graph.add_edge(meta, sync);

    auto chains = matcher.match(graph);
    ASSERT_EQ(chains.size(), 1u);
    EXPECT_EQ(chains[0].pattern, SyncPattern::TwoFF);
    EXPECT_EQ(chains[0].source_reg_id, src);
    EXPECT_EQ(chains[0].stage_ids.size(), 2u);
}

TEST_F(SyncTest, TwoFFNotMisclassifiedAsThreeFFByConsumer) {
    // Two independent 2FF chains reconverge at a common consumer.
    // Each chain is meta→sync. The consumer has two same-domain predecessors
    // and must NOT be counted as a 3rd FF stage.
    uint64_t src = graph.add_register("mod.src", "clk_a", 1, {"mod.sv", 5, 5});
    uint64_t meta1 = graph.add_register("mod.meta1", "clk_b", 1, {"mod.sv", 8, 5});
    uint64_t sync1 = graph.add_register("mod.sync1", "clk_b", 1, {"mod.sv", 9, 5});
    uint64_t meta2 = graph.add_register("mod.meta2", "clk_b", 1, {"mod.sv", 10, 5});
    uint64_t sync2 = graph.add_register("mod.sync2", "clk_b", 1, {"mod.sv", 11, 5});
    uint64_t consumer = graph.add_register("mod.consumer", "clk_b", 1, {"mod.sv", 12, 5});

    graph.add_edge(src, meta1);
    graph.add_edge(meta1, sync1);
    graph.add_edge(sync1, consumer);
    graph.add_edge(src, meta2);
    graph.add_edge(meta2, sync2);
    graph.add_edge(sync2, consumer);

    auto pat = matcher.find_pattern_for_dest(meta1, graph);
    EXPECT_EQ(pat, SyncPattern::TwoFF)
        << "Reconvergent consumer with two preds must not inflate to ThreeFF";
}

TEST_F(SyncTest, PackedVectorTwoFFRecognized) {
    // `sync <= {sync[0], src}`: one 2-bit node holds both stages. The
    // frontend tag carries the verified stage count; the graph alone (a
    // cross-domain edge plus a self-loop) would be ambiguous.
    uint64_t src = graph.add_register("mod.src", "clk_a", 1, {"mod.sv", 5, 5});
    uint64_t packed = graph.add_register("mod.sync", "clk_b", 2, {"mod.sv", 8, 5});
    graph.find_node_mutable(packed)->packed_shift_depth = 2;
    graph.add_edge(src, packed);
    graph.add_edge(packed, packed);

    EXPECT_EQ(matcher.find_pattern_for_dest(packed, graph), SyncPattern::TwoFF);
    EXPECT_EQ(matcher.find_pattern_for_dest(packed, graph, true), SyncPattern::TwoFF);
    EXPECT_EQ(matcher.chain_depth(packed, graph), 2u);
    EXPECT_FALSE(matcher.below_min_stages(packed, graph, 2));
}

TEST_F(SyncTest, PackedVectorThreeFFRecognized) {
    uint64_t src = graph.add_register("mod.src", "clk_a", 1, {"mod.sv", 5, 5});
    uint64_t packed = graph.add_register("mod.sync", "clk_b", 3, {"mod.sv", 8, 5});
    graph.find_node_mutable(packed)->packed_shift_depth = 3;
    graph.add_edge(src, packed);

    EXPECT_EQ(matcher.find_pattern_for_dest(packed, graph, true), SyncPattern::ThreeFF);
    EXPECT_EQ(matcher.chain_depth(packed, graph), 3u);
}

TEST_F(SyncTest, WideNodeWithoutTagNotRecognized) {
    // `sync <= sync | src` produces the same edges as a packed chain
    // (cross-domain edge + self-loop) but is no shift chain: without the
    // frontend tag the crossing must stay a violation.
    uint64_t src = graph.add_register("mod.src", "clk_a", 1, {"mod.sv", 5, 5});
    uint64_t wide = graph.add_register("mod.bus", "clk_b", 2, {"mod.sv", 8, 5});
    graph.add_edge(src, wide);
    graph.add_edge(wide, wide);

    EXPECT_EQ(matcher.find_pattern_for_dest(wide, graph), SyncPattern::None);
    EXPECT_EQ(matcher.find_pattern_for_dest(wide, graph, true), SyncPattern::None);
    EXPECT_EQ(matcher.chain_depth(wide, graph), 0u);
}

TEST_F(SyncTest, PackedVectorRequiresCrossDomainPred) {
    // The tag alone does not make a node a synchronizer: it also needs a
    // register from another clock domain feeding it.
    uint64_t feed = graph.add_register("mod.feed", "clk_b", 1, {"mod.sv", 5, 5});
    uint64_t packed = graph.add_register("mod.sync", "clk_b", 2, {"mod.sv", 8, 5});
    graph.find_node_mutable(packed)->packed_shift_depth = 2;
    graph.add_edge(feed, packed);

    EXPECT_EQ(matcher.find_pattern_for_dest(packed, graph, true), SyncPattern::None);
    EXPECT_EQ(matcher.chain_depth(packed, graph), 0u);
}

TEST_F(SyncTest, PackedVectorStrictRejectsSameDomainPred) {
    // Same strict rule as scalar chains: an extra same-domain register
    // feeding the packed destination disqualifies it.
    uint64_t src = graph.add_register("mod.src", "clk_a", 1, {"mod.sv", 5, 5});
    uint64_t extra = graph.add_register("mod.extra", "clk_b", 1, {"mod.sv", 7, 5});
    uint64_t packed = graph.add_register("mod.sync", "clk_b", 2, {"mod.sv", 8, 5});
    graph.find_node_mutable(packed)->packed_shift_depth = 2;
    graph.add_edge(src, packed);
    graph.add_edge(extra, packed);

    EXPECT_EQ(matcher.find_pattern_for_dest(packed, graph, true), SyncPattern::None);
    // Non-strict still sees the verified chain.
    EXPECT_EQ(matcher.find_pattern_for_dest(packed, graph, false), SyncPattern::TwoFF);
}

TEST_F(SyncTest, PackedChainMatchReportsVectorDepth) {
    // match() must report the vector-internal stage count, not the number
    // of physical nodes (which is one).
    uint64_t src = graph.add_register("mod.src", "clk_a", 1, {"mod.sv", 5, 5});
    uint64_t packed = graph.add_register("mod.sync", "clk_b", 2, {"mod.sv", 8, 5});
    graph.find_node_mutable(packed)->packed_shift_depth = 2;
    graph.add_edge(src, packed);
    graph.add_edge(packed, packed);

    auto chains = matcher.match(graph);
    ASSERT_EQ(chains.size(), 1u);
    EXPECT_EQ(chains[0].pattern, SyncPattern::TwoFF);
    EXPECT_EQ(chains[0].stage_ids.size(), 1u);
    EXPECT_EQ(chains[0].depth, 2u);
}
