#include "cell/liberty.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace opencdc::cell {

namespace {

// ---- Tokenizer -------------------------------------------------------------
// Minimal Liberty tokenizer: symbols, quoted strings, barewords. Skips
// whitespace, /* */ and // comments, and `directives (backtick-to-EOL), so
// vendor-library noise does not break classification parsing.

struct Tok {
    enum class K { Sym, Str, Word };
    K kind = K::Word;
    std::string text;
    size_t line = 1;
};

class Tokenizer {
   public:
    Tokenizer(const std::string& src, std::string* error) : src_(src), error_(error) {}

    bool run() {
        size_t i = 0;
        while (i < src_.size()) {
            char c = src_[i];
            if (c == '\n') {
                ++line_;
                ++i;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r') {
                ++i;
                continue;
            }
            if (c == '/' && i + 1 < src_.size() && src_[i + 1] == '/') {
                while (i < src_.size() && src_[i] != '\n')
                    ++i;
                continue;
            }
            if (c == '/' && i + 1 < src_.size() && src_[i + 1] == '*') {
                i += 2;
                while (i + 1 < src_.size() && !(src_[i] == '*' && src_[i + 1] == '/')) {
                    if (src_[i] == '\n')
                        ++line_;
                    ++i;
                }
                if (i + 1 >= src_.size()) {
                    fail("unterminated block comment");
                    return false;
                }
                i += 2;
                continue;
            }
            if (c == '`') {  // `define / `include-style directives: skip to EOL
                while (i < src_.size() && src_[i] != '\n')
                    ++i;
                continue;
            }
            if (c == '"' || c == '\'') {
                char q = c;
                size_t start_line = line_;
                size_t j = i + 1;
                std::string value;
                while (j < src_.size() && src_[j] != q) {
                    if (src_[j] == '\\' && j + 1 < src_.size()) {
                        value.push_back(src_[j + 1]);  // keep escaped char raw
                        j += 2;
                        continue;
                    }
                    if (src_[j] == '\n')
                        ++line_;
                    value.push_back(src_[j]);
                    ++j;
                }
                if (j >= src_.size()) {
                    fail("unterminated string literal");
                    return false;
                }
                toks_.push_back({Tok::K::Str, value, start_line});
                i = j + 1;
                continue;
            }
            if (std::strchr("{}();:,", c)) {
                toks_.push_back({Tok::K::Sym, std::string(1, c), line_});
                ++i;
                continue;
            }
            // Bareword: attribute/group names, numbers, booleans. Stop before
            // symbols, quotes, comments, and whitespace.
            size_t start = i;
            while (i < src_.size()) {
                char d = src_[i];
                if (d == ' ' || d == '\t' || d == '\r' || d == '\n' || std::strchr("{}();:,\"/", d))
                    break;
                if (d == '/' && i + 1 < src_.size() && (src_[i + 1] == '/' || src_[i + 1] == '*'))
                    break;
                ++i;
            }
            toks_.push_back({Tok::K::Word, src_.substr(start, i - start), line_});
        }
        return true;
    }

    const std::vector<Tok>& toks() const {
        return toks_;
    }

   private:
    void fail(const std::string& msg) {
        if (error_ && error_->empty())
            *error_ = "liberty:" + std::to_string(line_) + ": " + msg;
    }

