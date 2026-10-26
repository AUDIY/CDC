#include "frontend/netlist/netlist_parser.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "frontend/netlist/netlist_lexer.h"

namespace opencdc::frontend::netlist {

namespace {

const std::unordered_set<std::string>& gate_primitives() {
    // Built-in gate primitives (positional; first port is the output). Map to
    // combinational/passthrough logic in the adapter.
    static const std::unordered_set<std::string> kGates = {
        "and", "or",     "not",    "nand",   "nor",    "xor",    "xnor",
        "buf", "bufif0", "bufif1", "notif0", "notif1", "pullup", "pulldown"};
    return kGates;
}

// Netlist keywords that terminate or redirect parsing.
bool is_decl_keyword(const std::string& s) {
    return s == "input" || s == "output" || s == "inout" || s == "wire" || s == "reg" ||
           s == "logic" || s == "tri" || s == "tri0" || s == "tri1" || s == "supply0" ||
           s == "supply1" || s == "wand" || s == "wor";
}

}  // namespace

namespace {

class Parser {
   public:
    Parser(const std::vector<Token>& tokens, const std::string& filename, const std::string& top)
        : tokens_(tokens), filename_(filename), top_(top) {}

    ParseResult run() {
        ParseResult result;
        result_ = &result;
        std::vector<NetlistModule> modules;
        while (!at_end()) {
            if (is_kw("module")) {
                modules.push_back(parse_module());
                if (has_errors())
                    break;
            } else if (is_kw("endmodule")) {
                error("endmodule without matching module");
                break;
            } else if (is_kw("timescale") || is_kw("celldefine") || is_kw("endcelldefine") ||
                       is_kw("default_nettype") || is_kw("unconnected_drive") ||
                       is_kw("nounconnected_drive")) {
                skip_statement();
            } else if (is_sym("`")) {
                // Compiler directive (`include, `timescale, `ifdef family, ...).
                skip_directive();
            } else if (is_kw("config")) {
                error("elaboration configs are not supported; flatten the netlist first");
                break;
            } else {
                error("unexpected token '" + peek().text + "' at top level");
                break;
            }
        }

        if (has_errors()) {
            result.ok = false;
            return result;
        }

        // Select the top module.
        if (modules.empty()) {
            error("no module found in netlist");
            result.ok = false;
            return result;
        }
        const NetlistModule* chosen = nullptr;
        if (!top_.empty()) {
            for (const auto& m : modules) {
                if (m.name == top_) {
                    chosen = &m;
                    break;
                }
            }
            if (!chosen) {
                error("top module '" + top_ + "' not found in netlist");
                result.ok = false;
                return result;
            }
        } else if (modules.size() == 1) {
            chosen = &modules.front();
        } else {
            error("netlist defines " + std::to_string(modules.size()) +
                  " modules; specify --top <module> to select one");
            result.ok = false;
            return result;
        }

        // The chosen module becomes the elaboration root; every other module
        // definition is retained (moved) for hierarchical elaboration —
        // submodule instances are flattened against them at build time.
        result.module = *chosen;
        for (auto& m : modules) {
            if (&m != chosen)
                result.modules.push_back(std::move(m));
        }
        result.ok = true;
        return result;
    }

   private:
    // --- token cursor ---------------------------------------------------
    const Token& peek(size_t ahead = 0) const {
        size_t i = pos_ + ahead;
        return i < tokens_.size() ? tokens_[i] : tokens_.back();
    }
    bool at_end() const {
        return peek().kind == TokenKind::EndOfFile;
    }
    const Token& advance() {
        const Token& t = peek();
        if (!at_end())
            ++pos_;
        return t;
    }
    bool is_kw(const std::string& kw) const {
        return peek().kind == TokenKind::Identifier && peek().text == kw;
    }
    bool is_sym(const std::string& s) const {
        return peek().kind == TokenKind::Symbol && peek().text == s;
    }
    bool match_sym(const std::string& s) {
        if (is_sym(s)) {
            advance();
            return true;
        }
        return false;
    }
    bool match_kw(const std::string& s) {
        if (is_kw(s)) {
            advance();
            return true;
        }
        return false;
    }

    std::string token_name(const Token& t) const {
        return t.kind == TokenKind::Symbol ? "'" + t.text + "'" : t.text;
    }

    void error(const std::string& msg) {
        const Token& t = peek();
        result_->errors.push_back(filename_ + ":" + std::to_string(t.line) + ":" +
                                  std::to_string(t.col) + ": " + msg);
    }
    void warn(size_t line, size_t col, const std::string& msg) {
        result_->warnings.push_back(filename_ + ":" + std::to_string(line) + ":" +
                                    std::to_string(col) + ": " + msg);
    }
    bool has_errors() const {
        return !result_->errors.empty();
    }

