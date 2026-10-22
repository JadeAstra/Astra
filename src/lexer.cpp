#include "lexer.h"

#include "value.h"

#include <charconv>
#include <unordered_map>

#include "section_compiler.h"  // last: this file's own code goes in its section (D161)

namespace astra {

namespace {

// Made the first time it is needed (M6.5 part 5: nothing of the compiler runs as a built program starts).
const std::unordered_map<std::string, TokenType>& keywords() {
    static const std::unordered_map<std::string, TokenType> table = {
    {"val", TokenType::Val},       {"var", TokenType::Var},           {"fun", TokenType::Fun},
    {"class", TokenType::Class},
    {"this", TokenType::This},     {"super", TokenType::Super},       {"return", TokenType::Return},
    {"if", TokenType::If},         {"else", TokenType::Else},         {"while", TokenType::While},
    {"for", TokenType::For},       {"in", TokenType::In},             {"break", TokenType::Break},
    {"continue", TokenType::Continue}, {"true", TokenType::True},     {"false", TokenType::False},
    {"null", TokenType::Null},     {"try", TokenType::Try},           {"catch", TokenType::Catch},
    {"throw", TokenType::Throw},   {"import", TokenType::Import},     {"is", TokenType::Is},
    {"and", TokenType::And},       {"or", TokenType::Or},             {"when", TokenType::When},
    };
    return table;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool isIdentPart(char c) { return isIdentStart(c) || isDigit(c); }

}  // namespace

CompileError::CompileError(std::string f, int l, int c, const std::string& message, bool eof)
    : std::runtime_error(message), file(std::move(f)), line(l), col(c), atEof(eof) {}

std::string CompileError::format() const {
    std::string text = file + ":" + std::to_string(line) + ":" + std::to_string(col) + ": error: " + what();
    for (const std::string& other : more) text += "\n" + other;
    return text;
}

Lexer::Lexer(const std::string& source, std::string file)
    : src_(source), file_(std::move(file)), pos_(0), end_(source.size()), line_(1), lineStart_(0) {}

Lexer::Lexer(const std::string& source, std::string file, size_t start, size_t end, int line)
    : src_(source), file_(std::move(file)), pos_(start), end_(end), line_(line), lineStart_(start) {}

Lexer::Lexer(const std::string& source, std::string file, size_t start, size_t end, int line, size_t lineStart)
    : src_(source), file_(std::move(file)), pos_(start), end_(end), line_(line), lineStart_(lineStart) {
    // An interpolated expression behaves as if it were parenthesized.
    brackets_.push_back('(');
}

CompileError Lexer::error(const std::string& message, bool atEof) const {
    return CompileError(file_, line_, column(), message, atEof);
}

std::vector<Token> Lexer::scan() {
    if (pos_ == 0 && src_.compare(0, 2, "#!") == 0)
        while (pos_ < end_ && src_[pos_] != '\n') pos_++;

    for (;;) {
        skipTrivia();
        if (pos_ >= end_) {
            Token eof;
            eof.type = TokenType::Eof;
            eof.line = line_;
            eof.col = column();
            eof.newlineBefore = true;
            tokens_.push_back(std::move(eof));
            return std::move(tokens_);
        }
        scanToken();
    }
}

void Lexer::skipTrivia() {
    while (pos_ < end_) {
        char c = src_[pos_];
        if (c == '\n') {
            sawNewline_ = true;
            pos_++;
            line_++;
            lineStart_ = pos_;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            pos_++;
        } else if (c == '/' && next('/')) {
            while (pos_ < end_ && src_[pos_] != '\n') pos_++;
        } else if (c == '/' && next('*')) {
            int startLine = line_;
            int startCol = column();
            pos_ += 2;
            for (;;) {
                if (pos_ + 1 >= end_) {
                    pos_ = end_;
                    throw CompileError(file_, startLine, startCol, "unterminated block comment", true);
                }
                if (src_[pos_] == '*' && src_[pos_ + 1] == '/') {
                    pos_ += 2;
                    break;
                }
                if (src_[pos_] == '\n') {
                    line_++;
                    lineStart_ = pos_ + 1;
                }
                pos_++;
            }
        } else {
            break;
        }
    }
}

Token& Lexer::add(TokenType type, size_t start, int line, int col) {
    Token t;
    t.type = type;
    t.text = src_.substr(start, pos_ - start);
    t.line = line;
    t.col = col;
    t.newlineBefore = sawNewline_ && (brackets_.empty() || brackets_.back() == '{');
    sawNewline_ = false;

    switch (type) {
        case TokenType::LParen: brackets_.push_back('('); break;
        case TokenType::LBracket: brackets_.push_back('['); break;
        case TokenType::LBrace: brackets_.push_back('{'); break;
        case TokenType::RParen:
        case TokenType::RBracket:
        case TokenType::RBrace:
            if (!brackets_.empty()) brackets_.pop_back();
            break;
        default: break;
    }

    tokens_.push_back(std::move(t));
    return tokens_.back();
}

void Lexer::scanToken() {
    size_t start = pos_;
    int line = line_;
    int col = column();
    char c = src_[pos_];

    if (isIdentStart(c)) {
        while (pos_ < end_ && isIdentPart(src_[pos_])) pos_++;
        auto it = keywords().find(src_.substr(start, pos_ - start));
        add(it == keywords().end() ? TokenType::Identifier : it->second, start, line, col);
        return;
    }

    if (isDigit(c)) {
        number(start, line, col);
        return;
    }

    if (c == '\'') {
        character(start, line, col);
        return;
    }
    if (c == '"') {
        string(c, start, line, col);
        return;
    }

    // `$$"""..."""`: a raw string that reads `$$name` and `$${...}`, a lone `$` being text (D112).
    if (c == '$') {
        size_t quote = pos_;
        while (quote < end_ && src_[quote] == '$') quote++;
        const bool raw = quote + 2 < end_ && src_[quote] == '"' && src_[quote + 1] == '"' && src_[quote + 2] == '"';
        if (quote - pos_ >= 2 && raw) {
            const size_t dollars = quote - pos_;
            pos_ = quote;
            string('"', start, line, col, dollars);
            return;
        }
        if (quote < end_ && (src_[quote] == '"' || src_[quote] == '\''))
            throw error(quote - pos_ == 1 ? "a string needs no '$' in front; for one whose dollar signs are text, write $$\"\"\"...\"\"\", which reads $$name"
                                          : "dollar signs in front go on a raw string: write " + std::string(quote - pos_, '$') + "\"\"\"...\"\"\"");
    }

    TokenType type = TokenType::Eof;
    size_t length = 1;
    auto withEq = [&](TokenType plain, TokenType assign) {
        if (next('=')) {
            type = assign;
            length = 2;
        } else {
            type = plain;
        }
    };

    switch (c) {
        case '(': type = TokenType::LParen; break;
        case ')': type = TokenType::RParen; break;
        case '{': type = TokenType::LBrace; break;
        case '}': type = TokenType::RBrace; break;
        case '[': type = TokenType::LBracket; break;
        case ']': type = TokenType::RBracket; break;
        case ',': type = TokenType::Comma; break;
        case ';': type = TokenType::Semicolon; break;
        case ':': type = TokenType::Colon; break;
        case '@': type = TokenType::At; break;
        case '#': type = TokenType::Hash; break;
        case '?':
            // `c ?.5 : x` is still a conditional: a digit cannot start a member name.
            if (next('.') && !(pos_ + 2 < end_ && isDigit(src_[pos_ + 2]))) {
                type = TokenType::QuestionDot;
                length = 2;
            } else if (next(':')) {
                type = TokenType::Elvis;
                length = 2;
            } else {
                type = TokenType::Question;
            }
            break;
        case '.':
            if (next('.') && pos_ + 2 < end_ && src_[pos_ + 2] == '.') {
                type = TokenType::Ellipsis;
                length = 3;
            } else if (next('.')) {
                if (pos_ + 2 < end_ && src_[pos_ + 2] == '=')
                    throw error("'..=' is not an operator; 'a..b' includes b, and 'a..<b' leaves it out");
                if (pos_ + 2 < end_ && src_[pos_ + 2] == '<') {
                    type = TokenType::DotDotLt;
                    length = 3;
                } else {
                    type = TokenType::DotDot;
                    length = 2;
                }
            } else if (next('=')) {
                type = TokenType::DotEq;
                length = 2;
            } else {
                type = TokenType::Dot;
            }
            break;
        case '+':
            if (next('+')) {
                type = TokenType::PlusPlus;
                length = 2;
            } else {
                withEq(TokenType::Plus, TokenType::PlusEq);
            }
            break;
        case '-':
            if (next('-')) {
                type = TokenType::MinusMinus;
                length = 2;
            } else if (next('>')) {
                type = TokenType::ThinArrow;
                length = 2;
            } else {
                withEq(TokenType::Minus, TokenType::MinusEq);
            }
            break;
        case '*': withEq(TokenType::Star, TokenType::StarEq); break;
        case '/': withEq(TokenType::Slash, TokenType::SlashEq); break;
        case '%': withEq(TokenType::Percent, TokenType::PercentEq); break;
        case '!':
            if (pos_ + 2 < end_ && src_[pos_ + 1] == '=' && src_[pos_ + 2] == '=') {
                type = TokenType::BangEqEq;
                length = 3;
            } else if (next('!')) {
                type = TokenType::BangBang;
                length = 2;
            } else if (pos_ + 2 < end_ && src_[pos_ + 1] == 'i' && (src_[pos_ + 2] == 's' || src_[pos_ + 2] == 'n') &&
                       !(pos_ + 3 < end_ && isIdentPart(src_[pos_ + 3]))) {
                // `x !is T` and `x !in list`; `!isReady` and `!index` are still a not.
                type = src_[pos_ + 2] == 's' ? TokenType::NotIs : TokenType::NotIn;
                length = 3;
            } else {
                withEq(TokenType::Bang, TokenType::BangEq);
            }
            break;
        case '<':
            if (next('<')) {
                bool assign = pos_ + 2 < end_ && src_[pos_ + 2] == '=';
                type = assign ? TokenType::ShlEq : TokenType::Shl;
                length = assign ? 3 : 2;
            } else {
                withEq(TokenType::Lt, TokenType::LtEq);
            }
            break;
        case '>':
            if (next('>') && pos_ + 2 < end_ && src_[pos_ + 2] == '>') {
                // `>>>`, a shift that brings in zeros (D44).
                bool assign = pos_ + 3 < end_ && src_[pos_ + 3] == '=';
                type = assign ? TokenType::UshrEq : TokenType::Ushr;
                length = assign ? 4 : 3;
            } else if (next('>')) {
                bool assign = pos_ + 2 < end_ && src_[pos_ + 2] == '=';
                type = assign ? TokenType::ShrEq : TokenType::Shr;
                length = assign ? 3 : 2;
            } else {
                withEq(TokenType::Gt, TokenType::GtEq);
            }
            break;
        case '=':
            if (next('=')) {
                const bool identity = pos_ + 2 < end_ && src_[pos_ + 2] == '=';
                type = identity ? TokenType::EqEqEq : TokenType::EqEq;
                length = identity ? 3 : 2;
            } else if (next('>')) {
                type = TokenType::Arrow;
                length = 2;
            } else {
                type = TokenType::Eq;
            }
            break;
        case '&':
            if (next('&')) throw error("'&&' is not an operator; logical and is written 'and'");
            if (next('+') || next('-') || next('*')) {
                // `a &+ b`: an int sum that wraps around rather than overflowing (D27), as Swift's.
                const char second = src_[pos_ + 1];
                type = second == '+' ? TokenType::AmpPlus : second == '-' ? TokenType::AmpMinus : TokenType::AmpStar;
                length = 2;
                break;
            }
            withEq(TokenType::Amp, TokenType::AmpEq);
            break;
        case '|':
            if (next('|')) throw error("'||' is not an operator; logical or is written 'or'");
            withEq(TokenType::Pipe, TokenType::PipeEq);
            break;
        case '^': withEq(TokenType::Caret, TokenType::CaretEq); break;
        case '~': type = TokenType::Tilde; break;
        default:
            throw error(std::string("unexpected character '") + c + "'");
    }

    // `++?`, `+=?` and the like: the null-safe forms (L118), the `?` part of the operator. They
    // are statements, so a `?` after `++` cannot be the start of `? :`, and no expression
    // begins with `?`.
    const bool update = type == TokenType::PlusPlus || type == TokenType::MinusMinus || type == TokenType::PlusEq ||
                        type == TokenType::MinusEq || type == TokenType::StarEq || type == TokenType::SlashEq ||
                        type == TokenType::PercentEq;
    if (update && pos_ + length < end_ && src_[pos_ + length] == '?') length++;
    pos_ += length;
    add(type, start, line, col);
}

void Lexer::number(size_t start, int line, int col) {
    auto invalid = [&]() { return CompileError(file_, line, col, "invalid number literal"); };

    if (src_[pos_] == '0' && pos_ + 1 < end_ &&
        (src_[pos_ + 1] == 'x' || src_[pos_ + 1] == 'X' || src_[pos_ + 1] == 'b' || src_[pos_ + 1] == 'B')) {
        int base = (src_[pos_ + 1] == 'x' || src_[pos_ + 1] == 'X') ? 16 : 2;
        pos_ += 2;
        std::string digits;
        while (pos_ < end_ && isIdentPart(src_[pos_])) {
            if (src_[pos_] != '_') digits += src_[pos_];
            pos_++;
        }
        uint64_t value = 0;
        auto res = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
        if (digits.empty() || res.ec != std::errc() || res.ptr != digits.data() + digits.size()) throw invalid();
        add(TokenType::Int, start, line, col).intValue = static_cast<int64_t>(value);
        return;
    }

    std::string text;
    auto digits = [&]() {
        while (pos_ < end_ && (isDigit(src_[pos_]) || src_[pos_] == '_')) {
            if (src_[pos_] != '_') text += src_[pos_];
            pos_++;
        }
    };

    digits();
    bool isFloat = false;

    // A '.' only belongs to the number when a digit follows, so `1..5` still lexes as a range.
    if (pos_ + 1 < end_ && src_[pos_] == '.' && isDigit(src_[pos_ + 1])) {
        isFloat = true;
        text += '.';
        pos_++;
        digits();
    }

    if (pos_ < end_ && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
        size_t p = pos_ + 1;
        if (p < end_ && (src_[p] == '+' || src_[p] == '-')) p++;
        if (p < end_ && isDigit(src_[p])) {
            isFloat = true;
            text += 'e';
            if (src_[pos_ + 1] == '-') text += '-';
            pos_ = p;
            digits();
        }
    }

    if (pos_ < end_ && isIdentStart(src_[pos_])) throw invalid();

    if (isFloat) {
        double value = 0;
        auto res = std::from_chars(text.data(), text.data() + text.size(), value);
        if (res.ec == std::errc::invalid_argument) throw invalid();
        add(TokenType::Float, start, line, col).floatValue = value;
    } else {
        int64_t value = 0;
        auto res = std::from_chars(text.data(), text.data() + text.size(), value);
        if (res.ec == std::errc::result_out_of_range)
            throw CompileError(file_, line, col, "integer literal is too large");
        if (res.ec != std::errc()) throw invalid();
        add(TokenType::Int, start, line, col).intValue = value;
    }
}

void Lexer::string(char quote, size_t start, int line, int col, size_t dollars) {
    // Three quotes open a raw string: it runs to the next three, over as many lines as it
    // likes, and a backslash in it is just a backslash. $name and ${...} still work, or, with
    // `dollars` signs in front of it, $$name and $${...}: fewer in a row are text (D112).
    const bool raw = quote == '"' && pos_ + 2 < end_ && src_[pos_ + 1] == '"' && src_[pos_ + 2] == '"';
    pos_ += raw ? 3 : 1;
    std::string text;
    std::shared_ptr<std::vector<InterpPart>> parts;

    for (;;) {
        if (pos_ >= end_) throw CompileError(file_, line, col, "unterminated string", true);
        char c = src_[pos_];

        if (raw && c == quote) {
            if (pos_ + 2 < end_ && src_[pos_ + 1] == quote && src_[pos_ + 2] == quote) {
                // Quotes beyond the closing three belong to the text.
                while (pos_ + 3 < end_ && src_[pos_ + 3] == quote) {
                    text += quote;
                    pos_++;
                }
                pos_ += 3;
                break;
            }
            text += c;
            pos_++;
            continue;
        }
        if (raw && c == '\\') {
            text += c;
            pos_++;
            continue;
        }

        if (c == quote) {
            pos_++;
            break;
        }

        if (c == '\n') {
            text += c;
            pos_++;
            line_++;
            lineStart_ = pos_;
            continue;
        }

        if (c == '\\') {
            pos_++;
            if (pos_ >= end_) throw CompileError(file_, line, col, "unterminated string", true);
            escape(text);
            continue;
        }

        // A run of dollar signs: only the last `dollars` of them can begin "$name" or "${".
        size_t after = pos_ + 1;
        if (c == '$') {
            size_t run = 1;
            while (pos_ + run < end_ && src_[pos_ + run] == '$') run++;
            if (run < dollars) {
                text.append(run, '$');
                pos_ += run;
                continue;
            }
            text.append(run - dollars, '$');
            pos_ += run - dollars;
            after = pos_ + dollars;
        }

        if (c == '$' && after < end_ && src_[after] == '{') {
            if (!parts) parts = std::make_shared<std::vector<InterpPart>>();
            if (!text.empty()) {
                InterpPart literal;
                literal.text = std::move(text);
                parts->push_back(std::move(literal));
                text.clear();
            }

            size_t exprStart = after + 1;
            size_t exprEnd = findInterpolationEnd(exprStart);
            Lexer sub(src_, file_, exprStart, exprEnd, line_, lineStart_);
            InterpPart expr;
            expr.isExpr = true;
            expr.tokens = sub.scan();
            if (expr.tokens.size() == 1) throw error("empty string interpolation");
            parts->push_back(std::move(expr));

            line_ = sub.line_;
            lineStart_ = sub.lineStart_;
            pos_ = exprEnd + 1;
            continue;
        }

        // "$name": a single variable. Anything longer ("$a.b", a call) needs the braces.
        if (c == '$' && after < end_ && isIdentStart(src_[after])) {
            size_t nameStart = after;
            size_t nameEnd = nameStart;
            while (nameEnd < end_ && isIdentPart(src_[nameEnd])) nameEnd++;
            std::string name = src_.substr(nameStart, nameEnd - nameStart);

            // Keywords that are not values ("$if") stay as ordinary text.
            TokenType type = TokenType::Identifier;
            auto keyword = keywords().find(name);
            if (keyword != keywords().end()) type = keyword->second;
            bool isValue = type == TokenType::Identifier || type == TokenType::This || type == TokenType::True ||
                           type == TokenType::False || type == TokenType::Null;

            if (isValue) {
                if (!parts) parts = std::make_shared<std::vector<InterpPart>>();
                if (!text.empty()) {
                    InterpPart literal;
                    literal.text = std::move(text);
                    parts->push_back(std::move(literal));
                    text.clear();
                }

                Token variable;
                variable.type = type;
                variable.text = name;
                variable.line = line_;
                variable.col = static_cast<int>(nameStart - lineStart_) + 1;
                Token eof;
                eof.type = TokenType::Eof;
                eof.line = line_;
                eof.col = static_cast<int>(nameEnd - lineStart_) + 1;
                eof.newlineBefore = true;

                InterpPart expr;
                expr.isExpr = true;
                expr.tokens.push_back(std::move(variable));
                expr.tokens.push_back(std::move(eof));
                parts->push_back(std::move(expr));

                pos_ = nameEnd;
                continue;
            }
        }
        if (c == '$') {
            text.append(dollars, '$');
            pos_ = after;
            continue;
        }

        text += c;
        pos_++;
    }

    if (!parts) {
        add(TokenType::String, start, line, col).stringValue = std::move(text);
        return;
    }

    if (!text.empty()) {
        InterpPart literal;
        literal.text = std::move(text);
        parts->push_back(std::move(literal));
    }
    add(TokenType::InterpString, start, line, col).parts = std::move(parts);
}

void Lexer::escape(std::string& out) {
    char e = src_[pos_++];
    switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '0': out += '\0'; break;
        case '\\': out += '\\'; break;
        case '"': out += '"'; break;
        case '\'': out += '\''; break;
        case '$': out += '$'; break;
        case 'u': {
            if (pos_ >= end_ || src_[pos_] != '{') throw error("expected '{' after \\u");
            size_t close = src_.find('}', pos_);
            if (close == std::string::npos || close >= end_) throw error("unterminated \\u{...} escape");
            const char* first = src_.data() + pos_ + 1;
            const char* last = src_.data() + close;
            uint32_t cp = 0;
            auto res = std::from_chars(first, last, cp, 16);
            if (first == last || res.ec != std::errc() || res.ptr != last || cp > 0x10FFFF ||
                (cp >= 0xD800 && cp <= 0xDFFF))
                throw error("invalid \\u{...} escape");
            appendUtf8(out, cp);
            pos_ = close + 1;
            break;
        }
        default:
            pos_--;
            throw error(std::string("unknown escape sequence '\\") + e + "'");
    }
}

// 'c': one character (D24, D25). Single quotes are for characters only; text takes double quotes.
void Lexer::character(size_t start, int line, int col) {
    pos_++;
    std::string text;
    size_t count = 0;
    for (;;) {
        if (pos_ >= end_ || src_[pos_] == '\n') throw CompileError(file_, line, col, "a character is not closed: it ends with '", pos_ >= end_);
        const char c = src_[pos_];
        if (c == '\'') break;
        if (c == '\\') {
            pos_++;
            if (pos_ >= end_) throw CompileError(file_, line, col, "a character is not closed: it ends with '", true);
            escape(text);
        } else {
            // One character, all the bytes that spell it in UTF-8.
            const unsigned char lead = static_cast<unsigned char>(c);
            size_t size = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (pos_ + size > end_) size = end_ - pos_;
            text.append(src_, pos_, size);
            pos_ += size;
        }
        count++;
    }
    pos_++;
    if (count != 1) {
        // Characters counted as written; an escape is one.
        const std::string written = src_.substr(start + 1, pos_ - start - 2);
        if (count == 0)
            throw CompileError(file_, line, col, "'' holds no character: a char is exactly one; the empty string is \"\"");
        throw CompileError(file_, line, col, "single quotes hold one character, and '" + written + "' holds " + std::to_string(count) +
                                                 ": text takes double quotes, \"" + written + "\"");
    }
    const Characters one = Characters::fromUtf8(text);
    if (one.size() != 1) throw CompileError(file_, line, col, "'" + text + "' is not one character");
    add(TokenType::Char, start, line, col).intValue = one[0];
}

// Finds the '}' closing a "${" whose expression starts at `p`.
size_t Lexer::findInterpolationEnd(size_t p) const {
    int depth = 1;
    while (p < end_) {
        char c = src_[p];
        if (c == '"' || c == '\'' || (c == '$' && p + 1 < end_ && src_[p + 1] == '$')) {
            p = skipString(p);
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}' && --depth == 0) {
            return p;
        }
        p++;
    }
    throw error("unterminated string interpolation", true);
}

