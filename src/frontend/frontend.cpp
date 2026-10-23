#include "frontend/frontend.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_set>

namespace opencdc::frontend {

bool parse_input_type(const std::string& value, InputType& out, std::string* error) {
    if (value == "auto") {
        out = InputType::Auto;
        return true;
    }
    if (value == "rtl") {
        out = InputType::Rtl;
        return true;
    }
    if (value == "netlist") {
        out = InputType::Netlist;
        return true;
    }
    if (error)
        *error = "unknown input type '" + value + "' (expected auto, rtl, or netlist)";
    return false;
}

const char* input_type_name(InputType type) {
    switch (type) {
        case InputType::Auto:
            return "auto";
        case InputType::Rtl:
            return "rtl";
        case InputType::Netlist:
            return "netlist";
    }
    return "rtl";
}

namespace {

constexpr size_t kDetectScanBytes = 64 * 1024;

// Behavioral constructs that can only appear in RTL, never in a gate-level
// netlist. Word-boundary matched against comment-stripped text.
const std::unordered_set<std::string>& rtl_keywords() {
    static const std::unordered_set<std::string> kWords = {
        "always",  "always_ff", "always_comb", "always_latch", "initial", "function",
        "task",    "property",  "sequence",    "interface",    "class",   "generate",
        "specify", "defparam",  "primitive",   "table"};
    return kWords;
}

// Structural markers required before we call a file "netlist". Guards against
// classifying an empty/garbled file as a netlist.
const std::unordered_set<std::string>& netlist_markers() {
    static const std::unordered_set<std::string> kWords = {"module", "input", "output",
                                                           "wire",   "assign"};
    return kWords;
}

// Strip //... and /*...*/ comments in place-ish (returns cleaned copy).
// Unterminated block comments are truncated at end-of-input.
std::string strip_comments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        char c = text[i];
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            while (i < n && text[i] != '\n')
                ++i;
        } else if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(text[i] == '*' && text[i + 1] == '/'))
                ++i;
            i = (i + 1 < n) ? i + 2 : n;
            out.push_back(' ');  // keep token separation
        } else if (c == '"') {
            // Preserve string literals verbatim so keywords inside strings
            // ("always" in a $display) don't influence detection.
            out.push_back(c);
            ++i;
            while (i < n && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < n)
                    ++i;
                out.push_back(text[i]);
                ++i;
            }
            if (i < n) {
                out.push_back('"');
                ++i;
            }
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// Word-boundary keyword search (case-sensitive, matching Verilog's
// case-sensitive keywords). "my_always_on" must not match "always".
bool contains_word(const std::string& text, size_t pos, const std::string& word) {
    while ((pos = text.find(word, pos)) != std::string::npos) {
        bool bad_left = pos > 0 && is_word_char(text[pos - 1]);
        size_t end = pos + word.size();
        bool bad_right = end < text.size() && is_word_char(text[end]);
        if (!bad_left && !bad_right)
            return true;
        pos = end;
    }
    return false;
}

InputType detect_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return InputType::Rtl;  // unreadable → safe historical default

    std::string buffer(kDetectScanBytes, '\0');
    file.read(&buffer[0], static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<size_t>(file.gcount()));
    if (buffer.empty())
        return InputType::Rtl;

    std::string text = strip_comments(buffer);

    for (const auto& w : rtl_keywords()) {
        if (contains_word(text, 0, w))
            return InputType::Rtl;
    }

    // No behavioral constructs: require at least one structural marker before
    // declaring a netlist (guards against truncated/garbage input).
    for (const auto& w : netlist_markers()) {
        if (contains_word(text, 0, w))
            return InputType::Netlist;
    }
    return InputType::Rtl;
}

}  // namespace

InputType detect_input_type(const std::vector<std::string>& files) {
    // Conservative policy: ANY file that looks like RTL → analyze as RTL
    // (historical behavior; slang produces clear errors on real netlists).
    // Netlist only when every readable file is unambiguously structural.
    bool saw_readable = false;
    for (const auto& path : files) {
        std::ifstream probe(path, std::ios::binary);
        if (!probe.is_open())
            continue;
        saw_readable = true;
        if (detect_file(path) == InputType::Rtl)
            return InputType::Rtl;
    }
    return saw_readable ? InputType::Netlist : InputType::Rtl;
}

// Temporary definition for the frontend-extraction phase; the netlist adapter
// (PR: frontend/netlist/netlist_adapter.cpp) provides the real implementation
// and this stub is removed in the same change.
std::unique_ptr<IFrontend> make_netlist_adapter() {
    return nullptr;
}

}  // namespace opencdc::frontend