    // --- skipping -------------------------------------------------------
    // Skip a statement up to and including the next ';'.
    void skip_statement() {
        while (!at_end() && !is_sym(";"))
            advance();
        if (is_sym(";"))
            advance();
    }
    // Skip balanced parentheses (used for parameter lists, attribute-like args).
    void skip_parens() {
        int depth = 0;
        while (!at_end()) {
            if (is_sym("("))
                ++depth;
            else if (is_sym(")")) {
                --depth;
                if (depth == 0) {
                    advance();
                    return;
                }
            }
            advance();
        }
    }
    // Skip an `include`/`define`/`ifdef family/`pragma directive. The lexer
    // emits '`' as a Symbol followed by the directive-name Identifier. We skip
    // the '`', the name, and the remainder of the logical line (stopping early
    // at a structural keyword or a new directive).
    void skip_directive() {
        advance();  // '`'
        if (peek().kind == TokenKind::Identifier)
            advance();  // directive name
        size_t start_line = peek().line;
        while (!at_end()) {
            if (is_sym("`"))
                break;  // next directive
            if (is_kw("module") || is_kw("endmodule") || is_kw("input") || is_kw("output") ||
                is_kw("inout") || is_kw("wire") || is_kw("assign") || is_kw("supply0") ||
                is_kw("supply1"))
                break;
            if (peek().line != start_line && start_line != 0 && peek().kind != TokenKind::Symbol)
                break;
            advance();
        }
    }

    // --- ranges / identifiers ------------------------------------------
    // Parses an optional `[msb:lsb]` (or `[expr]`) range. Returns width bounds.
    void parse_range(uint32_t& msb, uint32_t& lsb) {
        msb = 0;
        lsb = 0;
        if (!is_sym("["))
            return;
        advance();
        uint32_t a = parse_int_expr();
        if (match_sym(":")) {
            uint32_t b = parse_int_expr();
            msb = a;
            lsb = b;
        } else {
            // Single-bit select used as a range is not valid in a declaration,
            // but tolerate it.
            msb = a;
            lsb = a;
        }
        match_sym("]");
    }

    // Parse a (possibly simple) integer: number literal or identifier token.
    // Non-numeric expressions degrade to 0 (we don't need symbolic widths for
    // flat gate-level netlists where buses are already bit-blasted).
    uint32_t parse_int_expr() {
        if (peek().kind == TokenKind::Number) {
            std::string num = advance().text;
            return number_value(num);
        }
        if (peek().kind == TokenKind::Identifier) {
            advance();  // symbolic constant — value unknown, use 0
            return 0;
        }
        if (is_sym("-")) {
            advance();
            return parse_int_expr();
        }
        return 0;
    }

    // Extract an integer value from a Verilog number literal like 4'hF, 1'b0,
    // 32'd5, or a plain decimal. Unbased literals ('b101) and x/z → 0.
    static uint32_t number_value(const std::string& lit) {
        auto tick = lit.find('\'');
        std::string digits;
        bool has_s = false;
        size_t start = 0;
        if (tick == std::string::npos) {
            digits = lit;
        } else {
            // [size]'[s]base digits
            std::string base_part = lit.substr(tick + 1);
            if (!base_part.empty() && (base_part[0] == 's' || base_part[0] == 'S')) {
                has_s = true;
                base_part = base_part.substr(1);
            }
            (void)has_s;
            if (base_part.empty())
                return 0;
            char base = base_part[0];
            digits = base_part.substr(1);
            int radix = 10;
            if (base == 'h' || base == 'H')
                radix = 16;
            else if (base == 'b' || base == 'B')
                radix = 2;
            else if (base == 'o' || base == 'O')
                radix = 8;
            else if (base == 'd' || base == 'D')
                radix = 10;
            else
                return 0;
            uint32_t value = 0;
            for (char c : digits) {
                if (c == '_')
                    continue;
                int digit;
                if (c >= '0' && c <= '9')
                    digit = c - '0';
                else if (c >= 'a' && c <= 'f')
                    digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    digit = c - 'A' + 10;
                else
                    return value;  // x/z or invalid → stop
                if (digit >= radix)
                    return value;
                value = value * static_cast<uint32_t>(radix) + static_cast<uint32_t>(digit);
            }
            return value;
        }
        // Plain decimal (no tick).
        uint32_t value = 0;
        for (char c : digits) {
            if (c >= '0' && c <= '9')
                value = value * 10 + static_cast<uint32_t>(c - '0');
        }
        (void)start;
        return value;
    }

