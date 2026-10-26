#include "cell/liberty.h"

#include <gtest/gtest.h>

namespace opencdc::cell {
namespace {

#define LIB_FIXTURE (std::string(FIXTURES_DIR) + "/netlist/nangate_mini.lib")

TEST(LibertyTest, LoadsFixtureLibrary) {
    CellLibrary lib;
    std::string err;
    ASSERT_TRUE(load_liberty_file(LIB_FIXTURE, lib, &err)) << err;
    EXPECT_EQ(lib.size(), 9u);
}

TEST(LibertyTest, AsyncClearFlopMapsToFfReset) {
    CellLibrary lib;
    std::string err;
    ASSERT_TRUE(load_liberty_file(LIB_FIXTURE, lib, &err)) << err;
    const CellDef* d = lib.lookup("DFF_X1");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->kind, CellKind::DffReset);
    EXPECT_EQ(d->clock_pin, "CK");
    EXPECT_EQ(d->data_pin, "D");
    EXPECT_EQ(d->output_pin, "Q");
    EXPECT_EQ(d->reset_pin, "CDN");
    EXPECT_TRUE(d->reset_active_low);  // clear : "!CDN"
    EXPECT_TRUE(d->reset_async);
}

TEST(LibertyTest, ActiveHighClearIsNotNegated) {
    CellLibrary lib;
    std::string err;
    ASSERT_TRUE(load_liberty_file(LIB_FIXTURE, lib, &err)) << err;
    const CellDef* d = lib.lookup("DFFR_X2");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->kind, CellKind::DffReset);
    EXPECT_EQ(d->clock_pin, "CLK");
    EXPECT_EQ(d->data_pin, "DI");
    EXPECT_EQ(d->reset_pin, "R");
    EXPECT_FALSE(d->reset_active_low);  // clear : "R"
}

TEST(LibertyTest, LatchGroupMapsToLatch) {
    CellLibrary lib;
    std::string err;
    ASSERT_TRUE(load_liberty_file(LIB_FIXTURE, lib, &err)) << err;
    const CellDef* d = lib.lookup("LATCH_X1");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->kind, CellKind::Latch);
    EXPECT_EQ(d->clock_pin, "G");
    EXPECT_EQ(d->data_pin, "D");
    EXPECT_EQ(d->output_pin, "Q");
}

TEST(LibertyTest, CombFunctionsGiveLogicTypeHints) {
    CellLibrary lib;
    std::string err;
    ASSERT_TRUE(load_liberty_file(LIB_FIXTURE, lib, &err)) << err;
    // ir::LogicType integer values: And=1, Or=2, Xor=3, Not=4, Buf=5, Mux=6.
    const CellDef* buf = lib.lookup("BUF_X1");
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(buf->kind, CellKind::Buffer);
    const CellDef* inv = lib.lookup("INV_X1");
    ASSERT_NE(inv, nullptr);
    EXPECT_EQ(inv->kind, CellKind::Inverter);
    const CellDef* and2 = lib.lookup("AND2_X1");
    ASSERT_NE(and2, nullptr);
    EXPECT_EQ(and2->kind, CellKind::Comb);
    EXPECT_EQ(and2->logic_type_hint, 1);  // And
    EXPECT_EQ(and2->output_pin, "Y");
    const CellDef* xor2 = lib.lookup("XOR2_X1");
    ASSERT_NE(xor2, nullptr);
    EXPECT_EQ(xor2->logic_type_hint, 3);  // Xor
    const CellDef* mux2 = lib.lookup("MUX2_X1");
    ASSERT_NE(mux2, nullptr);
    EXPECT_EQ(mux2->logic_type_hint, 6);  // Mux
    // Complex mixed expression: no hint — the adapter falls back to the
    // pattern-name heuristic (never a wrong structural assumption).
    const CellDef* aoi = lib.lookup("AOI21_X1");
    ASSERT_NE(aoi, nullptr);
    EXPECT_EQ(aoi->logic_type_hint, -1);
}

TEST(LibertyTest, LibertyDefsShadowHeuristicDefaults) {
    CellLibrary lib = CellLibrary::with_defaults();
    std::string err;
    ASSERT_TRUE(load_liberty_file(LIB_FIXTURE, lib, &err)) << err;
    // The exact-name liberty def wins over the "DFF*" default glob...
    const CellDef* d = lib.lookup("DFF_X1");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->clock_pin, "CK");
    // ...while cell types absent from the liberty file keep the defaults.
    const CellDef* fallback = lib.lookup("DFF_Z9");
    ASSERT_NE(fallback, nullptr);
    EXPECT_EQ(fallback->kind, CellKind::Dff);
    EXPECT_TRUE(fallback->clock_pin.empty());
}

