#include "cell/cell_library.h"

#include <gtest/gtest.h>

namespace opencdc::cell {
namespace {

TEST(CellLibraryTest, GlobMatchBasics) {
    EXPECT_TRUE(glob_match("*", "anything"));
    EXPECT_TRUE(glob_match("", ""));
    EXPECT_TRUE(glob_match("DFF*", "DFFCNQ_X1"));
    EXPECT_TRUE(glob_match("DFCNQ_*", "DFCNQ_X1"));
    EXPECT_FALSE(glob_match("DFCNQ_*", "DFFX1"));
    EXPECT_TRUE(glob_match("D?F*", "DXFZ"));
    EXPECT_FALSE(glob_match("D?F*", "DFZ"));  // '?' needs exactly one char
    EXPECT_TRUE(glob_match("*BUF*", "CLKBUF_X2"));
    EXPECT_TRUE(glob_match("CLK*", "CLKBUF_X2"));
    EXPECT_FALSE(glob_match("CLK*", "DFF_CLK"));  // must match from start
    EXPECT_TRUE(glob_match("A*B*C", "AXBXC"));
    EXPECT_TRUE(glob_match("A*B*C", "ABC"));
    EXPECT_TRUE(glob_match("A*B*C", "ABXC"));  // * spans the X
    EXPECT_FALSE(glob_match("A*B*C", "AC"));   // B is required
}

TEST(CellLibraryTest, KindNamesRoundTrip) {
    CellKind kinds[] = {CellKind::Dff,         CellKind::DffReset, CellKind::DffEnable,
                        CellKind::DffSetReset, CellKind::Latch,    CellKind::Icg,
                        CellKind::Buffer,      CellKind::Inverter, CellKind::Comb,
                        CellKind::Isolation,   CellKind::Retention};
    for (CellKind k : kinds) {
        CellKind parsed;
        ASSERT_TRUE(parse_cell_kind(cell_kind_name(k), parsed)) << cell_kind_name(k);
        EXPECT_EQ(parsed, k);
    }
    CellKind out;
    EXPECT_FALSE(parse_cell_kind("bogus", out));
    EXPECT_FALSE(parse_cell_kind("", out));
}

TEST(CellLibraryTest, DefaultsClassify) {
    CellLibrary lib = CellLibrary::with_defaults();
    EXPECT_EQ(lib.classify("DFF_X1"), CellKind::Dff);
    EXPECT_EQ(lib.classify("DFCNQ_X1"), CellKind::DffReset);
    EXPECT_EQ(lib.classify("DLATCH_X1"), CellKind::Latch);
    EXPECT_EQ(lib.classify("CLKGATE_X1"), CellKind::Icg);
    EXPECT_EQ(lib.classify("BUFX2"), CellKind::Buffer);
    EXPECT_EQ(lib.classify("INV_X1"), CellKind::Inverter);
    EXPECT_EQ(lib.classify("AND2_X1"), CellKind::Comb);
    EXPECT_EQ(lib.classify("UNKNOWN_CELL"), CellKind::Unknown);
    EXPECT_EQ(lib.classify("random_ip_block"), CellKind::Unknown);
}

TEST(CellLibraryTest, DefaultsClassifyLowPowerCells) {
    CellLibrary lib = CellLibrary::with_defaults();
    // Isolation clamps: transparent combinational (Buffer-equivalent).
    EXPECT_EQ(lib.classify("ISO_X1"), CellKind::Isolation);
    EXPECT_EQ(lib.classify("ENISO_X2"), CellKind::Isolation);
    // Retention (save/restore) flops: sequential like a plain Dff.
    EXPECT_EQ(lib.classify("RSDFFQ_X1"), CellKind::Retention);
    EXPECT_EQ(lib.classify("ENRSDFF_X1"), CellKind::Retention);
    // Non-collisions: functional names must keep their kinds.
    EXPECT_EQ(lib.classify("DFF_X1"), CellKind::Dff);
    EXPECT_EQ(lib.classify("SDFF_X1"), CellKind::Dff);  // scan DFF is not retention
    EXPECT_EQ(lib.classify("DFCNQ_X1"), CellKind::DffReset);
    EXPECT_EQ(lib.classify("BUF_X1"), CellKind::Buffer);
    EXPECT_EQ(lib.classify("$_DFF_P_"), CellKind::Dff);
    EXPECT_EQ(lib.classify("$_MUX_"), CellKind::Comb);
}

TEST(CellLibraryTest, SpecificPatternShadowsDefault) {
    CellLibrary lib = CellLibrary::with_defaults();
    // Default maps DFF* -> Dff. A user entry DFCNQ_* -> DffReset has a longer
    // (more specific) pattern and must win for DFCNQ_* while DFF* still maps
    // to Dff.
    CellDef user;
    user.pattern = "DFCNQ_*";
    user.kind = CellKind::DffReset;
    user.clock_pin = "CK";
    user.data_pin = "D";
    user.output_pin = "Q";
    user.reset_pin = "CDN";
    user.reset_active_low = true;
    user.reset_async = true;
    lib.add(user);

    EXPECT_EQ(lib.classify("DFCNQ_X1"), CellKind::DffReset);
    EXPECT_EQ(lib.classify("DFF_X1"), CellKind::Dff);

    const CellDef* def = lib.lookup("DFCNQ_X1");
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->clock_pin, "CK");
    EXPECT_TRUE(def->reset_active_low);
    EXPECT_TRUE(def->reset_async);
}

TEST(CellLibraryTest, LongestMatchWinsRegardlessOfOrder) {
    CellLibrary lib;
    CellDef broad;
    broad.pattern = "DFF*";
    broad.kind = CellKind::Dff;
    CellDef specific;
    specific.pattern = "DFFSR_*";
    specific.kind = CellKind::DffSetReset;
    // Insert specific FIRST; longest-match must still prefer it for DFFSR_*.
    lib.add(specific);
    lib.add(broad);
    EXPECT_EQ(lib.classify("DFFSR_X1"), CellKind::DffSetReset);
    EXPECT_EQ(lib.classify("DFF_X1"), CellKind::Dff);
}

TEST(CellLibraryTest, LoadFromYaml) {
    const std::string yaml = R"(
cells:
  - pattern: "DFCNQ_*"
    kind: dff_reset
    clock: CK
    data: D
    output: Q
    reset: CDN
    reset_active_low: true
    reset_async: true
  - pattern: "MYICG*"
    kind: icg
    clock: CLK
    enable: EN
    output: GCK
)";
    CellLibrary lib;
    std::string err;
    ASSERT_TRUE(CellLibrary::load_from_yaml_string(yaml, lib, &err)) << err;
    EXPECT_EQ(lib.size(), 2u);

