#ifndef OPENCDC_FRONTEND_NETLIST_NETLIST_LEXER_H
#define OPENCDC_FRONTEND_NETLIST_NETLIST_LEXER_H

#include <cstddef>
#include <string>
#include <vector>

namespace opencdc::frontend::netlist {

enum class TokenKind {
    Identifier,         // plain identifier: module, wire, DFFCNQ_X1
    EscapedIdentifier,  // \foo.bar[3]  (text excludes the leading backslash)
    Number,             // 1'bz, 4'hF, 32'd5, 1'b0
    Symbol,             // single punctuation char: ( ) [ ] { } ; , . : # = * etc.
    StringLit,          // "text"
    EndOfFile
};

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string text;  // identifier body / number literal / symbol char / string content
    size_t line = 1;
    size_t col = 1;
};

// Streaming lexer for the Verilog-2001/SV gate-level netlist subset. Handles
// comments, (* attributes *) (skipped), escaped identifiers, sized literals,
// and string literals. Errors are collected, not thrown; a lexical error stops
// tokenization (callers check has_errors()).
class Lexer {
   public:
    Lexer(const std::string& source, std::string filename);

    // Tokenize the whole input into a vector terminated by an EndOfFile token.
    std::vector<Token> tokenize();

    bool has_errors() const {
        return !errors_.empty();
    }
    const std::vector<std::string>& errors() const {
        return errors_;
    }

   private:
    void error(size_t line, size_t col, const std::string& msg);

    // Cursor helpers.
    char peek(size_t ahead = 0) const;
    bool at_end() const;
    void advance();

    // Skips whitespace, comments, and (* ... *) attributes.
    void skip_trivia();

    Token lex_identifier_or_keyword();
    Token lex_escaped_identifier();
    Token lex_number();
    Token lex_string();
    Token lex_symbol();

    const std::string& src_;
    std::string filename_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t col_ = 1;
    std::vector<std::string> errors_;
};

}  // namespace opencdc::frontend::netlist

#endif  // OPENCDC_FRONTEND_NETLIST_NETLIST_LEXER_H
