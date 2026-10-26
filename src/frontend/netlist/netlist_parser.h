#ifndef OPENCDC_FRONTEND_NETLIST_NETLIST_PARSER_H
#define OPENCDC_FRONTEND_NETLIST_NETLIST_PARSER_H

#include <cstdint>
#include <string>
#include <vector>

namespace opencdc::frontend::netlist {

enum class PortDir { Input, Output, Inout };

// A net reference inside a pin connection or assign.
struct NetRef {
    enum class Kind { Net, Const, Concat };
    Kind kind = Kind::Net;
    std::string net;       // base net name (Kind::Net)
    bool has_bit = false;  // bit-select `net[3]`
    uint32_t bit = 0;
    char const_val = '0';             // Kind::Const: '0' | '1' | 'x' | 'z'
    std::vector<std::string> concat;  // Kind::Concat: member net names
};

struct PortDecl {
    std::string name;
    PortDir dir = PortDir::Input;
    uint32_t msb = 0;
    uint32_t lsb = 0;
    uint32_t width() const {
        return (msb > lsb ? msb - lsb : lsb - msb) + 1;
    }
};

struct NetDecl {
    std::string name;
    uint32_t msb = 0;
    uint32_t lsb = 0;
    uint32_t width() const {
        return (msb > lsb ? msb - lsb : lsb - msb) + 1;
    }
};

struct AssignStmt {
    NetRef lhs;
    NetRef rhs;
    size_t line = 0;
};

struct Conn {
    std::string pin;  // pin name for named connections, empty for positional
    NetRef net;
};

struct Instance {
    std::string cell_type;
    std::string inst_name;
    std::vector<Conn> conns;  // empty pin names ⇒ positional
    size_t line = 0;
    bool positional() const {
        return !conns.empty() && conns.front().pin.empty();
    }
};

struct NetlistModule {
    std::string name;
    std::vector<PortDecl> ports;
    std::vector<NetDecl> nets;
    std::vector<AssignStmt> assigns;
    std::vector<Instance> instances;
};

// Parse result: the chosen top module plus, when the design contains more
// than one module, the submodule definitions (moved, excluding the top) that
// hierarchical elaboration (flatten.cpp) needs.
struct ParseResult {
    NetlistModule module;
    std::vector<NetlistModule> modules;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    bool ok = false;
};

// Parse a gate-level Verilog netlist (flat, Verilog-2001/SV subset). `top` is
// the desired top module; when empty or not found, a single-module design is
// accepted and multi-module designs error asking for an explicit top.
ParseResult parse_netlist(const std::vector<std::string>& sources,
                          const std::vector<std::string>& filenames, const std::string& top);

}  // namespace opencdc::frontend::netlist

#endif  // OPENCDC_FRONTEND_NETLIST_NETLIST_PARSER_H