    const CellDef* dff = lib.lookup("DFCNQ_LV");
    ASSERT_NE(dff, nullptr);
    EXPECT_EQ(dff->kind, CellKind::DffReset);
    EXPECT_EQ(dff->reset_pin, "CDN");
    EXPECT_TRUE(dff->reset_active_low);

    const CellDef* icg = lib.lookup("MYICG_1");
    ASSERT_NE(icg, nullptr);
    EXPECT_EQ(icg->kind, CellKind::Icg);
    EXPECT_EQ(icg->enable_pin, "EN");
}

TEST(CellLibraryTest, LoadFromYamlLowPowerKinds) {
    const std::string yaml = R"(
cells:
  - pattern: "CLAMPX*"
    kind: isolation
    output: Z
  - pattern: "MYRET*"
    kind: retention
    clock: CK
    data: D
    output: Q
)";
    CellLibrary lib;
    std::string err;
    ASSERT_TRUE(CellLibrary::load_from_yaml_string(yaml, lib, &err)) << err;
    const CellDef* iso = lib.lookup("CLAMPX_1");
    ASSERT_NE(iso, nullptr);
    EXPECT_EQ(iso->kind, CellKind::Isolation);
    EXPECT_EQ(iso->output_pin, "Z");
    const CellDef* ret = lib.lookup("MYRET_Q");
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->kind, CellKind::Retention);
    EXPECT_EQ(ret->clock_pin, "CK");
}