TEST(LibertyTest, ExplicitCellMapCanStillOverrideLiberty) {
    // Precedence in the adapter: defaults < liberty < YAML cell map. The
    // longest-matching glob wins, so a longer map pattern shadows the exact
    // liberty name (equal lengths keep the first inserted — the liberty def).
    CellLibrary lib = CellLibrary::with_defaults();
    std::string err;
    ASSERT_TRUE(load_liberty_file(LIB_FIXTURE, lib, &err)) << err;
    CellDef override_def;
    override_def.pattern = "DFF_X1*";  // 7 chars > exact "DFF_X1"
    override_def.kind = CellKind::DffEnable;
    lib.add(override_def);
    const CellDef* d = lib.lookup("DFF_X1");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->kind, CellKind::DffEnable);
}

TEST(LibertyTest, MalformedInputFails) {
    CellLibrary lib;
    std::string err;
    EXPECT_FALSE(load_liberty_string("library (x {\n  cell (A) {", lib, &err));
    EXPECT_FALSE(err.empty());
}

TEST(LibertyTest, UnterminatedStringFails) {
    CellLibrary lib;
    std::string err;
    EXPECT_FALSE(load_liberty_string("cell (A) { pin (Y) { function : \"A }", lib, &err));
    EXPECT_FALSE(err.empty());
}

TEST(LibertyTest, TopLevelCellWithoutLibraryWrapper) {
    // Some flows emit bare cell groups; they must import the same way.
    CellLibrary lib;
    std::string err;
    const char* src =
        "cell (AND2_X1) {\n"
        "  pin (A) { direction : input; }\n"
        "  pin (B) { direction : input; }\n"
        "  pin (Y) { direction : output; function : \"A & B\"; }\n"
        "}\n";
    ASSERT_TRUE(load_liberty_string(src, lib, &err)) << err;
    EXPECT_EQ(lib.size(), 1u);
    EXPECT_EQ(lib.lookup("AND2_X1")->logic_type_hint, 1);
}

TEST(LibertyTest, LowPowerCells) {
    // Power-aware annotation wins over the functional classification, and
    // retention (save/restore) flops classify by name (glob heuristics
    // mirroring CellLibrary::with_defaults()).
    CellLibrary lib;
    std::string err;
    const char* src =
        "library (lp) {\n"
        "  cell (ISO_X1) {\n"
        "    is_isolation_cell : true;\n"
        "    pin (A) { direction : input; }\n"
        "    pin (SE) { direction : input; }\n"
        "    pin (Z) { direction : output; }\n"
        "  }\n"
        "  cell (CLAMPBUF) {\n"
        "    is_isolation_cell : true;\n"
        "    pin (A) { direction : input; }\n"
        "    pin (Z) { direction : output; }\n"
        "  }\n"
        "  cell (RSDFFQ_X1) {\n"
        "    ff (IQ, IQN) { next_state : \"D\"; clocked_on : \"CK\"; }\n"
        "    pin (CK) { direction : input; clock : true; }\n"
        "    pin (D) { direction : input; }\n"
        "    pin (SI) { direction : input; }\n"
        "    pin (SE) { direction : input; }\n"
        "    pin (Q) { direction : output; }\n"
        "  }\n"
        "}\n";
    ASSERT_TRUE(load_liberty_string(src, lib, &err)) << err;

    // Even without "ISO" in the name, the explicit annotation classifies.
    const CellDef* clamp = lib.lookup("CLAMPBUF");
    ASSERT_NE(clamp, nullptr);
    EXPECT_EQ(clamp->kind, CellKind::Isolation);
    EXPECT_EQ(clamp->output_pin, "Z");

    // Retention flop: sequential like a plain Dff; scan/save ports are
    // ignored by the adapter (only CK/D/Q bind).
    const CellDef* ret = lib.lookup("RSDFFQ_X1");
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->kind, CellKind::Retention);
    EXPECT_EQ(ret->clock_pin, "CK");
    EXPECT_EQ(ret->data_pin, "D");
    EXPECT_EQ(ret->output_pin, "Q");
}

}  // namespace
}  // namespace opencdc::cell