    // Net name: identifier or escaped identifier. Returns "" on failure.
    std::string parse_net_name() {
        if (peek().kind == TokenKind::Identifier || peek().kind == TokenKind::EscapedIdentifier) {
            return advance().text;
        }
        return "";
    }

    // A constant literal like 1'b0 / 1'bx / 4'hF → NetRef::Const. Returns false
    // if the token is not a constant.
    bool parse_constant(NetRef& out) {
        if (peek().kind != TokenKind::Number)
            return false;
        std::string lit = peek().text;
        auto tick = lit.find('\'');
        if (tick == std::string::npos) {
            // Plain integer used as a constant (tie to 0/1).
            advance();
            out.kind = NetRef::Kind::Const;
            out.const_val = (number_value(lit) != 0) ? '1' : '0';
            return true;
        }
        // [size]'[s]<base><digits>
        std::string after = lit.substr(tick + 1);
        if (!after.empty() && (after[0] == 's' || after[0] == 'S'))
            after = after.substr(1);
        if (after.empty())
            return false;
        std::string digits = after.substr(1);
        advance();
        out.kind = NetRef::Kind::Const;
        // x/z detection first (they dominate the constant's CDC meaning).
        for (char c : digits) {
            if (c == 'x' || c == 'X') {
                out.const_val = 'x';
                return true;
            }
            if (c == 'z' || c == 'Z') {
                out.const_val = 'z';
                return true;
            }
        }
        out.const_val = (number_value(lit) != 0) ? '1' : '0';
        return true;
    }

    // Parse a net reference: constant | identifier[sel] | {concat}.
    NetRef parse_net_ref() {
        NetRef ref;
        if (is_sym("{")) {
            advance();
            ref.kind = NetRef::Kind::Concat;
            while (!at_end() && !is_sym("}")) {
                NetRef part = parse_net_ref();
                if (part.kind == NetRef::Kind::Net)
                    ref.concat.push_back(part.net);
                // Nested concat/const members are flattened/ignored for MVP.
                if (!match_sym(","))
                    break;
            }
            match_sym("}");
            return ref;
        }
        if (parse_constant(ref))
            return ref;
        std::string name = parse_net_name();
        if (name.empty()) {
            error("expected a net name");
            return ref;
        }
        ref.kind = NetRef::Kind::Net;
        ref.net = name;
        if (is_sym("[")) {
            advance();
            uint32_t sel = parse_int_expr();
            if (match_sym(":")) {
                uint32_t sel2 = parse_int_expr();
                (void)sel2;  // part-select: reference base net (MVP)
            } else {
                ref.has_bit = true;
                ref.bit = sel;
            }
            match_sym("]");
        }
        return ref;
    }

    // --- module ---------------------------------------------------------
    NetlistModule parse_module() {
        NetlistModule m;
        advance();  // 'module'
        m.name = parse_net_name();
        if (m.name.empty()) {
            error("expected module name");
            return m;
        }
        // Optional parameter list #(...) — skipped.
        if (is_sym("#"))
            skip_parens();

        // Port list.
        bool ansi = false;
        if (match_sym("(")) {
            // Detect ANSI (direction keyword inside the port list).
            for (size_t i = pos_; i < tokens_.size(); ++i) {
                const Token& t = tokens_[i];
                if (t.kind == TokenKind::Symbol && t.text == ")")
                    break;
                if (t.kind == TokenKind::Identifier &&
                    (t.text == "input" || t.text == "output" || t.text == "inout")) {
                    ansi = true;
                    break;
                }
            }
            if (ansi) {
                parse_ansi_ports(m);
            } else {
                // Non-ANSI: comma-separated bare names.
                while (!at_end() && !is_sym(")")) {
                    std::string name = parse_net_name();
                    if (!name.empty()) {
                        PortDecl p;
                        p.name = name;
                        p.dir = PortDir::Input;  // direction set by body decl
                        m.ports.push_back(p);
                    }
                    if (!match_sym(","))
                        break;
                }
                match_sym(")");
            }
        }
        match_sym(";");

        // Body.
        while (!at_end() && !is_kw("endmodule")) {
            parse_body_item(m);
            if (has_errors())
                break;
        }
        match_kw("endmodule");
        return m;
    }