    const std::string& src_;
    std::string* error_;
    std::vector<Tok> toks_;
    size_t line_ = 1;
};

// ---- Parsed shapes ---------------------------------------------------------

struct PinInfo {
    std::string name;
    std::string direction;
    std::string function;
    bool clock = false;
};

struct FfInfo {
    bool present = false;
    std::string clocked_on;
    std::string next_state;
    std::string clear;
    std::string preset;
};

struct LatchInfo {
    bool present = false;
    std::string enable;
    std::string data_in;
    std::string data_out;
};

struct CellInfo {
    std::string name;
    std::vector<PinInfo> pins;
    FfInfo ff;
    LatchInfo latch;
    bool is_isolation_cell = false;  // power-aware library annotation
};

// Expression helpers ------------------------------------------------------------

// First referenced pin identifier in a simple boolean expression, skipping
// logic keywords and the flop state symbols. "" when the expression only
// references constants/state (e.g. next_state : "1").
std::string expr_pin(const std::string& expr) {
    size_t i = 0;
    while (i < expr.size()) {
        if (!std::isalpha(static_cast<unsigned char>(expr[i])) && expr[i] != '_') {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < expr.size() &&
               (std::isalnum(static_cast<unsigned char>(expr[i])) || expr[i] == '_'))
            ++i;
        std::string id = expr.substr(start, i - start);
        if (id == "and" || id == "or" || id == "not" || id == "xor" || id == "xnor" ||
            id == "buf" || id == "true" || id == "false" || id == "IQ" || id == "IQN")
            continue;
        return id;
    }
    return "";
}

bool expr_negated(const std::string& expr) {
    for (char c : expr) {
        if (c == ' ' || c == '\t')
            continue;
        return c == '!' || c == '~';
    }
    return false;
}

// Logic type hint (as ir::LogicType integer value, -1 = unknown) derived from
// the operator characters of a function expression. A single-kind operator
// set maps cleanly; mixed operators are complex expressions and stay unknown.
int expr_logic_hint(const std::string& expr) {
    bool has_and = false, has_or = false, has_xor = false, has_not = false, has_mux = false;
    bool has_ident = false;
    for (char c : expr) {
        switch (c) {
            case '&':
                has_and = true;
                break;
            case '|':
                has_or = true;
                break;
            case '^':
                has_xor = true;
                break;
            case '!':
            case '~':
                has_not = true;
                break;
            case '?':
                has_mux = true;
                break;
            default:
                if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
                    has_ident = true;
                break;
        }
    }
    int kAnd = 1, kOr = 2, kXor = 3, kNot = 4, kBuf = 5, kMux = 6;  // ir::LogicType
    if (has_mux)
        return kMux;
    if (!has_ident)
        return -1;  // constant expression ("0"/"1"): nothing to classify
    int kinds = (has_and ? 1 : 0) + (has_or ? 1 : 0) + (has_xor ? 1 : 0) + (has_not ? 1 : 0);
    if (kinds > 1)
        return -1;  // complex mixed expression
    if (has_xor)
        return kXor;  // xnor (extra '!') degrades to Xor, matching the adapter
    if (has_and)
        return kAnd;
    if (has_or)
        return kOr;
    if (has_not)
        return kNot;
    return kBuf;  // plain identifier: pass-through buffer
}

// ---- Parser ------------------------------------------------------------------

class Parser {
   public:
    Parser(std::vector<Tok> toks, std::string* error) : toks_(std::move(toks)), error_(error) {}

    bool parse(CellLibrary& out) {
        size_t i = 0;
        while (i < toks_.size()) {
            const Tok& t = toks_[i];
            if (t.kind == Tok::K::Word && (t.text == "library" || t.text == "cell") &&
                i + 2 < toks_.size() && toks_[i + 1].text == "(") {
                bool is_cell = (t.text == "cell");
                std::string name;
                if (!parse_args_name(i, name))
                    return false;
                if (i >= toks_.size() || toks_[i].text != "{") {
                    fail(t, "expected '{' after group name");
                    return false;
                }
                ++i;
                if (is_cell) {
                    CellInfo cell;
                    cell.name = name;
                    if (!parse_cell_body(i, cell))
                        return false;
                    out.add(make_def(cell));
                } else {
                    if (!parse_library_body(i, out))
                        return false;
                }
                continue;
            }
            ++i;  // skip unknown top-level items (vendor extensions)
        }
        return true;
    }

   private:
    // ( name ) where name may be a bareword, string, or empty (e.g. ff()).
    bool parse_args_name(size_t& i, std::string& name) {
        i += 2;  // skip group word + '('
        name.clear();
        if (i < toks_.size() && toks_[i].text != ")") {
            name = toks_[i].text;
            ++i;
        }
        while (i < toks_.size() && toks_[i].text != ")")  // skip extra args
            ++i;
        if (i >= toks_.size()) {
            if (error_ && error_->empty())
                *error_ = "liberty: unexpected end of file in group arguments";
            return false;
        }
        ++i;  // consume ')'
        return true;
    }

    bool parse_library_body(size_t& i, CellLibrary& out) {
        while (i < toks_.size() && toks_[i].text != "}") {
            const Tok& t = toks_[i];
            if (t.kind == Tok::K::Word && t.text == "cell" && i + 2 < toks_.size() &&
                toks_[i + 1].text == "(") {
                CellInfo cell;
                if (!parse_args_name(i, cell.name))
                    return false;
                if (i >= toks_.size() || toks_[i].text != "{") {
                    fail(t, "expected '{' after cell name");
                    return false;
                }
                ++i;
                if (!parse_cell_body(i, cell))
                    return false;
                out.add(make_def(cell));
                continue;
            }
            if (!skip_item(i))
                return false;
        }
        return consume_close(i);
    }

