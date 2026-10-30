#include "report/report.h"

#include <algorithm>
#include <string>
#include <vector>

#include "clock/relationship.h"

namespace opencdc::report {

namespace {

// ANSI styles for the text report. Emitted only when Reporter::color() is on.
constexpr const char* kReset = "\033[0m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kDim = "\033[2m";
constexpr const char* kRed = "\033[1;31m";
constexpr const char* kYellow = "\033[1;33m";
constexpr const char* kBlue = "\033[1;34m";
constexpr const char* kGreen = "\033[1;32m";
constexpr const char* kCyan = "\033[1;36m";

int severity_rank(const std::string& sev) {
    if (sev == "error")
        return 0;
    if (sev == "warning")
        return 1;
    if (sev == "info")
        return 2;
    return 3;
}

const char* severity_color(const std::string& sev) {
    if (sev == "error")
        return kRed;
    if (sev == "warning")
        return kYellow;
    if (sev == "info")
        return kBlue;
    return "";
}

const char* severity_label(const std::string& sev) {
    if (sev == "error")
        return "ERROR";
    if (sev == "warning")
        return "WARNING";
    if (sev == "info")
        return "INFO";
    return sev.c_str();
}

// "── Summary ─────────..." — fixed 60-column section rule.
void write_section(std::ostream& os, const std::string& title, bool color) {
    if (color)
        os << kCyan;
    os << "── " << title << " ";
    const size_t used = 3 + title.size() + 1;
    for (size_t i = used; i < 60; ++i)
        os << "─";
    if (color)
        os << kReset;
    os << "\n";
}

// "2 — 1 error, 1 warning" (omits zero buckets).
std::string counts_phrase(const ReportCounts& c) {
    std::string s = std::to_string(c.total);
    std::vector<std::string> parts;
    auto add = [&](size_t n, const char* one, const char* many) {
        if (n > 0)
            parts.push_back(std::to_string(n) + " " + (n == 1 ? one : many));
    };
    add(c.errors, "error", "errors");
    add(c.warnings, "warning", "warnings");
    add(c.waived, "waived", "waived");
    add(c.suppressed, "suppressed", "suppressed");
    add(c.multicycle_suppressed, "mc-suppressed", "mc-suppressed");
    if (parts.empty())
        return s;
    s += " — ";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            s += ", ";
        s += parts[i];
    }
    return s;
}

}  // namespace

std::string Reporter::escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

static const char* multi_bit_type_name(cdc::MultiBitCrossingType t) {
    switch (t) {
        case cdc::MultiBitCrossingType::Raw:
            return "raw";
        case cdc::MultiBitCrossingType::Synchronized:
            return "synchronized";
        case cdc::MultiBitCrossingType::StaticData:
            return "static_data";
        case cdc::MultiBitCrossingType::HandshakeControlled:
            return "handshake_controlled";
        case cdc::MultiBitCrossingType::GrayCoded:
            return "gray_coded";
        case cdc::MultiBitCrossingType::AsyncFifo:
            return "async_fifo";
        default:
            return nullptr;
    }
}

ReportCounts Reporter::count(const std::vector<cdc::Finding>& findings) const {
    ReportCounts c;
    c.total = findings.size();
    for (const auto& f : findings) {
        if (f.waived) {
            c.waived++;
            continue;
        }
        if (f.suppressed_by_false_path) {
            c.suppressed++;
            continue;
        }
        if (f.suppressed_by_multicycle) {
            c.multicycle_suppressed++;
            continue;
        }
        if (f.severity == "error")
            c.errors++;
        else if (f.severity == "warning")
            c.warnings++;
    }
    return c;
}

bool Reporter::has_unsuppressed_errors(const std::vector<cdc::Finding>& findings) const {
    for (const auto& f : findings) {
        if (f.waived || f.suppressed_by_false_path || f.suppressed_by_multicycle)
            continue;
        if (f.severity == "error")
            return true;
    }
    return false;
}

void Reporter::report_json(const std::vector<cdc::Finding>& findings, std::ostream& os,
                           const std::string& analysis_status,
                           const std::string& input_type) const {
    auto sorted = sorted_findings(findings);
    os << "{\n"
       << "  \"analysis_status\": \"" << escape_json(analysis_status) << "\",\n";
    // Emitted only for netlist input so RTL reports stay byte-identical.
    if (input_type == "netlist")
        os << "  \"input_type\": \"netlist\",\n";
    os << "  \"finding_count\": " << sorted.size() << ",\n"
       << "  \"findings\": [\n";
    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto& f = sorted[i];
        os << "    {\n"
           << "      \"rule_id\": \"" << escape_json(f.rule_id) << "\",\n"
           << "      \"rule_name\": \"" << escape_json(f.rule_name) << "\",\n"
           << "      \"severity\": \"" << escape_json(f.severity) << "\",\n"
           << "      \"waived\": " << (f.waived ? "true" : "false") << ",\n";

        if (f.waived) {
            os << "      \"waiver_justification\": \"" << escape_json(f.waiver_justification)
               << "\",\n"
               << "      \"waiver_owner\": \"" << escape_json(f.waiver_owner) << "\",\n";
            if (!f.waiver_ticket.empty()) {
                os << "      \"waiver_ticket\": \"" << escape_json(f.waiver_ticket) << "\",\n";
            }
        }

        os << "      \"source\": \"" << escape_json(f.source_reg_name) << "\",\n"
           << "      \"source_domain\": \"" << escape_json(f.source_domain) << "\",\n"
           << "      \"dest\": \"" << escape_json(f.dest_reg_name) << "\",\n"
           << "      \"dest_domain\": \"" << escape_json(f.dest_domain) << "\",\n"
           << "      \"bus_width\": " << f.bus_width << ",\n"
           << "      \"reason\": \"" << escape_json(f.reason) << "\",\n"
           << "      \"is_gray_coded\": " << (f.is_gray_coded ? "true" : "false") << ",\n"
           << "      \"has_handshake\": " << (f.has_handshake ? "true" : "false") << ",\n"
           << "      \"source_module_path\": \"" << escape_json(f.source_module_path) << "\",\n"
           << "      \"dest_module_path\": \"" << escape_json(f.dest_module_path) << "\",\n"
           << "      \"crosses_module_boundary\": "
           << (f.crosses_module_boundary ? "true" : "false") << ",\n"
           << "      \"clock_relationship\": \""
           << escape_json(clock::clock_relationship_name(f.clock_relationship)) << "\",\n";

        if (f.safety_status != cdc::SafetyStatus::Unknown) {
            const char* status_str = "unknown";
            switch (f.safety_status) {
                case cdc::SafetyStatus::Candidate:
                    status_str = "candidate";
                    break;
                case cdc::SafetyStatus::VerifiedSafe:
                    status_str = "verified_safe";
                    break;
                case cdc::SafetyStatus::VerifiedUnsafe:
                    status_str = "verified_unsafe";
                    break;
                case cdc::SafetyStatus::Ambiguous:
                    status_str = "ambiguous";
                    break;
                default:
                    break;
            }
            os << "      \"safety_status\": \"" << status_str << "\",\n";
            if (!f.safety_provenance.empty()) {
                os << "      \"safety_provenance\": \"" << escape_json(f.safety_provenance)
                   << "\",\n";
            }
        }

        if (f.has_multicycle_exception) {
            os << "      \"multicycle_cycles\": " << f.multicycle_cycles << ",\n"
               << "      \"constraint_source\": \"" << escape_json(f.constraint_source) << "\",\n";
        }

        if (f.suppressed_by_false_path) {
            os << "      \"suppressed_by_false_path\": true,\n"
               << "      \"false_path_source\": \"" << escape_json(f.false_path_source) << "\",\n";
        }

        if (f.suppressed_by_multicycle) {
            os << "      \"suppressed_by_multicycle\": true,\n"
               << "      \"multicycle_source\": \"" << escape_json(f.multicycle_source) << "\",\n";
        }

        if (f.multi_bit_type != cdc::MultiBitCrossingType::None) {
            const char* mbt = "unknown";
            switch (f.multi_bit_type) {
                case cdc::MultiBitCrossingType::Raw:
                    mbt = "raw";
                    break;
                case cdc::MultiBitCrossingType::Synchronized:
                    mbt = "synchronized";
                    break;
                case cdc::MultiBitCrossingType::StaticData:
                    mbt = "static_data";
                    break;
                case cdc::MultiBitCrossingType::HandshakeControlled:
                    mbt = "handshake_controlled";
                    break;
                case cdc::MultiBitCrossingType::GrayCoded:
                    mbt = "gray_coded";
                    break;
                case cdc::MultiBitCrossingType::AsyncFifo:
                    mbt = "async_fifo";
                    break;
                default:
                    break;
            }
            os << "      \"multi_bit_type\": \"" << mbt << "\",\n";
        }

        if (!f.evidence_chain.empty()) {
            os << "      \"evidence_chain\": [\n";
            for (size_t j = 0; j < f.evidence_chain.size(); ++j) {
                const auto& step = f.evidence_chain[j];
                os << "        {\n"
                   << "          \"step_type\": \"" << escape_json(step.step_type) << "\",\n"
                   << "          \"description\": \"" << escape_json(step.description) << "\",\n"
                   << "          \"result\": \"" << escape_json(step.result) << "\"\n"
                   << "        }";
                if (j + 1 < f.evidence_chain.size())
                    os << ",";
                os << "\n";
            }
            os << "      ],\n";
        }

        os << "      \"file\": \"" << escape_json(f.source_loc.file) << "\",\n"
           << "      \"line\": " << f.source_loc.line << "\n"
           << "    }";

        if (i + 1 < sorted.size())
            os << ",";
        os << "\n";
    }
    os << "  ]\n"
       << "}\n";
}

