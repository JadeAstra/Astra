#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace astra {

enum class TokenType {
    // Punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Dot, Semicolon, Colon, Question,

    // Operators
    Plus, Minus, Star, Slash, Percent,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    Bang, BangEq, Eq, EqEq, Lt, LtEq, Gt, GtEq,
    AndAnd, OrOr, Arrow, DotDot, DotDotEq,

    // Literals
    Identifier, Int, Float, String, InterpString,

    // Keywords
    Val, Var, Fun, Class, Static, Operator, This, Super,
    Return, If, Else, While, For, In, Break, Continue,
    True, False, Null, Try, Catch, Throw, Import, Is,

    Eof,
};

struct InterpPart;

struct Token {
    TokenType type = TokenType::Eof;
    std::string text;
    int line = 0;
    int col = 0;

    // True when a statement-significant line break precedes this token.
    bool newlineBefore = false;

    int64_t intValue = 0;
    double floatValue = 0;
    std::string stringValue;

    // Only for InterpString: literal text alternating with embedded expressions.
    std::shared_ptr<std::vector<InterpPart>> parts;
};

struct InterpPart {
    bool isExpr = false;
    std::string text;           // when !isExpr
    std::vector<Token> tokens;  // when isExpr; ends with Eof
};

struct CompileError : std::runtime_error {
    std::string file;
    int line;
    int col;

    // The error was hit at end of input, so more input might fix it (used by the REPL).
    bool atEof;

    CompileError(std::string file, int line, int col, const std::string& message, bool atEof = false);
    std::string format() const;
};

class Lexer {
public:
    // `source` must outlive the lexer.
    Lexer(const std::string& source, std::string file);
    std::vector<Token> scan();

private:
    Lexer(const std::string& source, std::string file, size_t start, size_t end, int line, size_t lineStart);

    const std::string& src_;
    std::string file_;
    size_t pos_;
    size_t end_;
    int line_;
    size_t lineStart_;
    bool sawNewline_ = false;

    // Open brackets, innermost last. Line breaks only end statements at the top level or
    // directly inside braces; inside ( ) and [ ] they are plain whitespace.
    std::vector<char> brackets_;
    std::vector<Token> tokens_;

    int column() const { return static_cast<int>(pos_ - lineStart_) + 1; }
    bool next(char c) const { return pos_ + 1 < end_ && src_[pos_ + 1] == c; }
    CompileError error(const std::string& message, bool atEof = false) const;

    void skipTrivia();
    Token& add(TokenType type, size_t start, int line, int col);
    void scanToken();
    void number(size_t start, int line, int col);
    void string(char quote, size_t start, int line, int col);
    void escape(std::string& out);
    size_t findInterpolationEnd(size_t p) const;
    size_t skipString(size_t p) const;
};

}  // namespace astra