    void parse_ansi_ports(NetlistModule& m) {
        PortDir dir = PortDir::Input;
        uint32_t msb = 0, lsb = 0;
        while (!at_end() && !is_sym(")")) {
            if (match_kw("input") || match_kw("output") || match_kw("inout")) {
                dir = (tokens_[pos_ - 1].text == "output")  ? PortDir::Output
                      : (tokens_[pos_ - 1].text == "inout") ? PortDir::Inout
                                                            : PortDir::Input;
                // Optional net type keywords then optional range.
                while (is_decl_keyword(peek().text) && !is_sym(",") && !is_sym(")"))
                    advance();
                parse_range(msb, lsb);
            }
            std::string name = parse_net_name();
            if (!name.empty()) {
                PortDecl p;
                p.name = name;
                p.dir = dir;
                p.msb = msb;
                p.lsb = lsb;
                m.ports.push_back(p);
            }
            if (!match_sym(","))
                break;
        }
        match_sym(")");
        match_sym(";");
    }

    void parse_body_item(NetlistModule& m) {
        if (is_kw("input") || is_kw("output") || is_kw("inout")) {
            parse_port_decl(m);
        } else if (is_kw("wire") || is_kw("reg") || is_kw("logic") || is_kw("tri") ||
                   is_kw("tri0") || is_kw("tri1") || is_kw("supply0") || is_kw("supply1") ||
                   is_kw("wand") || is_kw("wor")) {
            parse_net_decl(m);
        } else if (is_kw("assign")) {
            parse_assign(m);
        } else if (is_kw("parameter") || is_kw("localparam")) {
            skip_statement();
        } else if (is_kw("defparam")) {
            skip_statement();
        } else if (is_kw("specify")) {
            skip_specify();
        } else if (is_kw("initial") || is_kw("always") || is_kw("always_ff") ||
                   is_kw("always_comb") || is_kw("always_latch")) {
            warn(peek().line, peek().col,
                 "behavioral block in netlist; skipped (use a flattened gate-level netlist)");
            skip_block();
        } else if (is_kw("generate")) {
            warn(peek().line, peek().col,
                 "generate block in netlist; skipped (use a flattened gate-level netlist)");
            skip_generate();
        } else if (is_kw("function") || is_kw("task")) {
            warn(peek().line, peek().col, "function/task in netlist; skipped");
            skip_block();
        } else if (is_sym("`")) {
            skip_directive();
        } else {
            // Instance or gate primitive.
            parse_instance(m);
        }
    }

    // Skip a begin...end / fork...join or a single statement (for initial/
    // always/function bodies).
    void skip_block() {
        if (is_kw("function") || is_kw("task")) {
            // Skip to matching endfunction/endtask, handling nested begin.
            advance();
            std::string end_kw = tokens_[pos_ - 1].text == "function" ? "endfunction" : "endtask";
            int depth = 1;
            while (!at_end() && depth > 0) {
                if (is_kw("begin") || is_kw("case"))
                    ++depth;
                if (is_kw(end_kw))
                    --depth;
                advance();
            }
            return;
        }
        // initial/always: expect begin...end or a single statement.
        advance();
        if (is_kw("begin")) {
            int depth = 0;
            while (!at_end()) {
                if (is_kw("begin"))
                    ++depth;
                if (is_kw("end")) {
                    --depth;
                    if (depth == 0) {
                        advance();
                        return;
                    }
                }
                advance();
            }
        } else {
            skip_statement();
        }
    }

    void skip_generate() {
        advance();  // generate
        int depth = 1;
        while (!at_end() && depth > 0) {
            if (is_kw("generate"))
                ++depth;
            if (is_kw("endgenerate"))
                --depth;
            advance();
        }
    }

    void skip_specify() {
        advance();  // specify
        int depth = 1;
        while (!at_end() && depth > 0) {
            if (is_kw("specify"))
                ++depth;
            if (is_kw("endspecify"))
                --depth;
            advance();
        }
    }

    void parse_port_decl(NetlistModule& m) {
        PortDir dir = PortDir::Input;
        if (match_kw("input"))
            dir = PortDir::Input;
        else if (match_kw("output"))
            dir = PortDir::Output;
        else if (match_kw("inout"))
            dir = PortDir::Inout;
        // Optional net type + range.
        while (is_decl_keyword(peek().text) && !is_sym(",") && !is_sym(";"))
            advance();
        uint32_t msb = 0, lsb = 0;
        parse_range(msb, lsb);
        // Update matching port decls or add new ones.
        std::vector<std::string> names;
        while (!at_end() && !is_sym(";")) {
            std::string n = parse_net_name();
            if (!n.empty())
                names.push_back(n);
            uint32_t nmsb = msb, nlsb = lsb;
            if (is_sym("[")) {
                // Per-name range override.
                parse_range(nmsb, nlsb);
            }
            if (!match_sym(","))
                break;
            // Apply per-name range to stored decl below.
            (void)nmsb;
            (void)nlsb;
        }
        match_sym(";");
        for (auto& n : names) {
            auto it = std::find_if(m.ports.begin(), m.ports.end(),
                                   [&](const PortDecl& p) { return p.name == n; });
            if (it != m.ports.end()) {
                it->dir = dir;
                it->msb = msb;
                it->lsb = lsb;
            } else {
                PortDecl p;
                p.name = n;
                p.dir = dir;
                p.msb = msb;
                p.lsb = lsb;
                m.ports.push_back(p);
            }
        }
    }