void Reporter::report_text(const std::vector<cdc::Finding>& findings, std::ostream& os,
                           const std::string& analysis_status,
                           const std::string& input_type) const {
    ReportContext ctx;
    ctx.analysis_status = analysis_status;
    ctx.input_type = input_type;
    report_text(findings, os, ctx);
}

void Reporter::report_text(const std::vector<cdc::Finding>& findings, std::ostream& os,
                           const ReportContext& ctx) const {
    auto sorted = sorted_findings(findings);
    // Group by severity (errors, then warnings, then info) while preserving
    // the file/line order inside each group.
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const cdc::Finding& a, const cdc::Finding& b) {
                         return severity_rank(a.severity) < severity_rank(b.severity);
                     });
    const bool color = color_;
    auto c = count(findings);

    // ── Header ────────────────────────────────────────────────────────────
    if (!ctx.version.empty() || !ctx.top_module.empty()) {
        os << (color ? kCyan : "") << "OpenCDC";
        if (!ctx.version.empty())
            os << " v" << ctx.version;
        os << (color ? kReset : "") << " — CDC static analysis\n";
        bool first = true;
        os << "  ";
        if (!ctx.top_module.empty()) {
            os << "Top: " << ctx.top_module;
            first = false;
        }
        if (!ctx.input_type.empty()) {
            if (!first)
                os << " | ";
            os << "Input: " << ctx.input_type;
            first = false;
        }
        if (!ctx.input_files.empty()) {
            if (!first)
                os << " | ";
            os << "Files:";
            for (const auto& f : ctx.input_files)
                os << " " << f;
            first = false;
        }
        if (!first)
            os << "\n";
    }

    // ── Findings ──────────────────────────────────────────────────────────
    write_section(os, "Findings (" + counts_phrase(c) + ")", color);
    if (sorted.empty())
        os << "  No CDC findings.\n";

    size_t n = 0;
    for (const auto& f : sorted) {
        ++n;
        os << "  " << n << ". " << (color ? severity_color(f.severity) : "")
           << severity_label(f.severity) << (color ? kReset : "") << "  " << (color ? kBold : "")
           << f.rule_id << (color ? kReset : "");
        if (!f.rule_name.empty())
            os << "  " << f.rule_name;
        os << "\n";

        // clang-format off
        os << "       " << f.source_reg_name << " (" << f.source_domain << ")"
           << " -> " << f.dest_reg_name << " (" << f.dest_domain << ")\n";
        // clang-format on
        os << "       " << f.reason << "\n";

        if (!f.safety_provenance.empty()) {
            os << (color ? kDim : "") << "       provenance: " << f.safety_provenance
               << (color ? kReset : "") << "\n";
        }

        if (!f.source_loc.file.empty()) {
            os << (color ? kDim : "") << "       at " << f.source_loc.file << ":"
               << f.source_loc.line;
            if (f.source_loc.col > 0)
                os << ":" << f.source_loc.col;
            os << (color ? kReset : "") << "\n";
        }

        std::string tags;
        auto add_tag = [&tags](const std::string& t) {
            tags += tags.empty() ? "[" + t + "]" : " [" + t + "]";
        };
        if (f.waived)
            add_tag("WAIVED");
        if (f.suppressed_by_false_path)
            add_tag("FALSE_PATH");
        if (f.suppressed_by_multicycle)
            add_tag("MC:" + std::to_string(f.multicycle_cycles) + "x suppressed");
        else if (f.has_multicycle_exception)
            add_tag("MC:" + std::to_string(f.multicycle_cycles) + "x");
        if (f.clock_relationship != clock::ClockRelationship::Unknown)
            add_tag(std::string("clock:") + clock::clock_relationship_name(f.clock_relationship));
        if (f.multi_bit_type != cdc::MultiBitCrossingType::None) {
            const char* mbt_name = multi_bit_type_name(f.multi_bit_type);
            if (mbt_name)
                add_tag(mbt_name);
        }
        if (f.safety_status != cdc::SafetyStatus::Unknown) {
            const char* tag = "";
            switch (f.safety_status) {
                case cdc::SafetyStatus::Candidate:
                    tag = "CANDIDATE";
                    break;
                case cdc::SafetyStatus::VerifiedSafe:
                    tag = "SAFE";
                    break;
                case cdc::SafetyStatus::VerifiedUnsafe:
                    tag = "UNSAFE";
                    break;
                case cdc::SafetyStatus::Ambiguous:
                    tag = "AMBIGUOUS";
                    break;
                default:
                    break;
            }
            if (tag[0] != '\0')
                add_tag(tag);
        }
        if (!tags.empty()) {
            os << (color ? kDim : "") << "       " << tags << (color ? kReset : "") << "\n";
        }

        if (f.waived && !f.waiver_justification.empty()) {
            os << "       waiver: " << f.waiver_justification;
            if (!f.waiver_owner.empty())
                os << " (" << f.waiver_owner << ")";
            if (!f.waiver_ticket.empty())
                os << " [" << f.waiver_ticket << "]";
            os << "\n";
        }
    }

    if (ctx.analysis_status == "incomplete") {
        bool truncated = false;
        for (const auto& f : findings) {
            if (f.rule_id == "CDC010") {
                truncated = true;
                break;
            }
        }
        os << "\n"
           << (color ? kYellow : "")
           << "WARNING: Analysis incomplete — "
           << (truncated ? "some crossings may be missed due to path traversal truncation."
                         : "unknown sequential cells were not modeled; some crossings may be "
                           "missed.")
           << (color ? kReset : "") << "\n";
    }

    // ── Summary ───────────────────────────────────────────────────────────
    write_section(os, "Summary", color);
    os << "  status         " << ctx.analysis_status << "\n";
    os << "  findings       " << counts_phrase(c) << "\n";
    os << "  waived         " << c.waived << "\n";
    os << "  suppressed     " << c.suppressed << " (false_path)\n";
    os << "  mc_suppressed  " << c.multicycle_suppressed << "\n";

    // ── Signoff ───────────────────────────────────────────────────────────
    if (ctx.show_signoff) {
        write_section(os, "Signoff", color);
        const char* sc = "";
        switch (ctx.signoff.status) {
            case analysis::SignoffStatus::Pass:
            case analysis::SignoffStatus::PassWithWaivers:
                sc = kGreen;
                break;
            case analysis::SignoffStatus::Fail:
            case analysis::SignoffStatus::Error:
                sc = kRed;
                break;
            case analysis::SignoffStatus::Incomplete:
                sc = kYellow;
                break;
        }
        os << "  " << (color ? sc : "") << analysis::signoff_status_name(ctx.signoff.status)
           << (color ? kReset : "") << " — " << ctx.signoff.reason << "\n";
    }
}

