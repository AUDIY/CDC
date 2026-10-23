#ifndef OPENCDC_CELL_CELL_LIBRARY_H
#define OPENCDC_CELL_CELL_LIBRARY_H

#include <string>
#include <vector>

namespace opencdc::cell {

// Functional classification of a standard-cell / macro instance. Drives how
// the netlist frontend maps an instance onto the IR graph.
enum class CellKind {
    Dff,        // plain flop: Q = D @ clk
    DffReset,   // flop with sync/async reset (see reset_async/active_low)
    DffEnable,  // flop with enable
    DffSetReset,  // flop with both set and reset
    Latch,      // level-sensitive latch
    Icg,        // integrated clock gate (latch + AND)
    Buffer,     // non-inverting buffer
    Inverter,   // inverter
    Comb,       // generic combinational cell (and/or/xor/mux/...)
    Unknown     // unmapped: treated as opaque combinational, never "safe"
};

// Cell classification pattern. `pattern` is a glob matched against the cell
// definition name from the netlist (e.g. "DFCNQ_*", "CLKGATE*"). Longest
// pattern wins so users can shadow broad defaults with specific entries.
struct CellDef {
    std::string pattern;
    CellKind kind = CellKind::Unknown;
    // Pin roles. Empty means "infer from common names at bind time" (the
    // netlist adapter guesses CK/CLK/C, D/DI, Q, etc.). Explicit names win.
    std::string clock_pin;
    std::string data_pin;
    std::string output_pin;
    std::string enable_pin;
    std::string reset_pin;
    std::string set_pin;
    // Reset electrical properties (only meaningful for DffReset/DffSetReset).
    bool reset_active_low = false;
    bool reset_async = false;
};

// Glob matcher supporting '*' (any run) and '?' (single char). Case-sensitive,
// matching Verilog identifiers. Exposed for unit tests.
bool glob_match(const std::string& pattern, const std::string& value);

// Human-readable CellKind name ("dff", "dff_reset", "icg", ...).
const char* cell_kind_name(CellKind kind);

// Parse a user-facing kind string ("dff"/"dff_reset"/... ). Returns false +
// error on unknown kind.
bool parse_cell_kind(const std::string& value, CellKind& out, std::string* error = nullptr);

class CellLibrary {
   public:
    // Heuristic generic defaults (DFF*/DLATCH*/CLKGATE*/...). Overridable by
    // adding user entries, which take precedence via longest-pattern match.
    static CellLibrary with_defaults();

    // Load a YAML cell map (the `cells:` list format documented in
    // docs/netlist.md). Returns false and fills `error` on I/O or parse error;
    // `config` receives the parsed cells on success.
    static bool load_from_yaml_file(const std::string& path, CellLibrary& out,
                                    std::string* error = nullptr);
    static bool load_from_yaml_string(const std::string& content, CellLibrary& out,
                                      std::string* error = nullptr);

    void add(CellDef def);

    // Longest-matching glob wins. Returns nullptr when no pattern matches (the
    // caller treats the instance as CellKind::Unknown).
    const CellDef* lookup(const std::string& cell_type) const;

    // Convenience: classification for a cell type, defaulting to Unknown.
    CellKind classify(const std::string& cell_type) const;

    size_t size() const {
        return defs_.size();
    }
    const std::vector<CellDef>& defs() const {
        return defs_;
    }

   private:
    std::vector<CellDef> defs_;
};

}  // namespace opencdc::cell

#endif  // OPENCDC_CELL_CELL_LIBRARY_H