    void parse_net_decl(NetlistModule& m) {
        advance();  // net type keyword
        // supply0/supply1 declare constant nets; record as normal nets (ties
        // are filtered in the adapter).
        while (is_decl_keyword(peek().text) && !is_sym(",") && !is_sym(";"))
            advance();
        uint32_t msb = 0, lsb = 0;
        parse_range(msb, lsb);
        while (!at_end() && !is_sym(";")) {
            std::string n = parse_net_name();
            if (!n.empty()) {
                NetDecl d;
                d.name = n;
                d.msb = msb;
                d.lsb = lsb;
                m.nets.push_back(d);
            }
            // Skip per-name assignment `= expr`.
            if (match_sym("=")) {
                parse_net_ref();
            }
            if (!match_sym(","))
                break;
        }
        match_sym(";");
    }

    void parse_assign(NetlistModule& m) {
        advance();  // assign
        AssignStmt a;
        a.line = peek().line;
        a.lhs = parse_net_ref();
        if (!match_sym("=")) {
            error("expected '=' in assign");
            return;
        }
        a.rhs = parse_net_ref();
        match_sym(";");
        m.assigns.push_back(std::move(a));
    }

    void parse_instance(NetlistModule& m) {
        // <celltype> [#(...)] <inst> ( conns ) (, <inst> ( conns ))* ;
        std::string cell_type = parse_net_name();
        if (cell_type.empty()) {
            error("expected cell type or gate primitive");
            skip_statement();
            return;
        }
        if (is_sym("#"))
            skip_parens();  // instance parameter override

        while (!at_end() && !is_sym(";")) {
            Instance inst;
            inst.cell_type = cell_type;
            inst.line = peek().line;
            inst.inst_name = parse_net_name();
            if (inst.inst_name.empty()) {
                error("expected instance name for cell '" + cell_type + "'");
                skip_statement();
                return;
            }
            if (!match_sym("(")) {
                error("expected '(' in instance '" + inst.inst_name + "'");
                skip_statement();
                return;
            }
            // Connections.
            bool named = false;
            while (!at_end() && !is_sym(")")) {
                Conn c;
                if (is_sym(".")) {
                    advance();
                    std::string pin = parse_net_name();
                    if (!match_sym("(")) {
                        error("expected '(' after pin name ." + pin);
                        skip_statement();
                        return;
                    }
                    c.pin = pin;
                    c.net = parse_net_ref();
                    if (!match_sym(")")) {
                        error("expected ')' after pin connection ." + pin);
                        skip_statement();
                        return;
                    }
                    named = true;
                } else {
                    // Positional connection (also covers the implicit empty
                    // port `.()` — rare).
                    c.net = parse_net_ref();
                }
                inst.conns.push_back(std::move(c));
                if (!match_sym(","))
                    break;
            }
            match_sym(")");
            m.instances.push_back(std::move(inst));
            if (!match_sym(",")) {
                // Either ';' ends the statement or a stray token.
                break;
            }
        }
        match_sym(";");
    }

    const std::vector<Token>& tokens_;
    std::string filename_;
    std::string top_;
    size_t pos_ = 0;
    ParseResult* result_ = nullptr;
};

}  // namespace

ParseResult parse_netlist(const std::vector<std::string>& sources,
                          const std::vector<std::string>& filenames, const std::string& top) {
    ParseResult result;
    if (sources.empty()) {
        result.errors.push_back("no netlist source provided");
        return result;
    }
    // Concatenate all sources into one token stream. Netlists are typically a
    // single file; multi-file just appends (module selection handles top).
    std::string combined;
    for (const auto& s : sources) {
        combined += s;
        combined += '\n';
    }
    std::string fname = filenames.empty() ? "<netlist>" : filenames.front();

    Lexer lexer(combined, fname);
    std::vector<Token> tokens = lexer.tokenize();
    if (lexer.has_errors()) {
        result.errors = lexer.errors();
        return result;
    }

    Parser parser(tokens, fname, top);
    return parser.run();
}

}  // namespace opencdc::frontend::netlist