void Reporter::report_summary(const std::vector<cdc::Finding>& findings, std::ostream& os,
                              const std::string& analysis_status) const {
    auto c = count(findings);
    os << "analysis_status=" << analysis_status << " ";
    if (c.total == 0) {
        os << "findings=0\n";
        return;
    }
    os << "findings=" << c.total;
    if (c.errors > 0)
        os << " errors=" << c.errors;
    if (c.warnings > 0)
        os << " warnings=" << c.warnings;
    if (c.waived > 0)
        os << " waived=" << c.waived;
    if (c.suppressed > 0)
        os << " suppressed=" << c.suppressed;
    if (c.multicycle_suppressed > 0)
        os << " mc_suppressed=" << c.multicycle_suppressed;
    os << "\n";
}

std::vector<cdc::Finding> Reporter::sorted_findings(const std::vector<cdc::Finding>& findings) {
    std::vector<cdc::Finding> sorted = findings;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const cdc::Finding& a, const cdc::Finding& b) {
                         if (a.source_loc.file != b.source_loc.file)
                             return a.source_loc.file < b.source_loc.file;
                         if (a.source_loc.line != b.source_loc.line)
                             return a.source_loc.line < b.source_loc.line;
                         if (a.rule_id != b.rule_id)
                             return a.rule_id < b.rule_id;
                         if (a.source_reg_name != b.source_reg_name)
                             return a.source_reg_name < b.source_reg_name;
                         return a.dest_reg_name < b.dest_reg_name;
                     });
    return sorted;
}