// Skips a string inside an interpolation, raw or not, with dollar signs in front or not.
size_t Lexer::skipString(size_t p) const {
    size_t dollars = 1;
    if (src_[p] == '$') {
        const size_t first = p;
        while (p < end_ && src_[p] == '$') p++;
        if (p >= end_ || src_[p] != '"') return p;  // not a string: `$$` in code is an error later
        dollars = p - first;
    }
    const char quote = src_[p];
    const bool raw = quote == '"' && p + 2 < end_ && src_[p + 1] == '"' && src_[p + 2] == '"';
    p += raw ? 3 : 1;
    while (p < end_) {
        char c = src_[p];
        if (raw && c == quote && p + 2 < end_ && src_[p + 1] == quote && src_[p + 2] == quote) {
            p += 3;
            while (p < end_ && src_[p] == quote) p++;
            return p;
        }
        if (!raw && c == '\\') {
            p += 2;
            continue;
        }
        if (!raw && c == quote) return p + 1;
        if (c == '$') {
            size_t run = 1;
            while (p + run < end_ && src_[p + run] == '$') run++;
            if (run >= dollars && p + run < end_ && src_[p + run] == '{') {
                p = findInterpolationEnd(p + run + 1) + 1;
            } else {
                p += run;
            }
            continue;
        }
        p++;
    }
    throw error("unterminated string", true);
}

}  // namespace astra