    // Inside a cell body: pin/ff/latch groups are captured; timing and other
    // groups, attributes, and stray tokens are skipped defensively.
    bool parse_cell_body(size_t& i, CellInfo& cell) {
        while (i < toks_.size() && toks_[i].text != "}") {
            const Tok& t = toks_[i];
            if (t.kind == Tok::K::Word && i + 2 < toks_.size() && toks_[i + 1].text == "(") {
                const std::string& gw = t.text;
                if (gw == "pin") {
                    PinInfo pin;
                    if (!parse_args_name(i, pin.name))
                        return false;
                    if (i >= toks_.size() || toks_[i].text != "{") {
                        fail(t, "expected '{' after pin name");
                        return false;
                    }
                    ++i;
                    if (!parse_pin_body(i, pin))
                        return false;
                    cell.pins.push_back(std::move(pin));
                    continue;
                }
                if (gw == "ff" || gw == "latch") {
                    std::string dummy;
                    if (!parse_args_name(i, dummy))
                        return false;
                    if (i >= toks_.size() || toks_[i].text != "{") {
                        fail(t, "expected '{' after ff/latch arguments");
                        return false;
                    }
                    ++i;
                    if (gw == "ff") {
                        cell.ff.present = true;
                        if (!parse_kv_body(i, cell.ff.clocked_on, cell.ff.next_state, cell.ff.clear,
                                           &cell.ff.preset))
                            return false;
                    } else {
                        cell.latch.present = true;
                        if (!parse_kv_body(i, cell.latch.enable, cell.latch.data_in,
                                           cell.latch.data_out, /*extra=*/nullptr))
                            return false;
                    }
                    continue;
                }
                if (!skip_group(i))  // timing() { ... }, leakage_power() { ... }
                    return false;
                continue;
            }
            if (t.kind == Tok::K::Word && i + 2 < toks_.size() && toks_[i + 1].text == ":") {
                // Cell-level `key : value;`. Only low-power annotations are
                // consumed; other attributes are skipped like skip_item would.
                const std::string key = t.text;
                const std::string value = toks_[i + 2].text;
                i += 3;
                if (i < toks_.size() && toks_[i].text == ";")
                    ++i;
                if (key == "is_isolation_cell")
                    cell.is_isolation_cell = (value == "true" || value == "1");
                continue;
            }
            if (!skip_item(i))
                return false;
        }
        return consume_close(i);
    }

    bool parse_pin_body(size_t& i, PinInfo& pin) {
        while (i < toks_.size() && toks_[i].text != "}") {
            const Tok& t = toks_[i];
            if (t.kind == Tok::K::Word && i + 2 < toks_.size() && toks_[i + 1].text == ":") {
                std::string key = t.text;
                std::string value = toks_[i + 2].text;
                i += 3;
                if (i < toks_.size() && toks_[i].text == ";")
                    ++i;
                if (key == "direction")
                    pin.direction = value;
                else if (key == "clock")
                    pin.clock = (value == "true" || value == "1");
                else if (key == "function")
                    pin.function = value;
                continue;
            }
            if (!skip_item(i))
                return false;
        }
        return consume_close(i);
    }

    // ff/latch body: capture `key : value;` attributes (both string and
    // bareword values); ignore everything else.
    bool parse_kv_body(size_t& i, std::string& a, std::string& b, std::string& c, std::string* d) {
        while (i < toks_.size() && toks_[i].text != "}") {
            const Tok& t = toks_[i];
            if (t.kind == Tok::K::Word && i + 2 < toks_.size() && toks_[i + 1].text == ":") {
                std::string key = t.text;
                std::string value = toks_[i + 2].text;
                i += 3;
                if (i < toks_.size() && toks_[i].text == ";")
                    ++i;
                // Positional capture by caller convention: ff passes
                // (clocked_on, next_state, clear[, preset]); latch passes
                // (enable, data_in, data_out).
                if (key == "clocked_on" || key == "enable")
                    a = value;
                else if (key == "next_state" || key == "data_in")
                    b = value;
                else if (key == "clear" || key == "data_out")
                    c = value;
                else if (d && key == "preset")
                    *d = value;
                continue;
            }
            if (!skip_item(i))
                return false;
        }
        return consume_close(i);
    }

    // Skip one item: a `key : value;` attribute, a nested group with or
    // without arguments, a bare `;`, or a single stray token.
    bool skip_item(size_t& i) {
        const Tok& t = toks_[i];
        if (t.kind == Tok::K::Word && i + 2 < toks_.size() && toks_[i + 1].text == ":") {
            i += 3;  // key : value
            if (i < toks_.size() && toks_[i].text == ";")
                ++i;
            return true;
        }
        if (t.kind == Tok::K::Word && i + 1 < toks_.size() && toks_[i + 1].text == "(")
            return skip_group(i);
        if (t.text == "{")
            return skip_group(i);
        if (t.text == ";") {
            ++i;
            return true;
        }
        ++i;
        return true;
    }