TEST(CellLibraryTest, LoadFromYamlErrors) {
    CellLibrary lib;
    std::string err;
    // Missing kind.
    EXPECT_FALSE(CellLibrary::load_from_yaml_string("cells:\n  - pattern: \"X*\"\n", lib, &err));
    EXPECT_FALSE(err.empty());
    // Missing pattern.
    err.clear();
    EXPECT_FALSE(CellLibrary::load_from_yaml_string("cells:\n  - kind: dff\n", lib, &err));
    EXPECT_FALSE(err.empty());
    // Bad kind.
    err.clear();
    EXPECT_FALSE(CellLibrary::load_from_yaml_string("cells:\n  - pattern: \"X*\"\n    kind: nope\n",
                                                    lib, &err));
    EXPECT_FALSE(err.empty());
    // Not a map.
    err.clear();
    EXPECT_FALSE(CellLibrary::load_from_yaml_string("- a\n- b\n", lib, &err));
    EXPECT_FALSE(err.empty());
    // Malformed YAML.
    err.clear();
    EXPECT_FALSE(CellLibrary::load_from_yaml_string("cells: [oops", lib, &err));
    EXPECT_FALSE(err.empty());
}

TEST(CellLibraryTest, ClassifiesYosysGenericCells) {
    // Post-yosys `write_verilog` output uses these escaped generic cell names.
    CellLibrary lib = CellLibrary::with_defaults();
    EXPECT_EQ(lib.classify("$_DFF_P_"), CellKind::Dff);
    EXPECT_EQ(lib.classify("$_DFFE_PN0P_"), CellKind::DffEnable);
    EXPECT_EQ(lib.classify("$_DFF_PN0_"), CellKind::DffReset);
    EXPECT_EQ(lib.classify("$_DLATCH_N_"), CellKind::Latch);
    EXPECT_EQ(lib.classify("$_AND_"), CellKind::Comb);
    EXPECT_EQ(lib.classify("$_NAND_"), CellKind::Comb);
    EXPECT_EQ(lib.classify("$_MUX_"), CellKind::Comb);
    EXPECT_EQ(lib.classify("$_NOT_"), CellKind::Inverter);
    EXPECT_EQ(lib.classify("$_BUF_"), CellKind::Buffer);
    // Longest-match: reset variant beats the plain DFF pattern.
    EXPECT_EQ(lib.classify("$_DFF_NN1_"), CellKind::DffReset);
}

TEST(CellLibraryTest, ClassifiesYosysSyncResetEnableFamilies) {
    // $_SDFFE_*/$_SDFFCE_* are sync-reset-with-enable flops emitted by yosys
    // for `always @(posedge clk) if (en) q <= r ? val : d`. They must classify
    // as sequential — never Unknown (which models them as opaque combinational
    // and silently drops every crossing through them).
    CellLibrary lib = CellLibrary::with_defaults();
    EXPECT_EQ(lib.classify("$_SDFFE_PP0P_"), CellKind::DffEnable);
    EXPECT_EQ(lib.classify("$_SDFFE_PN0P_"), CellKind::DffEnable);
    EXPECT_EQ(lib.classify("$_SDFFCE_PN0P_"), CellKind::DffEnable);
    EXPECT_EQ(lib.classify("$_SDFFCE_PP1N_"), CellKind::DffEnable);
    EXPECT_EQ(lib.classify("$_SDFF_PN0_"), CellKind::Dff);
    EXPECT_EQ(lib.classify("$_SDFF_PP1_"), CellKind::Dff);
    // Set/reset and async-load families.
    EXPECT_EQ(lib.classify("$_DFFSR_PPP_"), CellKind::DffSetReset);
    EXPECT_EQ(lib.classify("$_SDFFSR_PPP_"), CellKind::DffSetReset);
    EXPECT_EQ(lib.classify("$_DFFS_PN_"), CellKind::DffSetReset);
    EXPECT_EQ(lib.classify("$_DFFR_PN_"), CellKind::DffReset);
    EXPECT_EQ(lib.classify("$_ALDFF_PN_"), CellKind::Dff);
    EXPECT_EQ(lib.classify("$_ALDFFE_PPP_"), CellKind::DffEnable);
}

}  // namespace
}  // namespace opencdc::cell
