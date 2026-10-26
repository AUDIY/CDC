#include "cell/cell_library.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace opencdc::cell {

bool glob_match(const std::string& pattern, const std::string& value) {
    // Iterative two-pointer glob with '*' backtracking. O(n*m) worst case,
    // linear on typical patterns. Handles "" and patterns without '*'.
    size_t p = 0, v = 0;
    size_t star_p = std::string::npos, star_v = 0;
    while (v < value.size()) {
        if (p < pattern.size() && (pattern[p] == value[v] || pattern[p] == '?')) {
            ++p;
            ++v;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_p = p++;
            star_v = v;
        } else if (star_p != std::string::npos) {
            // Backtrack: extend the last '*' by one character.
            p = star_p + 1;
            v = ++star_v;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

const char* cell_kind_name(CellKind kind) {
    switch (kind) {
        case CellKind::Dff:
            return "dff";
        case CellKind::DffReset:
            return "dff_reset";
        case CellKind::DffEnable:
            return "dff_enable";
        case CellKind::DffSetReset:
            return "dff_set_reset";
        case CellKind::Latch:
            return "latch";
        case CellKind::Icg:
            return "icg";
        case CellKind::Buffer:
            return "buffer";
        case CellKind::Inverter:
            return "inverter";
        case CellKind::Comb:
            return "comb";
        case CellKind::Isolation:
            return "isolation";
        case CellKind::Retention:
            return "retention";
        case CellKind::Unknown:
            return "unknown";
    }
    return "unknown";
}

bool parse_cell_kind(const std::string& value, CellKind& out, std::string* error) {
    if (value == "dff") {
        out = CellKind::Dff;
        return true;
    }
    if (value == "dff_reset") {
        out = CellKind::DffReset;
        return true;
    }
    if (value == "dff_enable") {
        out = CellKind::DffEnable;
        return true;
    }
    if (value == "dff_set_reset") {
        out = CellKind::DffSetReset;
        return true;
    }
    if (value == "latch") {
        out = CellKind::Latch;
        return true;
    }
    if (value == "icg") {
        out = CellKind::Icg;
        return true;
    }
    if (value == "buffer") {
        out = CellKind::Buffer;
        return true;
    }
    if (value == "inverter") {
        out = CellKind::Inverter;
        return true;
    }
    if (value == "comb") {
        out = CellKind::Comb;
        return true;
    }
    if (value == "isolation") {
        out = CellKind::Isolation;
        return true;
    }
    if (value == "retention") {
        out = CellKind::Retention;
        return true;
    }
    if (error)
        *error = "unknown cell kind '" + value +
                 "' (expected dff, dff_reset, dff_enable, dff_set_reset, latch, icg, buffer, "
                 "inverter, comb, isolation, retention)";
    return false;
}

namespace {
// Aggregate helper: keeps the default entries readable without partial
// brace-initializers (which warn with -Wmissing-field-initializers).
CellDef mk(const char* pattern, CellKind kind) {
    CellDef d;
    d.pattern = pattern;
    d.kind = kind;
    return d;
}
}  // namespace

CellLibrary CellLibrary::with_defaults() {
    CellLibrary lib;
    // Generic, vendor-agnostic heuristics. These are intentionally broad and
    // clearly overridable: a user cell map entry with a longer/more specific
    // pattern shadows these via longest-match. Pin roles are left empty so the
    // adapter infers them from common names (CK/CLK/C, D/DI, Q, ...).
    //
    // Order is irrelevant — lookup picks the longest matching pattern.
    lib.add(mk("DFF*", CellKind::Dff));
    lib.add(mk("DFXP*", CellKind::Dff));
    lib.add(mk("SDFF*", CellKind::Dff));
    lib.add(mk("DF*N*", CellKind::DffReset));  // DFCN*, DFCNQ*, DFDN* → reset
    lib.add(mk("DF*P*", CellKind::DffReset));  // preset variants
    lib.add(mk("DFF*EN*", CellKind::DffEnable));
    lib.add(mk("SDFF*EN*", CellKind::DffEnable));
    lib.add(mk("DLATCH*", CellKind::Latch));
    lib.add(mk("LAT*", CellKind::Latch));
    lib.add(mk("CLKGATE*", CellKind::Icg));
    lib.add(mk("ICG*", CellKind::Icg));
    lib.add(mk("CKGATE*", CellKind::Icg));
    lib.add(mk("BUFX*", CellKind::Buffer));
    lib.add(mk("BUF*", CellKind::Buffer));
    lib.add(mk("CLKBUF*", CellKind::Buffer));
    lib.add(mk("INVD*", CellKind::Inverter));
    lib.add(mk("INV*", CellKind::Inverter));
    lib.add(mk("NOT*", CellKind::Inverter));
    lib.add(mk("AND*", CellKind::Comb));
    lib.add(mk("NAND*", CellKind::Comb));
    lib.add(mk("OR*", CellKind::Comb));
    lib.add(mk("NOR*", CellKind::Comb));
    lib.add(mk("XOR*", CellKind::Comb));
    lib.add(mk("XNOR*", CellKind::Comb));
    lib.add(mk("MUX*", CellKind::Comb));
    lib.add(mk("AOI*", CellKind::Comb));
    lib.add(mk("OAI*", CellKind::Comb));

    // Low-power cells. Isolation clamps are transparent combinational
    // (Buffer-equivalent); retention (save/restore) flops are real sequential
    // elements (Dff-equivalent — scan/save ports are ignored). Recognizing
    // them keeps them out of Unknown, which would model retention flops as
    // opaque combinational and hide the crossings they participate in.
    // "ISO"/"RSDFF" substrings are distinctive enough not to collide with
    // functional cell names, and no yosys primitive matches them.
    lib.add(mk("*ISO*", CellKind::Isolation));
    lib.add(mk("*RSDFF*", CellKind::Retention));
    lib.add(mk("*RETENTION*", CellKind::Retention));

    // Yosys generic gate-level cells (output of `write_verilog` after
    // `synth`/`abc -g`). Recognized so post-yosys netlists classify without a
    // custom map. Reset variants carry the R pin and are marked DffReset. The
    // generic entry records the reset net but assumes active-high polarity; a
    // custom cell map with per-pattern reset_active_low refines polarity for
    // signoff (it does not affect crossing/domain detection).
    lib.add(mk("$_DFF_*", CellKind::Dff));
    lib.add(mk("$_SDFF_*", CellKind::Dff));
    lib.add(mk("$_DFFE_*", CellKind::DffEnable));
    lib.add(mk("$_DFF_P?0_", CellKind::DffReset));
    lib.add(mk("$_DFF_P?1_", CellKind::DffReset));
    lib.add(mk("$_DFF_N?0_", CellKind::DffReset));
    lib.add(mk("$_DFF_N?1_", CellKind::DffReset));
    lib.add(mk("$_DLATCH_*", CellKind::Latch));
    lib.add(mk("$_AND_*", CellKind::Comb));
    lib.add(mk("$_NAND_*", CellKind::Comb));
    lib.add(mk("$_OR_*", CellKind::Comb));
    lib.add(mk("$_NOR_*", CellKind::Comb));
    lib.add(mk("$_XOR_*", CellKind::Comb));
    lib.add(mk("$_XNOR_*", CellKind::Comb));
    lib.add(mk("$_MUX_*", CellKind::Comb));
    lib.add(mk("$_AOI*", CellKind::Comb));
    lib.add(mk("$_OAI*", CellKind::Comb));
    lib.add(mk("$_NOT_*", CellKind::Inverter));
    lib.add(mk("$_BUF_*", CellKind::Buffer));
    return lib;
}

void CellLibrary::add(CellDef def) {
    defs_.push_back(std::move(def));
}

const CellDef* CellLibrary::lookup(const std::string& cell_type) const {
    const CellDef* best = nullptr;
    size_t best_len = 0;
    for (const auto& def : defs_) {
        if (glob_match(def.pattern, cell_type)) {
            // Longest pattern wins so specific user entries shadow broad
            // defaults regardless of insertion order.
            if (def.pattern.size() > best_len) {
                best = &def;
                best_len = def.pattern.size();
            }
        }
    }
    return best;
}

CellKind CellLibrary::classify(const std::string& cell_type) const {
    const CellDef* def = lookup(cell_type);
    return def ? def->kind : CellKind::Unknown;
}

namespace {

bool parse_cells_node(const YAML::Node& root, CellLibrary& out, std::string* error) {
    const YAML::Node& cells = root["cells"];
    if (!cells || !cells.IsSequence()) {
        if (error)
            *error = "cell map: 'cells' must be a list";
        return false;
    }
    for (const auto& item : cells) {
        if (!item.IsMap()) {
            if (error)
                *error = "cell map: each cell entry must be a map";
            return false;
        }
        CellDef def;
        if (!item["pattern"]) {
            if (error)
                *error = "cell map: each cell entry requires 'pattern'";
            return false;
        }
        def.pattern = item["pattern"].as<std::string>();
        if (def.pattern.empty()) {
            if (error)
                *error = "cell map: 'pattern' must not be empty";
            return false;
        }
        if (!item["kind"]) {
            if (error)
                *error = "cell map: cell entry '" + def.pattern + "' requires 'kind'";
            return false;
        }
        std::string kind_str = item["kind"].as<std::string>();
        if (!parse_cell_kind(kind_str, def.kind, error))
            return false;
        if (item["clock"])
            def.clock_pin = item["clock"].as<std::string>();
        if (item["data"])
            def.data_pin = item["data"].as<std::string>();
        if (item["output"])
            def.output_pin = item["output"].as<std::string>();
        if (item["enable"])
            def.enable_pin = item["enable"].as<std::string>();
        if (item["reset"])
            def.reset_pin = item["reset"].as<std::string>();
        if (item["set"])
            def.set_pin = item["set"].as<std::string>();
        if (item["reset_active_low"])
            def.reset_active_low = item["reset_active_low"].as<bool>();
        if (item["reset_async"])
            def.reset_async = item["reset_async"].as<bool>();
        out.add(std::move(def));
    }
    return true;
}

}  // namespace

bool CellLibrary::load_from_yaml_string(const std::string& content, CellLibrary& out,
                                        std::string* error) {
    try {
        YAML::Node root = YAML::Load(content);
        if (!root || !root.IsMap()) {
            if (error)
                *error = "cell map: top level must be a map with a 'cells' list";
            return false;
        }
        return parse_cells_node(root, out, error);
    } catch (const YAML::Exception& e) {
        if (error)
            *error = std::string("cell map parse error: ") + e.what();
        return false;
    }
}

bool CellLibrary::load_from_yaml_file(const std::string& path, CellLibrary& out,
                                      std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        if (error)
            *error = "could not open cell map: " + path;
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return load_from_yaml_string(content, out, error);
}

}  // namespace opencdc::cell
