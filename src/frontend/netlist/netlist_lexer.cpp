#include "frontend/netlist/netlist_lexer.h"

#include <cctype>

namespace opencdc::frontend::netlist {

namespace {

bool is_word_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

}  // namespace

Lexer::Lexer(const std::string& source, std::string filename)
    : src_(source), filename_(std::move(filename)) {}

void Lexer::error(size_t line, size_t col, const std::string& msg) {
    errors_.push_back(filename_ + ":" + std::to_string(line) + ":" + std::to_string(col) + ": " +
                      msg);
}

char Lexer::peek(size_t ahead) const {
    size_t i = pos_ + ahead;
    return i < src_.size() ? src_[i] : '\0';
}

bool Lexer::at_end() const {
    return pos_ >= src_.size();
}

void Lexer::advance() {
    if (at_end())
        return;
    if (src_[pos_] == '\n') {
        ++line_;
        col_ = 1;
    } else {
        ++col_;
    }
    ++pos_;
}

void Lexer::skip_trivia() {
    while (!at_end()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            while (!at_end() && peek() != '\n')
                advance();
        } else if (c == '/' && peek(1) == '*') {
            size_t sl = line_, sc = col_;
            advance();
            advance();
            while (!at_end() && !(peek() == '*' && peek(1) == '/'))
                advance();
            if (at_end()) {
                error(sl, sc, "unterminated block comment");
                return;
            }
            advance();
            advance();
        } else if (c == '(' && peek(1) == '*') {
            // (* attribute *) — skipped entirely.
            size_t sl = line_, sc = col_;
            advance();
            advance();
            while (!at_end() && !(peek() == '*' && peek(1) == ')'))
                advance();
            if (at_end()) {
                error(sl, sc, "unterminated attribute (* ... *)");
                return;
            }
            advance();
            advance();
        } else {
            return;
        }
    }
}

Token Lexer::lex_identifier_or_keyword() {
    Token t;
    t.kind = TokenKind::Identifier;
    t.line = line_;
    t.col = col_;
    size_t start = pos_;
    while (!at_end() && is_word_char(peek()))
        advance();
    t.text = src_.substr(start, pos_ - start);
    return t;
}

Token Lexer::lex_escaped_identifier() {
    Token t;
    t.kind = TokenKind::EscapedIdentifier;
    t.line = line_;
    t.col = col_;
    advance();  // consume leading backslash
    size_t start = pos_;
    // Per Verilog LRM, an escaped identifier runs to the next whitespace and
    // may contain any printable character (dots, brackets, etc.).
    while (!at_end() && peek() != ' ' && peek() != '\t' && peek() != '\n' && peek() != '\r') {
        advance();
    }
    t.text = src_.substr(start, pos_ - start);
    return t;
}

Token Lexer::lex_number() {
    Token t;
    t.kind = TokenKind::Number;
    t.line = line_;
    t.col = col_;
    size_t start = pos_;
    // [size]'[s]base digit...  e.g. 1'bz, 4'hF, 32'sd5. Also plain integers.
    while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '\'' ||
                         peek() == '_')) {
        advance();
    }
    t.text = src_.substr(start, pos_ - start);
    return t;
}

Token Lexer::lex_string() {
    Token t;
    t.kind = TokenKind::StringLit;
    t.line = line_;
    t.col = col_;
    advance();  // opening quote
    std::string value;
    while (!at_end() && peek() != '"') {
        if (peek() == '\\' && pos_ + 1 < src_.size()) {
            advance();
        }
        value.push_back(peek());
        advance();
    }
    if (at_end()) {
        error(t.line, t.col, "unterminated string literal");
        t.kind = TokenKind::EndOfFile;
        return t;
    }
    advance();  // closing quote
    t.text = value;
    return t;
}

Token Lexer::lex_symbol() {
    Token t;
    t.kind = TokenKind::Symbol;
    t.line = line_;
    t.col = col_;
    t.text = std::string(1, peek());
    advance();
    return t;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        skip_trivia();
        if (has_errors())
            break;
        if (at_end()) {
            Token eof;
            eof.kind = TokenKind::EndOfFile;
            eof.line = line_;
            eof.col = col_;
            tokens.push_back(std::move(eof));
            break;
        }
        char c = peek();
        if (is_word_start(c)) {
            tokens.push_back(lex_identifier_or_keyword());
        } else if (c == '\\') {
            tokens.push_back(lex_escaped_identifier());
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            tokens.push_back(lex_number());
        } else if (c == '"') {
            tokens.push_back(lex_string());
        } else {
            tokens.push_back(lex_symbol());
        }
    }
    return tokens;
}

}  // namespace opencdc::frontend::netlist