    // Skip `name ( args ) { ... }` or `{ ... }` including the balanced body.
    bool skip_group(size_t& i) {
        while (i < toks_.size() && toks_[i].text != "{") {
            if (toks_[i].text == ";" || toks_[i].text == "}") {  // arg-less form
                if (toks_[i].text == ";")
                    ++i;
                return true;
            }
            ++i;
        }
        return skip_balanced(i);
    }

    bool skip_balanced(size_t& i) {
        if (i >= toks_.size() || toks_[i].text != "{") {
            if (error_ && error_->empty())
                *error_ = "liberty: expected '{'";
            return false;
        }
        int depth = 0;
        while (i < toks_.size()) {
            if (toks_[i].text == "{")
                ++depth;
            else if (toks_[i].text == "}") {
                --depth;
                ++i;
                if (depth == 0)
                    return true;
                continue;
            }
            ++i;
        }
        if (error_ && error_->empty())
            *error_ = "liberty: unbalanced braces";
        return false;
    }

    bool consume_close(size_t& i) {
        if (i >= toks_.size() || toks_[i].text != "}") {
            if (error_ && error_->empty())
                *error_ = "liberty: missing '}'";
            return false;
        }
        ++i;
        return true;
    }

    void fail(const Tok& t, const std::string& msg) {
        if (error_ && error_->empty())
            *error_ = "liberty:" + std::to_string(t.line) + ": " + msg;
    }

    // Name heuristics mirroring the CellLibrary::with_defaults() low-power
    // globs (glob_match takes the pattern first).
    static bool retention_name(const std::string& name) {
        return glob_match("*RSDFF*", name) || glob_match("*RETENTION*", name);
    }

    // CellInfo -> CellDef (see liberty.h for the mapping rules).
    static CellDef make_def(const CellInfo& cell) {
        CellDef def;
        def.pattern = cell.name;  // exact name: longest-match beats defaults

        const PinInfo* out_pin = nullptr;
        for (const auto& p : cell.pins) {
            if (p.direction == "output") {
                out_pin = &p;
                break;
            }
        }

        // Explicit power annotation (power-aware libraries) wins over the
        // functional classification below.
        if (cell.is_isolation_cell) {
            def.kind = CellKind::Isolation;
            if (out_pin)
                def.output_pin = out_pin->name;
            return def;
        }

        if (cell.ff.present) {
            def.clock_pin = expr_pin(cell.ff.clocked_on);
            def.data_pin = expr_pin(cell.ff.next_state);
            const std::string& rst_expr = !cell.ff.clear.empty() ? cell.ff.clear : cell.ff.preset;
            def.reset_pin = expr_pin(rst_expr);
            def.reset_active_low = expr_negated(rst_expr);
            def.reset_async = true;  // Liberty ff clear/preset are asynchronous
            if (retention_name(cell.name)) {
                // Retention (save/restore) flop: sequential like a plain Dff;
                // the adapter ignores scan/save ports (SI/SE/...). Reset
                // properties above still apply.
                def.kind = CellKind::Retention;
            } else if (!cell.ff.clear.empty() && !cell.ff.preset.empty()) {
                def.kind = CellKind::DffSetReset;
                def.set_pin = expr_pin(cell.ff.preset);
            } else if (!rst_expr.empty()) {
                def.kind = CellKind::DffReset;
            } else {
                def.kind = CellKind::Dff;
            }
            if (out_pin)
                def.output_pin = out_pin->name;
            return def;
        }

        if (cell.latch.present) {
            def.kind = CellKind::Latch;
            def.clock_pin = expr_pin(cell.latch.enable);
            def.data_pin = expr_pin(cell.latch.data_in);
            if (out_pin)
                def.output_pin = out_pin->name;
            return def;
        }

        def.kind = CellKind::Comb;
        if (out_pin) {
            def.output_pin = out_pin->name;
            def.logic_type_hint = expr_logic_hint(out_pin->function);
            if (def.logic_type_hint == 5)  // ir::LogicType::Buf
                def.kind = CellKind::Buffer;
            else if (def.logic_type_hint == 4)  // ir::LogicType::Not
                def.kind = CellKind::Inverter;
        }
        return def;
    }

    std::vector<Tok> toks_;
    std::string* error_;
};

}  // namespace

bool load_liberty_string(const std::string& content, CellLibrary& out, std::string* error) {
    if (error)
        error->clear();
    Tokenizer tz(content, error);
    if (!tz.run())
        return false;
    Parser parser(tz.toks(), error);
    if (!parser.parse(out))
        return false;
    return true;
}

bool load_liberty_file(const std::string& path, CellLibrary& out, std::string* error) {
    if (error)
        error->clear();
    std::ifstream in(path);
    if (!in) {
        if (error)
            *error = "cannot open liberty file: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return load_liberty_string(ss.str(), out, error);
}

}  // namespace opencdc::cell