void Reporter::report_json(const std::vector<cdc::Finding>& findings,
                           const analysis::CoverageResult& coverage,
                           const analysis::SignoffResult& signoff, std::ostream& os,
                           const std::string& analysis_status,
                           const std::string& input_type) const {
    auto sorted = sorted_findings(findings);
    os << "{\n"
       << "  \"analysis_status\": \"" << escape_json(analysis_status) << "\",\n";
    // Emitted only for netlist input so RTL reports stay byte-identical.
    if (input_type == "netlist")
        os << "  \"input_type\": \"netlist\",\n";
    os << "  \"finding_count\": " << sorted.size() << ",\n";

    // Coverage
    os << "  \"coverage\": {\n"
       << "    \"total\": " << coverage.counts.total << ",\n"
       << "    \"errors\": " << coverage.counts.errors << ",\n"
       << "    \"warnings\": " << coverage.counts.warnings << ",\n"
       << "    \"verified_safe\": " << coverage.counts.verified_safe << ",\n"
       << "    \"verified_unsafe\": " << coverage.counts.verified_unsafe << ",\n"
       << "    \"waived\": " << coverage.counts.waived << ",\n"
       << "    \"suppressed\": " << coverage.counts.suppressed << "\n"
       << "  },\n";

    // Signoff
    os << "  \"signoff\": {\n"
       << "    \"status\": \"" << analysis::signoff_status_name(signoff.status) << "\",\n"
       << "    \"reason\": \"" << escape_json(signoff.reason) << "\"\n"
       << "  },\n";

    os << "  \"findings\": [\n";
    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto& f = sorted[i];
        os << "    {\n"
           << "      \"rule_id\": \"" << escape_json(f.rule_id) << "\",\n"
           << "      \"rule_name\": \"" << escape_json(f.rule_name) << "\",\n"
           << "      \"severity\": \"" << escape_json(f.severity) << "\",\n"
           << "      \"waived\": " << (f.waived ? "true" : "false") << ",\n";

        if (f.waived) {
            os << "      \"waiver_justification\": \"" << escape_json(f.waiver_justification)
               << "\",\n"
               << "      \"waiver_owner\": \"" << escape_json(f.waiver_owner) << "\",\n";
            if (!f.waiver_ticket.empty()) {
                os << "      \"waiver_ticket\": \"" << escape_json(f.waiver_ticket) << "\",\n";
            }
        }

        os << "      \"source\": \"" << escape_json(f.source_reg_name) << "\",\n"
           << "      \"source_domain\": \"" << escape_json(f.source_domain) << "\",\n"
           << "      \"dest\": \"" << escape_json(f.dest_reg_name) << "\",\n"
           << "      \"dest_domain\": \"" << escape_json(f.dest_domain) << "\",\n"
           << "      \"bus_width\": " << f.bus_width << ",\n"
           << "      \"reason\": \"" << escape_json(f.reason) << "\",\n"
           << "      \"is_gray_coded\": " << (f.is_gray_coded ? "true" : "false") << ",\n"
           << "      \"has_handshake\": " << (f.has_handshake ? "true" : "false") << ",\n"
           << "      \"source_module_path\": \"" << escape_json(f.source_module_path) << "\",\n"
           << "      \"dest_module_path\": \"" << escape_json(f.dest_module_path) << "\",\n"
           << "      \"crosses_module_boundary\": "
           << (f.crosses_module_boundary ? "true" : "false") << ",\n"
           << "      \"clock_relationship\": \""
           << escape_json(clock::clock_relationship_name(f.clock_relationship)) << "\",\n";

        if (f.safety_status != cdc::SafetyStatus::Unknown) {
            const char* status_str = "unknown";
            switch (f.safety_status) {
                case cdc::SafetyStatus::Candidate:
                    status_str = "candidate";
                    break;
                case cdc::SafetyStatus::VerifiedSafe:
                    status_str = "verified_safe";
                    break;
                case cdc::SafetyStatus::VerifiedUnsafe:
                    status_str = "verified_unsafe";
                    break;
                case cdc::SafetyStatus::Ambiguous:
                    status_str = "ambiguous";
                    break;
                default:
                    break;
            }
            os << "      \"safety_status\": \"" << status_str << "\",\n";
            if (!f.safety_provenance.empty()) {
                os << "      \"safety_provenance\": \"" << escape_json(f.safety_provenance)
                   << "\",\n";
            }
        }

        if (f.has_multicycle_exception) {
            os << "      \"multicycle_cycles\": " << f.multicycle_cycles << ",\n"
               << "      \"constraint_source\": \"" << escape_json(f.constraint_source) << "\",\n";
        }

        if (f.suppressed_by_false_path) {
            os << "      \"suppressed_by_false_path\": true,\n"
               << "      \"false_path_source\": \"" << escape_json(f.false_path_source) << "\",\n";
        }

        if (f.suppressed_by_multicycle) {
            os << "      \"suppressed_by_multicycle\": true,\n"
               << "      \"multicycle_source\": \"" << escape_json(f.multicycle_source) << "\",\n";
        }

        if (f.multi_bit_type != cdc::MultiBitCrossingType::None) {
            const char* mbt = "unknown";
            switch (f.multi_bit_type) {
                case cdc::MultiBitCrossingType::Raw:
                    mbt = "raw";
                    break;
                case cdc::MultiBitCrossingType::Synchronized:
                    mbt = "synchronized";
                    break;
                case cdc::MultiBitCrossingType::StaticData:
                    mbt = "static_data";
                    break;
                case cdc::MultiBitCrossingType::HandshakeControlled:
                    mbt = "handshake_controlled";
                    break;
                case cdc::MultiBitCrossingType::GrayCoded:
                    mbt = "gray_coded";
                    break;
                case cdc::MultiBitCrossingType::AsyncFifo:
                    mbt = "async_fifo";
                    break;
                default:
                    break;
            }
            os << "      \"multi_bit_type\": \"" << mbt << "\",\n";
        }

        if (!f.evidence_chain.empty()) {
            os << "      \"evidence_chain\": [\n";
            for (size_t j = 0; j < f.evidence_chain.size(); ++j) {
                const auto& step = f.evidence_chain[j];
                os << "        {\n"
                   << "          \"step_type\": \"" << escape_json(step.step_type) << "\",\n"
                   << "          \"description\": \"" << escape_json(step.description) << "\",\n"
                   << "          \"result\": \"" << escape_json(step.result) << "\"\n"
                   << "        }";
                if (j + 1 < f.evidence_chain.size())
                    os << ",";
                os << "\n";
            }
            os << "      ],\n";
        }

        os << "      \"file\": \"" << escape_json(f.source_loc.file) << "\",\n"
           << "      \"line\": " << f.source_loc.line << "\n"
           << "    }";

        if (i + 1 < sorted.size())
            os << ",";
        os << "\n";
    }
    os << "  ]\n"
       << "}\n";
}

}  // namespace opencdc::report
