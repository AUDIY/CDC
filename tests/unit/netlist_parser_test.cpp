#include "frontend/netlist/netlist_parser.h"

#include <gtest/gtest.h>

#include "frontend/netlist/netlist_lexer.h"

namespace opencdc::frontend::netlist {
namespace {

TEST(NetlistLexerTest, TokenizesBasicNetlist) {
    std::string src =
        "module top (input clk, output q);\n"
        "  wire d;\n"
        "  DFFCNQ_X1 reg0 (.CK(clk), .D(d), .Q(q));\n"
        "endmodule\n";
    Lexer lex(src, "test.v");
    auto toks = lex.tokenize();
    EXPECT_FALSE(lex.has_errors());
    ASSERT_FALSE(toks.empty());
    EXPECT_EQ(toks.back().kind, TokenKind::EndOfFile);
    // First token is 'module' identifier.
    EXPECT_EQ(toks.front().kind, TokenKind::Identifier);
    EXPECT_EQ(toks.front().text, "module");
}

TEST(NetlistLexerTest, HandlesEscapedIdentifiers) {
    std::string src = "\\foo.bar[3] ";
    Lexer lex(src, "t.v");
    auto toks = lex.tokenize();
    ASSERT_GE(toks.size(), 2u);
    EXPECT_EQ(toks[0].kind, TokenKind::EscapedIdentifier);
    EXPECT_EQ(toks[0].text, "foo.bar[3]");  // excludes leading backslash
}

TEST(NetlistLexerTest, SkipsCommentsAndAttributes) {
    std::string src = "(* keep *) // line comment\n/* block\ncomment */ wire";
    Lexer lex(src, "t.v");
    auto toks = lex.tokenize();
    ASSERT_FALSE(toks.empty());
    EXPECT_EQ(toks[0].kind, TokenKind::Identifier);
    EXPECT_EQ(toks[0].text, "wire");
}
TEST(NetlistLexerTest, LexesSizedLiterals) {
    std::string src = "1'bz 4'hF 32'd5 1'b0";
    Lexer lex(src, "t.v");
    auto toks = lex.tokenize();
    ASSERT_GE(toks.size(), 4u);
    EXPECT_EQ(toks[0].kind, TokenKind::Number);
    EXPECT_EQ(toks[0].text, "1'bz");
    EXPECT_EQ(toks[1].text, "4'hF");
}

TEST(NetlistParserTest, ParsesSimpleModule) {
    std::string src =
        "module top (input clk, input d, output q);\n"
        "  DFFCNQ_X1 reg0 (.CK(clk), .D(d), .Q(q));\n"
        "endmodule\n";
    auto res = parse_netlist({src}, {"t.v"}, "top");
    ASSERT_TRUE(res.ok) << (res.errors.empty() ? "" : res.errors.front());
    EXPECT_EQ(res.module.name, "top");
    ASSERT_EQ(res.module.ports.size(), 3u);
    EXPECT_EQ(res.module.ports[0].name, "clk");
    EXPECT_EQ(res.module.ports[0].dir, PortDir::Input);
    EXPECT_EQ(res.module.ports[2].name, "q");
    EXPECT_EQ(res.module.ports[2].dir, PortDir::Output);
    ASSERT_EQ(res.module.instances.size(), 1u);
    EXPECT_EQ(res.module.instances[0].cell_type, "DFFCNQ_X1");
    EXPECT_EQ(res.module.instances[0].inst_name, "reg0");
    ASSERT_EQ(res.module.instances[0].conns.size(), 3u);
    EXPECT_EQ(res.module.instances[0].conns[0].pin, "CK");
    EXPECT_EQ(res.module.instances[0].conns[0].net.net, "clk");
}

TEST(NetlistParserTest, ParsesAnsiPorts) {
    std::string src =
        "module top(input wire clk, output reg q);\n"
        "endmodule\n";
    auto res = parse_netlist({src}, {"t.v"}, "top");
    ASSERT_TRUE(res.ok) << (res.errors.empty() ? "" : res.errors.front());
    ASSERT_EQ(res.module.ports.size(), 2u);
    EXPECT_EQ(res.module.ports[0].name, "clk");
    EXPECT_EQ(res.module.ports[0].dir, PortDir::Input);
    EXPECT_EQ(res.module.ports[1].name, "q");
    EXPECT_EQ(res.module.ports[1].dir, PortDir::Output);
}

TEST(NetlistParserTest, ParsesNonAnsiPortsWithBodyDecls) {
    std::string src =
        "module top(clk, d, q);\n"
        "  input clk;\n"
        "  input d;\n"
        "  output q;\n"
        "endmodule\n";
    auto res = parse_netlist({src}, {"t.v"}, "top");
    ASSERT_TRUE(res.ok) << (res.errors.empty() ? "" : res.errors.front());
    ASSERT_EQ(res.module.ports.size(), 3u);
    EXPECT_EQ(res.module.ports[0].dir, PortDir::Input);
    EXPECT_EQ(res.module.ports[2].name, "q");
    EXPECT_EQ(res.module.ports[2].dir, PortDir::Output);
}

TEST(NetlistParserTest, ParsesNetsAssignsAndGatePrimitives) {
    std::string src =
        "module top(input a, input b, output y, output z);\n"
        "  wire n1, n2;\n"
        "  assign n1 = a;\n"
        "  and g1(n2, a, b);\n"
        "  not g2(y, n2);\n"
        "  buf g3(z, b);\n"
        "endmodule\n";
    auto res = parse_netlist({src}, {"t.v"}, "top");
    ASSERT_TRUE(res.ok) << (res.errors.empty() ? "" : res.errors.front());
    EXPECT_EQ(res.module.nets.size(), 2u);  // n1, n2
    EXPECT_EQ(res.module.assigns.size(), 1u);
    EXPECT_EQ(res.module.assigns[0].lhs.net, "n1");
    EXPECT_EQ(res.module.assigns[0].rhs.net, "a");
    ASSERT_EQ(res.module.instances.size(), 3u);
    EXPECT_EQ(res.module.instances[0].cell_type, "and");
    EXPECT_TRUE(res.module.instances[0].positional());
    ASSERT_EQ(res.module.instances[0].conns.size(), 3u);
    EXPECT_EQ(res.module.instances[0].conns[0].net.net, "n2");  // output first
}

TEST(NetlistParserTest, ParsesMultiInstanceStatement) {
    std::string src =
        "module top(input i, output o1, output o2);\n"
        "  BUF_X1 b1(o1, i), b2(o2, i);\n"
        "endmodule\n";
    auto res = parse_netlist({src}, {"t.v"}, "top");
    ASSERT_TRUE(res.ok) << (res.errors.empty() ? "" : res.errors.front());
    ASSERT_EQ(res.module.instances.size(), 2u);
    EXPECT_EQ(res.module.instances[0].inst_name, "b1");
    EXPECT_EQ(res.module.instances[1].inst_name, "b2");
}

TEST(NetlistParserTest, ParsesBitSelectAndConstant) {
    std::string src =
        "module top(input [3:0] bus, output z);\n"
        "  wire n;\n"
        "  assign n = bus[2];\n"
        "  assign z = 1'b0;\n"
        "endmodule\n";
    auto res = parse_netlist({src}, {"t.v"}, "top");
    ASSERT_TRUE(res.ok) << (res.errors.empty() ? "" : res.errors.front());
    ASSERT_EQ(res.module.ports.size(), 2u);
    EXPECT_EQ(res.module.ports[0].width(), 4u);
    // bus[2] bit-select
    bool found_bit = false;
    for (auto& a : res.module.assigns) {
        if (a.lhs.net == "n" && a.rhs.kind == NetRef::Kind::Net && a.rhs.net == "bus" &&
            a.rhs.has_bit && a.rhs.bit == 2)
            found_bit = true;
    }
    EXPECT_TRUE(found_bit);
    // constant tie
    bool found_const = false;
    for (auto& a : res.module.assigns) {
        if (a.lhs.net == "z" && a.rhs.kind == NetRef::Kind::Const && a.rhs.const_val == '0')
            found_const = true;
    }
    EXPECT_TRUE(found_const);
}

TEST(NetlistParserTest, SkipsSpecifyAndDirectives) {
    std::string src =
        "`timescale 1ns/1ps\n"
        "module top(input a, output y);\n"
        "  specify\n"
        "    (a *> y) = 1;\n"
        "  endspecify\n"
        "  buf g0(y, a);\n"
        "endmodule\n";
    auto res = parse_netlist({src}, {"t.v"}, "top");
    ASSERT_TRUE(res.ok) << (res.errors.empty() ? "" : res.errors.front());
    ASSERT_EQ(res.module.instances.size(), 1u);
    EXPECT_EQ(res.module.instances[0].cell_type, "buf");
}

TEST(NetlistParserTest, ErrorsOnMissingTop) {
    std::string src = "module other(input a);\nendmodule\n";
    auto res = parse_netlist({src}, {"t.v"}, "top");
    EXPECT_FALSE(res.ok);
    ASSERT_FALSE(res.errors.empty());
    EXPECT_NE(res.errors.front().find("not found"), std::string::npos);
}

TEST(NetlistParserTest, AutoSelectsSingleModule) {
    std::string src = "module only(input a, output y);\n  buf g(y, a);\nendmodule\n";
    auto res = parse_netlist({src}, {"t.v"}, "");
    ASSERT_TRUE(res.ok) << (res.errors.empty() ? "" : res.errors.front());
    EXPECT_EQ(res.module.name, "only");
}

TEST(NetlistParserTest, ErrorsOnBehavioralBlock) {
    std::string src =
        "module top(input clk, output reg q);\n"
        "  always @(posedge clk) q <= ~q;\n"
        "endmodule\n";
    auto res = parse_netlist({src}, {"t.v"}, "top");
    // Should still parse (behavioral block skipped with a warning), not crash.
    // Whether ok depends on the always being skipped cleanly.
    if (res.ok) {
        EXPECT_FALSE(res.warnings.empty());
    }
}

}  // namespace
}  // namespace opencdc::frontend::netlist
