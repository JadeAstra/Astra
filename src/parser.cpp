#include "parser.h"

#include <algorithm>

namespace astra {

namespace {

const int kMaxNesting = 256;

// Kotlin's `open`, `final` and `sealed` are not words of Astra: every class can be subclassed
// and every member overridden, and a fixed set of kinds is an `enum class`. Written in front
// of a declaration, any of them is an error.
bool kotlinOnlyWord(const std::string& word) { return word == "open" || word == "final" || word == "sealed"; }

std::string notAstraWord(const std::string& word) {
    if (word == "sealed")
        return "'sealed' is not a keyword; a fixed set of kinds is an 'enum class', and an 'abstract class' "
               "can stand in for one";
    return "'" + word + "' is not a keyword; every class can be subclassed and every member overridden";
}

// Type names are Title case, but for a fixed set (R21): bool, char, byte, int, float, the
// storage widths, string, any, nothing and unit. The lowercase names the other built-in
// types had give the one to write.
const char* oldTypeName(const std::string& name) {
    if (name == "list") return "List";
    if (name == "map") return "Map";
    if (name == "number") return "Number";
    if (name == "bytes") return "Bytes";
    if (name == "range") return "Range";
    if (name == "function") return "Function";
    return nullptr;
}

std::string oldTypeMessage(const std::string& name, const char* titled) {
    return "type names are written in Title case: '" + name + "' is " + titled +
           " (only bool, char, byte, int, float, string, any, nothing, unit and the storage widths are lowercase)";
}

std::string describe(const Token& t) {
    return t.type == TokenType::Eof ? "end of input" : "'" + t.text + "'";
}

template <class T>
std::unique_ptr<T> node(const Token& at) {
    auto n = std::make_unique<T>();
    n->line = at.line;
    n->col = at.col;
    return n;
}

ExprPtr binary(const Token& op, ExprPtr left, ExprPtr right) {
    auto e = node<BinaryExpr>(op);
    e->op = op.type;
    e->left = std::move(left);
    e->right = std::move(right);
    return e;
}

}  // namespace

struct Parser::DepthGuard {
    Parser& parser;
    explicit DepthGuard(Parser& p) : parser(p) {
        if (++parser.depth_ > kMaxNesting) {
            parser.depth_--;
            throw parser.error(parser.peek(), "code is nested too deeply");
        }
    }
    ~DepthGuard() { parser.depth_--; }
};

// Sets headerExpr_ for as long as it lives and then puts the old value back.
struct Parser::HeaderGuard {
    Parser& parser;
    bool saved;
    HeaderGuard(Parser& p, bool value) : parser(p), saved(p.headerExpr_) { p.headerExpr_ = value; }
    ~HeaderGuard() { restore(); }
    void restore() { parser.headerExpr_ = saved; }
};

Parser::Parser(const std::vector<Token>& tokens, std::string file) : toks_(tokens), file_(std::move(file)) {}

const Token& Parser::advance() {
    const Token& t = toks_[p_];
    if (t.type != TokenType::Eof) p_++;
    return t;
}

bool Parser::match(TokenType type) {
    if (toks_[p_].type != type) return false;
    p_++;
    return true;
}

const Token& Parser::expect(TokenType type, const char* what) {
    if (check(type)) return advance();
    throw error(peek(), std::string("expected ") + what + ", found " + describe(peek()));
}

CompileError Parser::error(const Token& at, const std::string& message) const {
    return CompileError(file_, at.line, at.col, message, at.type == TokenType::Eof);
}

std::vector<StmtPtr> Parser::parseProgram() {
    std::vector<StmtPtr> statements;
    for (;;) {
        skipSemicolons();
        if (check(TokenType::Eof)) return statements;
        statements.push_back(declaration());
    }
}

// Parses a token stream holding exactly one expression (a string interpolation).
ExprPtr Parser::parseSingleExpression() {
    ExprPtr e = expression();
    if (!check(TokenType::Eof)) throw error(peek(), "unexpected " + describe(peek()) + " in string interpolation");
    return e;
}

// ---- Statements ----

void Parser::skipSemicolons() {
    while (match(TokenType::Semicolon)) {
    }
}

void Parser::endStatement() {
    if (match(TokenType::Semicolon)) return;
    if (check(TokenType::RBrace) || check(TokenType::Eof) || peek().newlineBefore) return;
    throw error(peek(), "expected a new line or ';' before " + describe(peek()));
}

bool Parser::isKeyword(TokenType type) {
    switch (type) {
        case TokenType::Val: case TokenType::Var: case TokenType::Fun: case TokenType::Class:
        case TokenType::This: case TokenType::Super: case TokenType::Return:
        case TokenType::If: case TokenType::Else: case TokenType::While: case TokenType::For: case TokenType::In:
        case TokenType::Break: case TokenType::Continue: case TokenType::True: case TokenType::False:
        case TokenType::Null: case TokenType::Try: case TokenType::Catch: case TokenType::Throw:
        case TokenType::Import: case TokenType::Is: case TokenType::And: case TokenType::Or: case TokenType::When:
            return true;
        default:
            return false;
    }
}

const Token& Parser::expectName(const char* what) {
    if (isName(peek())) return advance();
    return expect(TokenType::Identifier, what);
}

// `@Volatile` in front of a declaration is accepted and means nothing: with one thread
// running Astra code at a time, a variable is always seen whole by the others. No other
// annotation is part of the language, and one that would mean something is not passed over.
void Parser::skipAnnotations() {
    while (check(TokenType::At) && peekNext().type == TokenType::Identifier) {
        if (peekNext().text != "Volatile")
            throw error(peek(), "annotations are not part of Astra: '@" + peekNext().text +
                                    "' has to be written out as code. Only @Volatile is accepted, and means nothing");
        advance();
        advance();
    }
}

StmtPtr Parser::declaration() {
    DepthGuard guard(*this);
    skipAnnotations();
    switch (peek().type) {
        case TokenType::Identifier: {
            // `let x = 1` would otherwise fail with a puzzling message about a missing line break.
            const Token& word = peek();
            const Token& after = peekNext();
            if (word.text == "let" && after.type == TokenType::Identifier && !after.newlineBefore)
                throw error(word, "'let' is not a keyword; declare variables with 'val' (immutable) or 'var'");
            if (word.text == "const" && !after.newlineBefore &&
                (after.type == TokenType::Identifier || after.type == TokenType::Val || after.type == TokenType::Var))
                return constDeclaration(false);
            if (word.text == "fn" && after.type == TokenType::Identifier && !after.newlineBefore)
                throw error(word, "'fn' is not a keyword; declare functions with 'fun'");
            // Who may see a declaration: `private` its file, `internal` its library, and
            // otherwise everyone. `public` is accepted and changes nothing.
            const bool makesAbstract = word.text == "abstract";
            // `suspend fun` marks a function that may wait, setting its flow aside (R1).
            const bool modifier = word.text == "private" || word.text == "internal" || word.text == "public" ||
                                  makesAbstract ||
                                  (word.text == "suspend" && (after.type == TokenType::Fun || after.type == TokenType::Identifier));
            const bool declares = after.type == TokenType::Val || after.type == TokenType::Var || after.type == TokenType::Fun ||
                                  after.type == TokenType::Class || after.type == TokenType::Identifier;
            if (kotlinOnlyWord(word.text) && declares && !after.newlineBefore) throw error(word, notAstraWord(word.text));
            if (word.text == "external" && !after.newlineBefore &&
                (after.type == TokenType::Fun || after.type == TokenType::Val ||
                 (after.type == TokenType::Identifier && after.text == "object")))
                return externalDeclaration();
            if (modifier && declares && !after.newlineBefore) {
                advance();
                StmtPtr declared = declaration();
                if (declared->kind != StmtKind::Var && declared->kind != StmtKind::Function && declared->kind != StmtKind::Class)
                    throw error(word, "'" + word.text + "' goes in front of a declaration");
                if (makesAbstract) {
                    if (declared->kind != StmtKind::Class || static_cast<const ClassStmt&>(*declared).isEnum ||
                        static_cast<const ClassStmt&>(*declared).isObject)
                        throw error(word, "'" + word.text + "' goes in front of a class");
                    static_cast<ClassStmt&>(*declared).isAbstract = true;
                }
                if (word.text == "suspend") {
                    if (declared->kind != StmtKind::Function) throw error(word, "'suspend' goes in front of a function");
                    static_cast<FunctionStmt&>(*declared).fn->isSuspend = true;
                }
                if (word.text == "private") declared->visibility = 2;
                else if (word.text == "internal" && declared->visibility == 0) declared->visibility = 1;
                return declared;
            }
            if (word.text == "enum" && after.type == TokenType::Identifier && !after.newlineBefore) {
                // `enum Name { ... }`; like `object`, the word is only special here.
                StmtPtr declared = classDeclaration(true);
                const auto& cls = static_cast<const ClassStmt&>(*declared);
                if (cls.hasSuperArgs) throw error(after, "enum '" + cls.name + "' cannot have a superclass");
                return declared;
            }
            if (word.text == "data" && after.type == TokenType::Identifier && !after.newlineBefore) {
                // `data Point(val x, val y)`; `data` is an ordinary name anywhere else.
                StmtPtr declared = classDeclaration();
                auto& cls = static_cast<ClassStmt&>(*declared);
                if (!cls.hasHeader || cls.header.empty())
                    throw error(after, "data '" + cls.name + "' needs a header listing its fields: data " + cls.name + "(val x, val y)");
                for (const HeaderParam& param : cls.header)
                    if (!param.isField)
                        throw error(after, "'" + param.name + "' in the header of data '" + cls.name + "' needs val or var: every entry is a field");
                cls.isData = true;
                addDataMembers(cls, word);
                return declared;
            }
            if (word.text == "interface" && after.type == TokenType::Identifier && !after.newlineBefore) {
                StmtPtr declared = classDeclaration(false, true);
                const auto& cls = static_cast<const ClassStmt&>(*declared);
                if (cls.hasHeader) throw error(after, "interface '" + cls.name + "' cannot have a header: it is never created");
                return declared;
            }
            if (word.text == "object" && after.type == TokenType::Identifier && !after.newlineBefore) {
                // `object Name { ... }`; `object` is an ordinary name anywhere else.
                nextIsObject_ = true;
                StmtPtr declared = classDeclaration();
                auto& cls = static_cast<ClassStmt&>(*declared);
                if (cls.hasHeader)
                    throw error(after, "object '" + cls.name + "' cannot have a header: it is created once, with no arguments");
                cls.isObject = true;
                return declared;
            }
            return statement();
        }
        case TokenType::Class:
            return classDeclaration();
        case TokenType::Fun:
            if (peekNext().type == TokenType::Identifier || peekNext().type == TokenType::Lt) {
                const Token& kw = advance();
                // `fun <T> first(items : list<T>) : T`: type parameters come first.
                std::vector<TypeParam> typeParams;
                if (check(TokenType::Lt)) typeParams = typeParameters();
                TypeParamScope typeParamsInReach(*this, typeParams);
                const Token* name = &expect(TokenType::Identifier, "the name of the function");
                auto s = node<FunctionStmt>(kw);
                // fun Shape.describe(), fun list<Shape>.total(), fun string?.orEmpty(): what stands
                // before the last dot is the type the function is added to.
                if (check(TokenType::Dot) || check(TokenType::Lt) || check(TokenType::QuestionDot)) {
                    if (const char* titled = oldTypeName(name->text)) throw error(*name, oldTypeMessage(name->text, titled));
                    s->receiver = name->text;
                    s->receiverType.line = name->line;
                    s->receiverType.col = name->col;
                    for (;;) {
                        if (check(TokenType::Lt)) {
                            // What a list or map holds: kept for the type checker; when the
                            // program runs the function is for every list.
                            advance();
                            s->receiverType.args = typeArgumentsRest();
                        }
                        if (match(TokenType::QuestionDot)) s->receiverNullable = true;
                        else expect(TokenType::Dot, "'.' and the name of the function");
                        name = &expectName("the name of the function");
                        if (check(TokenType::LParen)) break;
                        if (s->receiverNullable) throw error(*name, "expected '(' after the name of the function");
                        s->receiver += "." + name->text;
                        s->receiverType.args.clear();
                    }
                    s->receiverType.name = s->receiver;
                    s->receiverType.nullable = s->receiverNullable;
                    if (isTypeParam(s->receiver)) s->receiverType.typeParam = true;
                }
                s->fn = functionRest(name->text, kw);
                s->fn->typeParams = std::move(typeParams);
                return s;
            }
            return statement();
        case TokenType::Val:
        case TokenType::Var:
            return varDeclaration(false);
        default:
            return statement();
    }
}

// `external fun name(...) : T`, `external fun Type.name(...) : T` or `external val Type.name : T`:
// what the C++ provides, declared with its types. The word `external` is next.
StmtPtr Parser::externalDeclaration() {
    const Token& word = advance();
    if (check(TokenType::Identifier) && peek().text == "object") {
        // `external object Math { const pi = ...; fun sqrt(x : Number) : float }`
        nextIsObject_ = true;
        const bool saved = externalBody_;
        externalBody_ = true;
        StmtPtr declared = classDeclaration();
        externalBody_ = saved;
        auto& cls = static_cast<ClassStmt&>(*declared);
        cls.isExternal = true;
        for (const auto& m : cls.methods)
            if (!m->isAbstract) throw error(word, "a function of an external object has no body: the C++ provides it");
        return declared;
    }
    if (check(TokenType::Val)) {
        advance();
        auto v = node<VarStmt>(word);
        v->isExternal = true;
        v->isVal = true;
        if (check(TokenType::Lt)) v->typeParams = typeParameters();
        TypeParamScope typeParamsInReach(*this, v->typeParams);
        v->receiverType = typeExpression();
        // `String.size` reads as one dotted type name; `Map<K, V>.keys` has its dot after the '>'.
        const size_t dot = v->receiverType.name.rfind('.');
        if (dot != std::string::npos) {
            v->name = v->receiverType.name.substr(dot + 1);
            v->receiverType.name.resize(dot);
        } else {
            expect(TokenType::Dot, "'.' and the property's name: external val String.size : int");
            v->name = expectName("the property's name").text;
        }
        v->type = optionalType();
        if (!v->type.present()) throw error(word, "an external property writes its type");
        endStatement();
        return v;
    }
    const bool savedAbstract = abstractMember_;
    abstractMember_ = true;  // no body
    StmtPtr declared = declaration();
    abstractMember_ = savedAbstract;
    if (declared->kind != StmtKind::Function) throw error(word, "'external' goes in front of fun or val");
    auto& fs = static_cast<FunctionStmt&>(*declared);
    if (!fs.fn->isAbstract) throw error(word, "an external function has no body: the C++ provides it");
    fs.isExternal = true;
    return declared;
}

// Parses ": type" or ": type?" if a ':' comes next; otherwise returns an absent annotation.
TypeAnnotation Parser::optionalType() {
    if (!match(TokenType::Colon)) return TypeAnnotation{};
    return typeExpression();
}

// A type: `int`, `Animal?`, `list<string>`, `(int, string) -> bool`, `Grid.() -> Unit`.
// Everything is kept for the type checker; when the program runs only "List", "Map" or
// "Function" is checked.
TypeAnnotation Parser::typeExpression() {
    DepthGuard guard(*this);
    TypeAnnotation type;
    type.line = peek().line;
    type.col = peek().col;
    // `suspend () -> unit`: a function type, said to be of a function that may set a flow aside.
    bool suspends = false;
    const Token* suspendWord = nullptr;
    if (check(TokenType::Identifier) && peek().text == "suspend" &&
        (peekNext().type == TokenType::LParen || peekNext().type == TokenType::Identifier)) {
        suspendWord = &advance();
        suspends = true;
    }

    // What was between the parentheses, for telling `(int) -> bool` from `(int)`.
    bool named = false;
    std::vector<TypeAnnotation> inside;
    auto functionRest = [&] {
        // The '(' has been read: parameter types, ')', '->' and the result type.
        const bool tight = tightNullable_;
        tightNullable_ = false;
        while (!check(TokenType::RParen)) {
            if (check(TokenType::Identifier) && peekNext().type == TokenType::Colon) {
                advance();  // a parameter name, as in (delayMs : int) -> Unit
                advance();
                named = true;
            }
            inside.push_back(typeExpression());
            if (!match(TokenType::Comma)) break;
        }
        tightNullable_ = tight;
        expect(TokenType::RParen, "')' in a function type");
    };

    if (match(TokenType::LParen)) {
        functionRest();
        if (match(TokenType::ThinArrow)) {
            type.result.push_back(typeExpression());
            type.params = std::move(inside);
            type.name = "Function";
        } else if (inside.size() == 1 && !named) {
            // A type in parentheses, there so that a '?' can follow: ((int) -> bool)?
            const bool nullable = type.nullable;
            const int line = type.line, col = type.col;
            type = std::move(inside[0]);
            type.nullable = type.nullable || nullable;
            type.line = line;
            type.col = col;
        } else {
            throw error(peek(), "expected '->' after the parameter types of a function type");
        }
    } else {
        const Token& name = expect(TokenType::Identifier, "a type name after ':'");
        if (const char* titled = oldTypeName(name.text)) throw error(name, oldTypeMessage(name.text, titled));
        type.name = name.text;
        // A class declared inside another: `Event.Click`.
        while (check(TokenType::Dot) && peekNext().type == TokenType::Identifier) {
            advance();
            type.name += "." + advance().text;
        }
        if (type.name.find('.') == std::string::npos && isTypeParam(type.name)) type.typeParam = true;
        if (match(TokenType::Lt)) type.args = typeArgumentsRest();
        if (check(TokenType::Dot) && peekNext().type == TokenType::LParen) {
            // A function that runs on an object of this type: `Grid.() -> Unit`.
            advance();
            advance();
            functionRest();
            expect(TokenType::ThinArrow, "'->' in a function type");
            TypeAnnotation receiver = type;
            receiver.result.clear();
            type.result.push_back(typeExpression());
            type.params = std::move(inside);
            type.receiver.push_back(std::move(receiver));
            type.args.clear();
            type.name = "Function";
        }
    }
    if (suspends) {
        if (!type.isFunction()) throw error(*suspendWord, "'suspend' goes in front of a function type: suspend () -> unit");
        type.isSuspend = true;
    }
    while (check(TokenType::Question) && (!tightNullable_ || adjacentToPrevious())) {
        advance();
        type.nullable = true;
    }
    return type;
}

// After '<': type arguments up to the '>'. `list<*>` takes any element type, and
// `map<string, list<int>>` ends in what the lexer reads as one '>>'.
std::vector<TypeAnnotation> Parser::typeArgumentsRest() {
    std::vector<TypeAnnotation> args;
    do {
        if (check(TokenType::Star)) {
            TypeAnnotation any;
            any.line = peek().line;
            any.col = peek().col;
            advance();
            any.star = true;
            args.push_back(std::move(any));
        } else {
            args.push_back(typeExpression());
        }
    } while (match(TokenType::Comma));
    if (check(TokenType::Shr)) {
        if (pendingTypeClose_) {
            pendingTypeClose_ = false;
            advance();
        } else {
            pendingTypeClose_ = true;
        }
    } else if (pendingTypeClose_) {
        // The outer list's '>' was the second half of a '>>' already read.
        pendingTypeClose_ = false;
    } else {
        expect(TokenType::Gt, "'>' to end the type arguments");
    }
    return args;
}

// `<T, U : Comparable<U>>`: the type parameters of a class or function. Each is in reach of
// the bounds after it, so a bound may name it.
std::vector<TypeParam> Parser::typeParameters() {
    expect(TokenType::Lt, "'<'");
    std::vector<TypeParam> params;
    typeParamScopes_.emplace_back();
    do {
        // Kotlin's `reified` marks what Astra always does (R1).
        if (check(TokenType::Identifier) && peek().text == "reified" && peekNext().type == TokenType::Identifier)
            throw error(peek(), "'reified' is not needed: type arguments are kept when the program runs (L14), so "
                                "x is T works in any generic function");
        const Token& name = expect(TokenType::Identifier, "a type parameter's name");
        for (const TypeParam& earlier : params)
            if (earlier.name == name.text) throw error(name, "type parameter '" + name.text + "' is named twice");
        TypeParam p;
        p.line = name.line;
        p.col = name.col;
        p.name = name.text;
        typeParamScopes_.back().push_back(p.name);
        if (match(TokenType::Colon)) p.bound = typeExpression();
        params.push_back(std::move(p));
    } while (match(TokenType::Comma));
    typeParamScopes_.pop_back();
    if (check(TokenType::Shr) && pendingTypeClose_) {
        pendingTypeClose_ = false;
        advance();
    } else if (pendingTypeClose_) {
        pendingTypeClose_ = false;
    } else {
        expect(TokenType::Gt, "'>' to end the type parameters");
    }
    return params;
}

bool Parser::isTypeParam(const std::string& name) const {
    for (const auto& scope : typeParamScopes_)
        for (const std::string& n : scope)
            if (n == name) return true;
    return false;
}

// `first<int>(xs)`, `Box<string>(null)`, `emptyList<int>()`: type arguments written before a
// call's arguments. A '<' there could also be a comparison (`a < b`), so the type arguments
// are tried, and taken only if they read as types and a '(' or a lambda follows them.
bool Parser::typeArgumentsBeforeCall(std::vector<TypeAnnotation>& into) {
    if (!check(TokenType::Lt) || peek().newlineBefore) return false;
    const size_t start = p_;
    const bool savedClose = pendingTypeClose_;
    try {
        advance();
        std::vector<TypeAnnotation> args = typeArgumentsRest();
        // A lambda after them only outside the head of an if or while, where `a < b {` is a comparison.
        if (!pendingTypeClose_ && (check(TokenType::LParen) || (check(TokenType::LBrace) && !headerExpr_)) && !peek().newlineBefore) {
            into = std::move(args);
            return true;
        }
    } catch (const CompileError&) {
    }
    p_ = start;
    pendingTypeClose_ = savedClose;
    return false;
}

// The type after `is` or `as`. There a '?' only makes the type nullable when it is written
// against it, so that `x is Shape ? a : b` is still a conditional.
TypeAnnotation Parser::checkedType() {
    const bool saved = tightNullable_;
    tightNullable_ = true;
    TypeAnnotation type = typeExpression();
    tightNullable_ = saved;
    return type;
}

// Whether the next token starts exactly where the one before it ended.
bool Parser::adjacentToPrevious() const {
    if (p_ == 0) return false;
    const Token& before = toks_[p_ - 1];
    const Token& t = peek();
    return !t.newlineBefore && before.line == t.line && before.col + static_cast<int>(before.text.size()) == t.col;
}

// A `val`/`var` declaration. As a class field, a val may be left for the initializer to set.
// `(a, b, _)` after val, var or for. The '(' is next.
std::vector<std::string> Parser::destructuringPattern() {
    const Token& open = advance();
    std::vector<std::string> names;
    do {
        const Token& name = expect(TokenType::Identifier, "a variable name");
        if (name.text != "_" && std::find(names.begin(), names.end(), name.text) != names.end())
            throw error(name, "'" + name.text + "' appears twice");
        names.push_back(name.text);
        optionalType();  // allowed, as in Kotlin, and not checked
    } while (match(TokenType::Comma) && !check(TokenType::RParen));
    expect(TokenType::RParen, "')'");
    if (names.size() > 255) throw error(open, "too many names");
    return names;
}

std::unique_ptr<VarStmt> Parser::varDeclaration(bool isField) {
    const Token& kw = advance();
    if (!isField && check(TokenType::LParen)) {
        // val (a, b) = value
        auto s = node<VarStmt>(peek());
        s->isVal = kw.type == TokenType::Val;
        s->pattern = destructuringPattern();
        expect(TokenType::Eq, "'=' and the value to take apart");
        s->init = expression();
        endStatement();
        return s;
    }
    if (kw.type != TokenType::Identifier && check(TokenType::Identifier) && peek().text == "const" &&
        peekNext().type == TokenType::Identifier && !peekNext().newlineBefore)
        throw error(peek(), "a constant is written 'const " + peekNext().text + " = ...', without '" + kw.text + "'");
    const Token& name = isField ? expectName("a field name") : expect(TokenType::Identifier, "a variable name");
    auto s = node<VarStmt>(name);
    s->name = name.text;
    s->isVal = kw.type == TokenType::Val;

    s->type = optionalType();

    if (match(TokenType::Eq)) {
        s->init = expression();
    } else if (s->isVal && !isField) {
        throw error(peek(), "val '" + name.text + "' must be initialized");
    }
    // An accessor may follow on the same line: val area : int get() => width * height
    if (isField && (word("get") || word("set")) && peekNext().type == TokenType::LParen) return s;
    endStatement();
    return s;
}

// `const maxRows = 1000`, with the word `const` next. Kotlin's `const val` is refused, saying what
// to write, and so is a constant without its value.
std::unique_ptr<VarStmt> Parser::constDeclaration(bool isField) {
    const Token& word = peek();
    if (peekNext().type == TokenType::Val || peekNext().type == TokenType::Var) {
        const Token& kind = toks_[p_ + 1];
        const std::string name = p_ + 2 < toks_.size() && toks_[p_ + 2].type == TokenType::Identifier ? toks_[p_ + 2].text : "maxRows";
        throw error(word, "a constant is written 'const " + name + " = ...', without '" + kind.text + "'");
    }
    auto s = varDeclaration(isField);
    s->isVal = true;
    s->isConst = true;
    if (!s->init) throw error(word, "const '" + s->name + "' is given its value where it is declared: const " + s->name + " = ...");
    return s;
}

// After the word `object` where a value is expected: does `{`, or `: Type, Type(args) {`, follow?
bool Parser::anonymousObjectNext() const {
    if (peek().newlineBefore) return false;
    if (check(TokenType::LBrace)) return true;
    if (!check(TokenType::Colon)) return false;
    int nesting = 0;
    bool wantName = true;
    for (size_t i = p_ + 1; i < toks_.size(); i++) {
        const Token& t = toks_[i];
        if (nesting > 0) {
            if (t.type == TokenType::LParen) nesting++;
            else if (t.type == TokenType::RParen) nesting--;
            else if (t.type == TokenType::Eof) return false;
            continue;
        }
        if (t.type == TokenType::LBrace) return !wantName;
        if (wantName) {
            if (t.type != TokenType::Identifier) return false;
            wantName = false;
        } else if (t.type == TokenType::Dot || t.type == TokenType::Comma) {
            wantName = true;
        } else if (t.type == TokenType::LParen) {
            nesting = 1;
        } else {
            return false;
        }
    }
    return false;
}

StmtPtr Parser::classDeclaration(bool isEnum, bool isInterface, const Token* anonymous) {
    if (anonymous == nullptr) advance();
    const Token& name = anonymous != nullptr ? *anonymous
                        : expect(TokenType::Identifier, isEnum ? "an enum name" : isInterface ? "an interface name" : "a class name");
    auto s = node<ClassStmt>(name);
    s->name = name.text;
    s->isEnum = isEnum;
    s->isInterface = isInterface;
    s->isObject = nextIsObject_;  // known before the body, where a const may be declared
    nextIsObject_ = false;
    struct InterfaceBody {
        bool& flag;
        bool saved;
        InterfaceBody(bool& f, bool value) : flag(f), saved(f) { flag = value; }
        ~InterfaceBody() { flag = saved; }
    } interfaceBody(inInterface_, isInterface);

    // `class Box<T>`: type parameters, in reach of everything that follows.
    if (check(TokenType::Lt) && !peek().newlineBefore) s->typeParams = typeParameters();
    TypeParamScope typeParamsInReach(*this, s->typeParams);

    // Header: class Name(val a : T, var b : U)
    if (match(TokenType::LParen)) {
        s->hasHeader = true;
        while (!check(TokenType::RParen)) {
            // Kotlin's modifiers in front of a header field: `override` and the access words are
            // noted, `public` accepted, `open`, `final` and `sealed` refused.
            skipAnnotations();
            bool overrides = false;
            uint8_t access = 0;
            while (check(TokenType::Identifier) &&
                   (peekNext().type == TokenType::Val || peekNext().type == TokenType::Var || peekNext().type == TokenType::Identifier) &&
                   (peek().text == "override" || peek().text == "private" || peek().text == "internal" || peek().text == "public" ||
                    peek().text == "protected" || kotlinOnlyWord(peek().text))) {
                if (kotlinOnlyWord(peek().text)) throw error(peek(), notAstraWord(peek().text));
                overrides = overrides || peek().text == "override";
                if (accessWord(peek().text) != 0) access = accessWord(peek().text);
                advance();
            }
            // `val x : T` and `var x : T` are fields; a bare `x : T` is only a parameter.
            bool isField = check(TokenType::Val) || check(TokenType::Var);
            bool isVal = check(TokenType::Val);
            if (isField) advance();
            const Token& paramName = isField ? expectName("a field name")
                                             : expect(TokenType::Identifier, "'val', 'var' or a parameter name");

            HeaderParam param;
            param.line = paramName.line;
            param.col = paramName.col;
            param.name = paramName.text;
            param.isField = isField;
            param.type = optionalType();
            for (const HeaderParam& earlier : s->header)
                if (earlier.name == param.name) throw error(paramName, "duplicate name '" + param.name + "' in the class header");
            if (match(TokenType::Eq)) param.defaultValue = expression();

            if (isField) {
                auto field = node<VarStmt>(paramName);
                field->name = param.name;
                field->isVal = isVal;
                field->type = param.type;
                field->isOverride = overrides;
                field->access = access;
                s->fields.push_back(std::move(field));
            }
            s->header.push_back(std::move(param));
            if (!match(TokenType::Comma)) break;
        }
        expect(TokenType::RParen, "')' to end the class header");
        s->headerFieldCount = s->fields.size();
        if (s->header.size() > 255) throw error(name, "a class header cannot have more than 255 entries");
    }

    if (match(TokenType::Colon)) {
        // A superclass and interfaces, in any order: `: Base(args), Clickable, Focusable`.
        s->hasSuper = true;
        do {
            const Token& super = expect(TokenType::Identifier, "a superclass or interface name");
            std::string superName = super.text;
            while (check(TokenType::Dot) && peekNext().type == TokenType::Identifier) {
                advance();
                superName += "." + advance().text;
            }
            for (const std::string& earlier : s->supertypes)
                if (earlier == superName) throw error(super, "'" + superName + "' is listed twice");
            if (superName == s->name) throw error(super, "'" + s->name + "' cannot inherit from itself");
            s->supertypes.push_back(superName);
            TypeAnnotation superType;
            superType.line = super.line;
            superType.col = super.col;
            superType.name = superName;
            // `: Comparable<Money>`, `: Box<T>`: what the supertype is given for its type parameters.
            if (check(TokenType::Lt) && !peek().newlineBefore) {
                advance();
                superType.args = typeArgumentsRest();
            }
            s->supertypeTypes.push_back(std::move(superType));

            // : Super(args) passes arguments to the superclass initializer.
            if (check(TokenType::LParen) && !peek().newlineBefore) {
                const Token& open = advance();
                if (s->hasSuperArgs)
                    throw error(open, "only the superclass takes arguments, and a class has one superclass");
                if (isInterface) throw error(open, "an interface cannot pass arguments to what it extends");
                s->hasSuperArgs = true;
                s->superName = superName;
                while (!check(TokenType::RParen)) {
                    s->superArgs.push_back(expression());
                    if (!match(TokenType::Comma)) break;
                }
                expect(TokenType::RParen, "')' after the superclass arguments");
                if (s->superArgs.size() > 255) throw error(open, "a call cannot have more than 255 arguments");
            }
        } while (match(TokenType::Comma));
        if (s->superName.empty()) s->superName = s->supertypes.front();
        if (s->supertypes.size() > 255) throw error(name, "too many supertypes");
    }

    // The body is optional: `class Point(val x : int, val y : int)` is complete as it stands.
    if (!check(TokenType::LBrace)) {
        endStatement();
        return s;
    }

    expect(TokenType::LBrace, "'{' to begin the class body");
    if (isEnum) {
        // The entries come first, separated by commas; a ';' or the first member ends them.
        // An entry is a name; what follows tells it from a member that starts with the same word.
        auto entryNext = [&] {
            const Token& after = peekNext();
            const bool ends = after.type == TokenType::Comma || after.type == TokenType::LParen ||
                              after.type == TokenType::Semicolon || after.type == TokenType::RBrace;
            if (isKeyword(peek().type)) return ends || after.newlineBefore;
            if (!check(TokenType::Identifier)) return false;
            if (ends || after.newlineBefore) return true;
            const std::string& text = peek().text;
            if ((text == "get" || text == "set") && isName(after)) return false;
            return text != "op" && text != "static" && text != "enum" && text != "interface" && text != "object" &&
                   text != "data" && text != "private" && text != "internal" && text != "public";
        };
        while (entryNext()) {
            const Token& entryName = advance();
            EnumEntry entry;
            entry.line = entryName.line;
            entry.col = entryName.col;
            entry.name = entryName.text;
            for (const EnumEntry& earlier : s->entries)
                if (earlier.name == entry.name)
                    throw error(entryName, "duplicate entry '" + entry.name + "' in enum '" + s->name + "'");
            if (entry.name == "entries")
                throw error(entryName, "an entry called 'entries' would take the place of the list of all the entries, " +
                                           std::string("which every enum has under that name"));
            if (check(TokenType::LParen) && !peek().newlineBefore) {
                advance();
                HeaderGuard inside(*this, false);
                while (!check(TokenType::RParen)) {
                    entry.args.push_back(expression());
                    if (!match(TokenType::Comma)) break;
                }
                expect(TokenType::RParen, "')' after the arguments of the entry");
                if (entry.args.size() > 255) throw error(entryName, "a call cannot have more than 255 arguments");
            }
            s->entries.push_back(std::move(entry));
            if (!match(TokenType::Comma)) break;
        }
    }
    for (;;) {
        skipSemicolons();
        if (check(TokenType::RBrace) || check(TokenType::Eof)) break;

        classMember(*s, false);
    }
    expect(TokenType::RBrace, "'}' to end the class body");
    return s;
}

// What a data class gets without writing it. Equality, printing and coming apart are the
// VM's doing; `copy` is an ordinary method, added here unless the class has its own:
//   fun copy(a = this.a, b = this.b) => Name(a, b)
void Parser::addDataMembers(ClassStmt& s, const Token& at) {
    for (const auto& method : s.methods)
        if (!method->isStatic && method->name == "copy") return;

    auto place = [&](Node& n) {
        n.line = at.line;
        n.col = at.col;
    };
    auto copy = std::make_unique<FunctionDecl>();
    place(*copy);
    copy->name = "copy";
    copy->generated = true;
    auto call = std::make_unique<CallExpr>();
    place(*call);
    auto callee = std::make_unique<NameExpr>();
    place(*callee);
    callee->name = s.name;
    call->callee = std::move(callee);
    for (const HeaderParam& param : s.header) {
        copy->params.push_back(param.name);
        copy->paramTypes.emplace_back();
        Node position;
        place(position);
        copy->paramPositions.push_back(position);

        auto current = std::make_unique<MemberExpr>();
        place(*current);
        current->name = param.name;
        current->object = std::make_unique<ThisExpr>();
        place(*current->object);
        copy->paramDefaults.push_back(std::move(current));

        auto argument = std::make_unique<NameExpr>();
        place(*argument);
        argument->name = param.name;
        call->args.push_back(std::move(argument));
        call->names.emplace_back();
        call->spread.push_back(0);
    }
    auto result = std::make_unique<ReturnStmt>();
    place(*result);
    result->value = std::move(call);
    copy->body.push_back(std::move(result));
    s.methods.push_back(std::move(copy));
}

// The accessors written under a property: `get() => ...` and `set(value) { ... }`, in either
// order. Where the property has an initial value, and so a stored one, an accessor that is
// not written is the plain one.
void Parser::fieldAccessors(VarStmt& field) {
    const Token& at = peek();
    for (;;) {
        // `private set`: who may assign, with the setter written after it or with none, for
        // the plain one. (In front of `get` an access word says nothing new: a property is
        // read by whoever may use it.)
        const Token& third = toks_[p_ + 2 < toks_.size() ? p_ + 2 : toks_.size() - 1];
        if (check(TokenType::Identifier) && (nextWord("set") || nextWord("get")) && accessWord(peek().text) != 0) {
            if (nextWord("set")) field.setAccess = accessWord(peek().text);
            advance();
            if (third.type != TokenType::LParen) advance();
            continue;
        }
        const bool isGetter = word("get");
        if (!(isGetter || word("set")) || peekNext().type != TokenType::LParen) break;
        const Token& kind = advance();
        auto decl = functionRest(field.name, kind);
        decl->accessor = isGetter ? FunctionDecl::Accessor::Get : FunctionDecl::Accessor::Set;
        if (isGetter && !decl->params.empty()) throw error(kind, "the getter of '" + field.name + "' takes no parameters");
        if (!isGetter && decl->params.size() != 1)
            throw error(kind, "the setter of '" + field.name + "' takes exactly one parameter, the new value");
        if (!isGetter && field.isVal) throw error(kind, "'" + field.name + "' is a val, so it cannot have a setter");
        // As in Kotlin, the accessors have the type of the property unless they say otherwise.
        if (isGetter && !decl->returnType.present()) decl->returnType = field.type;
        if (!isGetter && !decl->paramTypes[0].present()) decl->paramTypes[0] = field.type;
        std::unique_ptr<FunctionDecl>& slot = isGetter ? field.getter : field.setter;
        if (slot) throw error(kind, std::string("'") + field.name + "' already has a " + (isGetter ? "getter" : "setter"));
        slot = std::move(decl);
    }
    if (!field.getter && !field.setter) return;
    if (!field.init) return;  // nothing stored: the compiler asks for what is missing

    auto place = [&](Node& n) {
        n.line = at.line;
        n.col = at.col;
    };
    auto stored = [&] {
        auto name = std::make_unique<NameExpr>();
        place(*name);
        name->name = "field";
        return name;
    };
    if (!field.getter) {
        // get() => field
        auto decl = std::make_unique<FunctionDecl>();
        place(*decl);
        decl->name = field.name;
        decl->accessor = FunctionDecl::Accessor::Get;
        auto result = std::make_unique<ReturnStmt>();
        place(*result);
        result->value = stored();
        decl->body.push_back(std::move(result));
        field.getter = std::move(decl);
    }
    if (!field.setter && !field.isVal) {
        // set(value) { field = value }
        auto decl = std::make_unique<FunctionDecl>();
        place(*decl);
        decl->name = field.name;
        decl->accessor = FunctionDecl::Accessor::Set;
        decl->params.push_back("value");
        decl->paramTypes.emplace_back();
        decl->paramDefaults.push_back(nullptr);
        Node position;
        place(position);
        decl->paramPositions.push_back(position);
        auto assign = std::make_unique<AssignExpr>();
        place(*assign);
        assign->target = stored();
        auto value = std::make_unique<NameExpr>();
        place(*value);
        value->name = "value";
        assign->value = std::move(value);
        auto statement = std::make_unique<ExprStmt>();
        place(*statement);
        statement->expr = std::move(assign);
        decl->body.push_back(std::move(statement));
        field.setter = std::move(decl);
    }
}

// One declaration in a class body. `inStatic` is set inside a `static { ... }` block, where
// every declaration is a static member.
void Parser::classMember(ClassStmt& s, bool inStatic) {
    skipAnnotations();
    if (!inStatic && word("static") && peekNext().type == TokenType::LBrace) {
        advance();
        advance();
        for (;;) {
            skipSemicolons();
            if (check(TokenType::RBrace) || check(TokenType::Eof)) break;
            classMember(s, true);
        }
        expect(TokenType::RBrace, "'}' to end the static block");
        return;
    }

    // Kotlin's modifiers on a member. `override`, `abstract` and the access words are noted;
    // `open`, `final` and `sealed` are refused.
    bool overrides = false;
    bool suspends = false;
    bool abstracts = false;
    uint8_t access = 0;
    for (; check(TokenType::Identifier) && !peekNext().newlineBefore &&
           (peek().text == "private" || peek().text == "internal" || peek().text == "public" || peek().text == "protected" ||
            peek().text == "override" || kotlinOnlyWord(peek().text) || peek().text == "abstract" ||
            peek().text == "suspend") &&
           (peekNext().type == TokenType::Val || peekNext().type == TokenType::Var || peekNext().type == TokenType::Fun ||
            peekNext().type == TokenType::Class || peekNext().type == TokenType::Identifier);
         advance()) {
        if (kotlinOnlyWord(peek().text)) throw error(peek(), notAstraWord(peek().text));
        overrides = overrides || peek().text == "override";
        suspends = suspends || peek().text == "suspend";
        abstracts = abstracts || peek().text == "abstract";
        if (accessWord(peek().text) != 0) access = accessWord(peek().text);
    }
    struct AbstractMember {
        bool& flag;
        bool saved;
        AbstractMember(bool& f, bool value) : flag(f), saved(f) { flag = value; }
        ~AbstractMember() { flag = saved; }
    } abstractMember(abstractMember_, abstracts);

    // init { ... }: runs when an object is made.
    if (word("init") && peekNext().type == TokenType::LBrace) {
        advance();
        AbstractMember body(abstractMember_, false);
        s.initBlocks.emplace_back(s.fields.size(), block());
        return;
    }

    // A class, enum, interface or object declared inside this one.
    const bool wordDeclares = check(TokenType::Identifier) && peekNext().type == TokenType::Identifier &&
                              !peekNext().newlineBefore &&
                              (peek().text == "enum" || peek().text == "interface" || peek().text == "object" || peek().text == "data");
    if (check(TokenType::Class) || wordDeclares) {
        AbstractMember body(abstractMember_, false);
        StmtPtr declared = declaration();
        s.nested.emplace_back(static_cast<ClassStmt*>(declared.release()));
        if (abstracts) s.nested.back()->isAbstract = true;
        s.nested.back()->access = access;
        return;
    }

    // `get` and `set` are only special here, so they stay usable as ordinary names.
    auto accessorAt = [&](size_t at) {
        const Token& word = toks_[at < toks_.size() ? at : toks_.size() - 1];
        const Token& after = toks_[at + 1 < toks_.size() ? at + 1 : toks_.size() - 1];
        return word.type == TokenType::Identifier && (word.text == "get" || word.text == "set") && isName(after) &&
               !after.newlineBefore;
    };

    bool isStatic = inStatic;
    if (word("static") &&
        (peekNext().type == TokenType::Val || peekNext().type == TokenType::Var || accessorAt(p_ + 1) ||
         (peekNext().type == TokenType::Identifier && peekNext().text == "const"))) {
        advance();
        isStatic = true;
    }

    // A constant belongs to an object or to the class's static code: one value for all.
    if (word("const") && !peekNext().newlineBefore &&
        (peekNext().type == TokenType::Identifier || peekNext().type == TokenType::Val || peekNext().type == TokenType::Var)) {
        const Token& keyword = peek();
        auto field = constDeclaration(true);
        field->isOverride = overrides;
        field->access = access;
        if (abstracts) throw error(keyword, "const '" + field->name + "' has its value here, so it is not abstract");
        if (isStatic) {
            s.staticFields.push_back(std::move(field));
        } else if (s.isObject) {
            s.fields.push_back(std::move(field));
        } else {
            throw error(keyword, "const '" + field->name + "' is one value for the whole class, so it goes in its static { } "
                                 "block (or is written 'static const'), or in an object");
        }
        return;
    }

    if (check(TokenType::Val) || check(TokenType::Var)) {
        const Token& keyword = peek();
        auto field = varDeclaration(true);
        field->isOverride = overrides;
        field->isAbstract = abstracts;
        field->access = access;
        if (abstracts && field->init) throw error(keyword, "'" + field->name + "' is abstract, so it has no value here");
        fieldAccessors(*field);
        if (!isStatic) {
            s.fields.push_back(std::move(field));
            return;
        }
        if (field->isVal && !field->init)
            throw error(keyword, "static val '" + field->name + "' must be given its value where it is declared");
        s.staticFields.push_back(std::move(field));
    } else if (accessorAt(p_)) {
        auto property = accessor();
        property->isStatic = isStatic;
        property->isOverride = overrides;
        property->isSuspend = suspends;
        property->access = access;
        s.methods.push_back(std::move(property));
    } else {
        if (inStatic && word("op")) throw error(peek(), "operators cannot be static");
        auto member = method();
        if (inStatic) member->isStatic = true;
        member->isOverride = overrides;
        member->isSuspend = suspends;
        member->access = access;
        s.methods.push_back(std::move(member));
    }
}

// A property: `get name() : type { ... }` or `set name(value : type) { ... }`.
std::unique_ptr<FunctionDecl> Parser::accessor() {
    const Token& kind = advance();
    bool isGetter = kind.text == "get";
    const Token& name = expectName("a property name");

    auto decl = functionRest(name.text, kind);
    decl->accessor = isGetter ? FunctionDecl::Accessor::Get : FunctionDecl::Accessor::Set;
    if (isGetter && !decl->params.empty())
        throw error(name, "the getter of '" + name.text + "' takes no parameters");
    if (!isGetter && decl->params.size() != 1)
        throw error(name, "the setter of '" + name.text + "' takes exactly one parameter, the new value");
    if (!isGetter && decl->paramDefaults[0])
        throw error(name, "the parameter of a setter cannot have a default value");
    if (!isGetter && decl->returnType.present())
        throw error(name, "a setter cannot declare a return type");
    return decl;
}

std::unique_ptr<FunctionDecl> Parser::method() {
    // fun name(...)   static fun name(...)   op +(...)
    const Token& start = peek();
    // `static` and `op` are words with a meaning only here, at the start of a member.
    if (word("static") && nextWord("op")) throw error(start, "operators cannot be static");
    bool isStatic = word("static") && peekNext().type == TokenType::Fun;
    if (isStatic) advance();
    bool isOperator = word("op") && peekNext().type != TokenType::LParen;
    if (isOperator) advance();
    if (isStatic && isOperator) throw error(start, "operators cannot be static");

    // An operator is introduced by `op` alone; everything else needs `fun`.
    if (!isOperator && !match(TokenType::Fun)) {
        const Token& found = peek();
        if (found.type == TokenType::Identifier && found.text == "operator") {
            throw error(found, "operators are declared with 'op' (op +(other) ...)");
        } else if (found.type == TokenType::Identifier) {
            throw error(found, std::string("methods are declared with 'fun' (") + (isStatic ? "static " : "") + "fun " +
                                   found.text + "(...))");
        } else {
            throw error(found, "expected a field ('val' or 'var'), a method ('fun') or an operator ('op') in the " +
                                   std::string("class body, found ") + describe(found));
        }
    }
    if (isOperator && check(TokenType::Fun))
        throw error(peek(), "an operator is declared with 'op' alone, without 'fun' (op +(other) ...)");

    if (isOperator) {
        const Token& opToken = peek();
        std::string op = operatorName();
        auto decl = functionRest(op, start);
        size_t arity = decl->params.size();
        if (op == "-" && arity == 0) {
            decl->name = "unary-";
        } else if (op == "~") {
            if (arity != 0) throw error(opToken, "op ~ takes no parameters");
            decl->name = "unary~";
        } else if (op == "[]=") {
            if (arity != 2) throw error(opToken, "op []= takes exactly two parameters (index, value)");
        } else if (arity != 1) {
            throw error(opToken, "op " + op + " takes exactly one parameter");
        }
        return decl;
    }

    // `fun <T> wrap(item : T) : Box<T>`: a method's own type parameters.
    std::vector<TypeParam> typeParams;
    if (check(TokenType::Lt)) typeParams = typeParameters();
    TypeParamScope typeParamsInReach(*this, typeParams);
    const Token& name = expectName("a method name");
    auto decl = functionRest(name.text, start);
    decl->isStatic = isStatic;
    decl->typeParams = std::move(typeParams);
    return decl;
}

std::string Parser::operatorName() {
    const Token& t = advance();
    switch (t.type) {
        case TokenType::Plus:
        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent:
        case TokenType::EqEq:
        case TokenType::Lt:
        case TokenType::LtEq:
        case TokenType::Gt:
        case TokenType::GtEq:
        case TokenType::Shl:
        case TokenType::Shr:
        case TokenType::Amp:
        case TokenType::Pipe:
        case TokenType::Caret:
        case TokenType::Tilde:
            return t.text;
        case TokenType::LBracket:
            expect(TokenType::RBracket, "']'");
            return match(TokenType::Eq) ? "[]=" : "[]";
        default:
            throw error(t, describe(t) + " is not an overloadable operator");
    }
}

// Parses "(params) { body }" or "(params) => expr".
std::unique_ptr<FunctionDecl> Parser::functionRest(std::string name, const Token& start) {
    auto decl = std::make_unique<FunctionDecl>();
    decl->line = start.line;
    decl->col = start.col;
    decl->name = std::move(name);

    expect(TokenType::LParen, "'(' before the parameter list");
    while (!check(TokenType::RParen)) {
        // `vararg items` takes any number of arguments, as a list.
        const bool vararg = check(TokenType::Identifier) && peek().text == "vararg" && peekNext().type == TokenType::Identifier;
        if (vararg) {
            if (decl->varargIndex >= 0) throw error(peek(), "a function can have only one vararg parameter");
            advance();
            decl->varargIndex = static_cast<int>(decl->params.size());
        }
        const Token& param = expect(TokenType::Identifier, "a parameter name");
        if (std::find(decl->params.begin(), decl->params.end(), param.text) != decl->params.end())
            throw error(param, "duplicate parameter '" + param.text + "'");
        decl->params.push_back(param.text);
        Node position;
        position.line = param.line;
        position.col = param.col;
        decl->paramPositions.push_back(position);
        TypeAnnotation type = optionalType();
        // A type written on a vararg parameter is that of each argument; the parameter is a list.
        decl->paramTypes.push_back(vararg ? TypeAnnotation() : type);
        if (vararg) decl->varargType = type;
        if (vararg && check(TokenType::Eq)) throw error(peek(), "a vararg parameter cannot have a default value");
        if (match(TokenType::Eq)) decl->paramDefaults.push_back(expression());
        else decl->paramDefaults.push_back(nullptr);
        if (!match(TokenType::Comma)) break;
    }
    expect(TokenType::RParen, "')' after the parameter list");
    if (decl->params.size() > 255) throw error(start, "a function cannot have more than 255 parameters");
    decl->returnType = optionalType();

    decl->bodyFrom = p_;
    if (match(TokenType::Arrow)) {
        auto ret = node<ReturnStmt>(peek());
        ret->value = expression();
        decl->body.push_back(std::move(ret));
    } else if ((inInterface_ || abstractMember_ || externalBody_) && !check(TokenType::LBrace)) {
        decl->isAbstract = true;  // for the implementing class to write
    } else {
        decl->body = std::move(block()->statements);
    }
    decl->bodyTo = p_;
    return decl;
}

std::unique_ptr<BlockStmt> Parser::block() {
    const Token& open = expect(TokenType::LBrace, "'{'");
    auto b = node<BlockStmt>(open);
    for (;;) {
        skipSemicolons();
        if (check(TokenType::RBrace) || check(TokenType::Eof)) break;
        b->statements.push_back(declaration());
    }
    expect(TokenType::RBrace, "'}'");
    return b;
}

// `@name` straight after break or continue: the loop meant. Empty when there is none.
std::string Parser::jumpLabel() {
    if (!check(TokenType::At) || !adjacentToPrevious()) return "";
    advance();
    return expect(TokenType::Identifier, "the name of a loop after '@'").text;
}

StmtPtr Parser::statement() {
    const Token& t = peek();

    // name@ for ..., name@ while ..., name@ do ...: a loop that break@name and continue@name reach.
    if (t.type == TokenType::Identifier && peekNext().type == TokenType::At && p_ + 2 < toks_.size()) {
        const Token& third = toks_[p_ + 2];
        const bool loop = third.type == TokenType::For || third.type == TokenType::While ||
                          (third.type == TokenType::Identifier && third.text == "do");
        if (loop && peekNext().col == t.col + static_cast<int>(t.text.size())) {
            advance();
            advance();
            StmtPtr labelled = statement();
            if (labelled->kind == StmtKind::While) static_cast<WhileStmt&>(*labelled).label = t.text;
            else if (labelled->kind == StmtKind::ForIn) static_cast<ForInStmt&>(*labelled).label = t.text;
            return labelled;
        }
    }

    // do { ... } while condition
    if (word("do") && peekNext().type == TokenType::LBrace) {
        advance();
        auto s = node<WhileStmt>(t);
        s->doWhile = true;
        s->body = block();
        expect(TokenType::While, "'while' and the condition after the block of a 'do'");
        s->condition = expression();
        endStatement();
        return s;
    }

    switch (t.type) {
        case TokenType::LBrace:
            return block();

        case TokenType::If:
            return ifStatement();

        case TokenType::While: {
            advance();
            auto s = node<WhileStmt>(t);
            {
                HeaderGuard header(*this, true);
                s->condition = expression();
            }
            s->body = block();
            rejectLambdaInHeader("while");
            return s;
        }

        case TokenType::For: {
            advance();
            auto s = node<ForInStmt>(t);
            // for x in ..., for (x in ...), for (a, b) in ..., for ((a, b) in ...)
            bool paren = false;
            if (check(TokenType::LParen)) {
                const Token& second = toks_[p_ + 2 < toks_.size() ? p_ + 2 : toks_.size() - 1];
                if (peekNext().type == TokenType::LParen) {
                    advance();
                    paren = true;
                    s->pattern = destructuringPattern();
                } else if (second.type == TokenType::Comma || second.type == TokenType::Colon) {
                    s->pattern = destructuringPattern();
                } else {
                    advance();
                    paren = true;
                }
            }
            if (s->pattern.empty()) s->variable = expect(TokenType::Identifier, "a loop variable name").text;
            expect(TokenType::In, "'in'");
            {
                // Inside `for (x in items)` the ')' ends the expression, so a lambda may follow a call.
                HeaderGuard header(*this, !paren);
                s->iterable = expression();
            }
            if (paren) expect(TokenType::RParen, "')'");
            s->body = block();
            if (!paren) rejectLambdaInHeader("for");
            return s;
        }

        case TokenType::Return: {
            advance();
            auto s = node<ReturnStmt>(t);
            if (check(TokenType::At) && adjacentToPrevious()) {
                advance();
                s->label = expect(TokenType::Identifier, "the name of a lambda after 'return@'").text;
            }
            if (!check(TokenType::Semicolon) && !check(TokenType::RBrace) && !check(TokenType::Eof) &&
                !peek().newlineBefore)
                s->value = expression();
            endStatement();
            return s;
        }

        case TokenType::Break: {
            advance();
            auto s = node<BreakStmt>(t);
            s->label = jumpLabel();
            endStatement();
            return s;
        }

        case TokenType::Continue: {
            advance();
            auto s = node<ContinueStmt>(t);
            s->label = jumpLabel();
            endStatement();
            return s;
        }

        case TokenType::Throw: {
            advance();
            auto s = node<ThrowStmt>(t);
            s->value = expression();
            endStatement();
            return s;
        }

        case TokenType::Try: {
            advance();
            auto s = node<TryStmt>(t);
            s->body = block();
            // `finally` is a word only here, so it stays usable as a name.
            auto finallyNext = [&] { return check(TokenType::Identifier) && peek().text == "finally"; };
            if (!finallyNext() && !check(TokenType::Catch))
                throw error(peek(), "expected 'catch' or 'finally' after the try block, found " + describe(peek()));
            // Several catches may follow, each for errors of one class: catch (e : NotFound) { }.
            // One without a class catches everything, so it has to be the last.
            while (check(TokenType::Catch)) {
                const Token& kw = advance();
                if (!s->catches.empty() && !s->catches.back().type.present())
                    throw error(kw, "the catch before this one catches every error, so this one can never run");
                CatchClause clause;
                if (match(TokenType::LParen)) {
                    clause.variable = expect(TokenType::Identifier, "an exception variable name").text;
                    clause.hasVariable = true;
                    clause.type = optionalType();
                    expect(TokenType::RParen, "')'");
                } else if (check(TokenType::Identifier) && !finallyNext()) {
                    clause.variable = advance().text;
                    clause.hasVariable = true;
                }
                clause.body = block();
                s->catches.push_back(std::move(clause));
            }
            if (finallyNext()) {
                advance();
                s->finalizer = block();
            }
            return s;
        }

        case TokenType::Import: {
            advance();
            auto s = node<ImportStmt>(t);
            // import jade            the folder (or file) jade: its names are written jade.Name
            // import jade.chart      jade/chart: jade.chart.Name
            // import jade.*          jade's names can be written bare, too
            // import jade as ui      ui.Name
            // A path that is not made of plain names goes in quotes: import "../shared/util" as util
            if (check(TokenType::String)) {
                s->path = advance().stringValue;
                // The name it goes by is the path with dots, when that makes one.
                std::string name = s->path;
                if (name.size() > 6 && name.compare(name.size() - 6, 6, ".astra") == 0) name.resize(name.size() - 6);
                bool usable = !name.empty();
                bool startOfPart = true;
                for (char& c : name) {
                    if (c == '/' || c == '\\') {
                        usable = usable && !startOfPart;
                        c = '.';
                        startOfPart = true;
                        continue;
                    }
                    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
                    usable = usable && (letter || (!startOfPart && c >= '0' && c <= '9'));
                    startOfPart = false;
                }
                if (usable && !startOfPart) s->alias = name;
            } else {
                const Token& first = expect(TokenType::Identifier, "the name of a folder or file to import");
                s->path = first.text;
                s->alias = first.text;
                while (check(TokenType::Dot) && peekNext().type == TokenType::Identifier) {
                    advance();
                    const std::string& part = advance().text;
                    s->path += "/" + part;
                    s->alias += "." + part;
                }
            }
            if (check(TokenType::Dot) && peekNext().type == TokenType::Star) {
                advance();
                advance();
                s->all = true;
            }
            if (check(TokenType::Identifier) && peek().text == "as" && !peek().newlineBefore) {
                advance();
                s->alias = expect(TokenType::Identifier, "a name for the import after 'as'").text;
            }
            endStatement();
            return s;
        }

        default: {
            auto s = node<ExprStmt>(t);
            s->expr = expression();
            endStatement();
            return s;
        }
    }
}

// In `if items.any { it > 1 } { ... }` the first '{' was taken as the body, because in a
// condition a '{' always is. What follows the body on the same line gives that away.
void Parser::rejectLambdaInHeader(const char* keyword) {
    const Token& next = peek();
    if (next.newlineBefore || (next.type != TokenType::LBrace && next.type != TokenType::Dot)) return;
    throw error(next, std::string("the '{' before this was read as the body of the '") + keyword +
                          "'; to pass a lambda to a call in its condition, put the condition in parentheses");
}

StmtPtr Parser::ifStatement() {
    const Token& kw = advance();
    auto s = node<IfStmt>(kw);
    {
        HeaderGuard header(*this, true);
        s->condition = expression();
    }
    s->then = block();
    rejectLambdaInHeader("if");
    if (match(TokenType::Else)) {
        if (check(TokenType::If)) {
            DepthGuard guard(*this);
            s->otherwise = ifStatement();
        } else {
            s->otherwise = block();
        }
    }
    return s;
}

// ---- Expressions ----

ExprPtr Parser::expression() {
    DepthGuard guard(*this);
    return assignment();
}

ExprPtr Parser::assignment() {
    ExprPtr left = ternary();

    switch (peek().type) {
        case TokenType::Eq:
        case TokenType::PlusEq:
        case TokenType::MinusEq:
        case TokenType::StarEq:
        case TokenType::SlashEq:
        case TokenType::PercentEq:
        case TokenType::ShlEq:
        case TokenType::ShrEq:
        case TokenType::AmpEq:
        case TokenType::PipeEq:
        case TokenType::CaretEq: {
            const Token& op = advance();
            if (left->kind != ExprKind::Name && left->kind != ExprKind::Member && left->kind != ExprKind::Index)
                throw error(op, "invalid assignment target");
            auto e = node<AssignExpr>(op);
            e->op = op.type;
            e->nullSafe = !op.text.empty() && op.text.back() == '?';
            e->target = std::move(left);
            e->value = expression();
            return e;
        }
        default:
            return left;
    }
}

ExprPtr Parser::ternary() {
    ExprPtr condition = logicalOr();
    if (!check(TokenType::Question)) return condition;

    const Token& q = advance();
    auto e = node<TernaryExpr>(q);
    e->condition = std::move(condition);
    e->then = expression();
    expect(TokenType::Colon, "':' in conditional expression");
    e->otherwise = expression();
    return e;
}

ExprPtr Parser::logicalOr() {
    ExprPtr left = logicalAnd();
    while (check(TokenType::Or)) {
        const Token& op = advance();
        auto e = node<LogicalExpr>(op);
        e->isAnd = false;
        e->left = std::move(left);
        e->right = logicalAnd();
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::logicalAnd() {
    ExprPtr left = equality();
    while (check(TokenType::And)) {
        const Token& op = advance();
        auto e = node<LogicalExpr>(op);
        e->isAnd = true;
        e->left = std::move(left);
        e->right = equality();
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::equality() {
    ExprPtr left = comparison();
    while (check(TokenType::EqEq) || check(TokenType::BangEq) || check(TokenType::EqEqEq) || check(TokenType::BangEqEq)) {
        const Token& op = advance();
        ExprPtr right = comparison();
        left = binary(op, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::comparison() {
    ExprPtr left = elvis();
    for (;;) {
        TokenType t = peek().type;
        if (t == TokenType::Lt || t == TokenType::LtEq || t == TokenType::Gt || t == TokenType::GtEq) {
            const Token& op = advance();
            ExprPtr right = elvis();
            left = binary(op, std::move(left), std::move(right));
        } else if (peek().newlineBefore) {
            // `is` and `in` at the start of a line begin something new: a branch of a `when`.
            return left;
        } else if (t == TokenType::Is || t == TokenType::NotIs) {
            const Token& op = advance();
            auto e = node<IsExpr>(op);
            e->value = std::move(left);
            e->negated = t == TokenType::NotIs;
            e->type = checkedType();
            left = std::move(e);
        } else if (t == TokenType::In || t == TokenType::NotIn) {
            const Token& op = advance();
            auto e = node<InExpr>(op);
            e->value = std::move(left);
            e->negated = t == TokenType::NotIn;
            e->container = elvis();
            left = std::move(e);
        } else {
            return left;
        }
    }
}

// `a ?: b` is a, unless a is null. It binds tighter than a comparison and looser than
// arithmetic, so `count ?: 0 > limit` compares and `total ?: a + b` adds first.
ExprPtr Parser::elvis() {
    ExprPtr left = pairing();
    while (check(TokenType::Elvis)) {
        const Token& op = advance();
        auto e = node<ElvisExpr>(op);
        e->left = std::move(left);
        e->right = pairing();
        left = std::move(e);
    }
    return left;
}

// `a to b` is Pair(a, b). `to` is only a word with a meaning here, between two values on one
// line; anywhere else it is an ordinary name.
ExprPtr Parser::pairing() {
    ExprPtr left = range();
    while (word("to") && !peek().newlineBefore) {
        const Token& op = advance();
        auto call = node<CallExpr>(op);
        auto pair = node<NameExpr>(op);
        pair->name = "Pair";
        call->callee = std::move(pair);
        call->args.push_back(std::move(left));
        call->args.push_back(range());
        call->names.resize(2);
        call->spread.resize(2, 0);
        left = std::move(call);
    }
    return left;
}

ExprPtr Parser::range() {
    ExprPtr left = bitOr();
    // a..b runs from a to b; a..<b stops before b. Kotlin's `a until b` is refused in favour
    // of a..<b. `a downTo b` counts down to b, and `step n` after any of them takes every nth.
    if (word("until") && !peek().newlineBefore)
        throw error(peek(), "'until' is not a keyword; a range that stops before its end is written a..<b");
    const bool downTo = word("downTo") && !peek().newlineBefore;
    if (check(TokenType::DotDot) || check(TokenType::DotDotLt) || downTo) {
        const Token& op = advance();
        auto e = node<RangeExpr>(op);
        e->inclusive = op.type == TokenType::DotDot || downTo;
        e->descending = downTo;
        e->start = std::move(left);
        e->end = bitOr();
        if (word("step") && !peek().newlineBefore) {
            advance();
            e->step = bitOr();
        }
        return e;
    }
    return left;
}

ExprPtr Parser::bitOr() {
    ExprPtr left = bitXor();
    while (check(TokenType::Pipe)) {
        const Token& op = advance();
        ExprPtr right = bitXor();
        left = binary(op, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::bitXor() {
    ExprPtr left = bitAnd();
    while (check(TokenType::Caret)) {
        const Token& op = advance();
        ExprPtr right = bitAnd();
        left = binary(op, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::bitAnd() {
    ExprPtr left = shift();
    while (check(TokenType::Amp)) {
        const Token& op = advance();
        ExprPtr right = shift();
        left = binary(op, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::shift() {
    ExprPtr left = additive();
    while (check(TokenType::Shl) || check(TokenType::Shr)) {
        const Token& op = advance();
        ExprPtr right = additive();
        left = binary(op, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::additive() {
    ExprPtr left = multiplicative();
    while (check(TokenType::Plus) || (check(TokenType::Minus) && !peek().newlineBefore)) {
        const Token& op = advance();
        ExprPtr right = multiplicative();
        left = binary(op, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::multiplicative() {
    ExprPtr left = cast();
    while (check(TokenType::Star) || check(TokenType::Slash) || check(TokenType::Percent)) {
        const Token& op = advance();
        ExprPtr right = cast();
        left = binary(op, std::move(left), std::move(right));
    }
    return left;
}

// `value as Type` and `value as? Type`. `as` is only a word with a meaning here, after a value
// on the same line, so it can still be used as a name.
ExprPtr Parser::cast() {
    ExprPtr left = unary();
    while (check(TokenType::Identifier) && peek().text == "as" && !peek().newlineBefore) {
        const Token& op = advance();
        auto e = node<CastExpr>(op);
        e->value = std::move(left);
        if (check(TokenType::Question) && adjacentToPrevious()) {
            advance();
            e->safe = true;
        }
        e->type = checkedType();
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::unary() {
    if (check(TokenType::Bang) || check(TokenType::Minus) || check(TokenType::Tilde)) {
        DepthGuard guard(*this);
        const Token& op = advance();
        auto e = node<UnaryExpr>(op);
        e->op = op.type;
        e->operand = unary();
        return e;
    }
    if (check(TokenType::BangBang)) {
        // In front of a value, `!!` is simply `!` twice.
        DepthGuard guard(*this);
        const Token& op = advance();
        auto outer = node<UnaryExpr>(op);
        auto inner = node<UnaryExpr>(op);
        outer->op = inner->op = TokenType::Bang;
        inner->operand = unary();
        outer->operand = std::move(inner);
        return outer;
    }
    if (check(TokenType::PlusPlus) || check(TokenType::MinusMinus)) {
        // There is no ++x or --x: the operator goes after its target, as a statement.
        const Token& op = peek();
        std::string hint = op.type == TokenType::MinusMinus ? "; to negate twice, write -(-x)" : "";
        throw error(op, "'" + op.text + "' goes after what it changes, as a statement of its own (count" + op.text + ")" + hint);
    }
    return postfix();
}

ExprPtr Parser::postfix() {
    ExprPtr e = primary();
    // A lambda passed to a function can be left with `return@` and that function's name.
    auto labelLambdas = [](CallExpr& call) {
        std::string name;
        if (call.callee->kind == ExprKind::Name) name = static_cast<const NameExpr&>(*call.callee).name;
        else if (call.callee->kind == ExprKind::Member) name = static_cast<const MemberExpr&>(*call.callee).name;
        if (name.empty()) return;
        for (ExprPtr& arg : call.args) {
            if (arg->kind != ExprKind::Function) continue;
            FunctionDecl& fn = *static_cast<FunctionExpr&>(*arg).fn;
            if (fn.isLambda) fn.calleeLabel = name;
        }
    };
    for (;;) {
        // `first<int>(xs)`: type arguments written before the arguments.
        std::vector<TypeAnnotation> typeArgs;
        if ((e->kind == ExprKind::Name || e->kind == ExprKind::Member) && typeArgumentsBeforeCall(typeArgs)) {
            if (check(TokenType::LBrace)) {
                // `build<Grid> { ... }`: the lambda is the only argument.
                const Token& open = advance();
                auto call = node<CallExpr>(open);
                call->names.emplace_back();
                call->spread.push_back(0);
                call->trailingLambda = true;
                call->args.push_back(lambdaLiteral(open, lambdaHasParams()));
                call->callee = std::move(e);
                call->typeArgs = std::move(typeArgs);
                labelLambdas(*call);
                e = std::move(call);
                continue;
            }
        }
        const Token& t = peek();
        if (t.type == TokenType::LParen && !t.newlineBefore) {
            advance();
            auto call = node<CallExpr>(t);
            call->typeArgs = std::move(typeArgs);
            HeaderGuard inside(*this, false);
            while (!check(TokenType::RParen)) {
                // `name = value` passes an argument by name.
                if (isName(peek()) && peekNext().type == TokenType::Eq) {
                    const Token& argName = advance();
                    advance();
                    for (const std::string& earlier : call->names)
                        if (earlier == argName.text) throw error(argName, "argument '" + argName.text + "' is given more than once");
                    call->names.push_back(argName.text);
                    call->namedCount++;
                    call->spread.push_back(0);
                } else {
                    if (call->namedCount > 0)
                        throw error(peek(), "positional arguments must come before named ones");
                    call->names.emplace_back();
                    // `*list` hands over the elements of a list as separate arguments.
                    const bool spread = match(TokenType::Star);
                    call->spread.push_back(spread ? 1 : 0);
                    call->hasSpread = call->hasSpread || spread;
                }
                call->args.push_back(expression());
                if (!match(TokenType::Comma)) break;
            }
            expect(TokenType::RParen, "')' after arguments");
            inside.restore();
            // A lambda written straight after the ')' is the call's last argument.
            if (trailingLambdaNext()) {
                const Token& open = advance();
                call->names.emplace_back();
                call->spread.push_back(0);
                call->trailingLambda = true;
                call->args.push_back(lambdaLiteral(open, lambdaHasParams()));
            }
            if (call->args.size() > 255) throw error(t, "a call cannot have more than 255 arguments");
            call->callee = std::move(e);
            labelLambdas(*call);
            e = std::move(call);
        } else if (t.type == TokenType::LBrace && trailingLambdaNext() &&
                   (e->kind == ExprKind::Name || e->kind == ExprKind::Member || e->kind == ExprKind::Super)) {
            // `grid { ... }`, `items.forEach { ... }`: a call whose only argument is the lambda.
            advance();
            auto call = node<CallExpr>(t);
            call->names.emplace_back();
            call->spread.push_back(0);
            call->trailingLambda = true;
            call->args.push_back(lambdaLiteral(t, lambdaHasParams()));
            call->callee = std::move(e);
            labelLambdas(*call);
            e = std::move(call);
        } else if (t.type == TokenType::LBracket && !t.newlineBefore) {
            advance();
            auto index = node<IndexExpr>(t);
            HeaderGuard inside(*this, false);
            index->index = expression();
            // `counts[word, default = 0]`: a start value for a key that is not there (L118).
            if (check(TokenType::Comma) && peekNext().type == TokenType::Identifier && peekNext().text == "default") {
                advance();
                advance();
                expect(TokenType::Eq, "'=' after 'default' in an index");
                index->defaultValue = expression();
            }
            expect(TokenType::RBracket, "']' after index");
            index->object = std::move(e);
            e = std::move(index);
        } else if ((t.type == TokenType::PlusPlus || t.type == TokenType::MinusMinus) && !t.newlineBefore) {
            advance();
            if (e->kind != ExprKind::Name && e->kind != ExprKind::Member && e->kind != ExprKind::Index)
                throw error(t, "'" + t.text + "' needs a variable, a field or an element to change");
            auto change = node<IncDecExpr>(t);
            change->increment = t.type == TokenType::PlusPlus;
            change->nullSafe = t.text.size() == 3;
            change->target = std::move(e);
            e = std::move(change);
        } else if (t.type == TokenType::Dot || t.type == TokenType::QuestionDot) {
            advance();
            const Token& name = expectName(t.type == TokenType::Dot ? "a member name after '.'" : "a member name after '?.'");
            auto member = node<MemberExpr>(name);
            member->name = name.text;
            member->safe = t.type == TokenType::QuestionDot;
            member->object = std::move(e);
            e = std::move(member);
        } else if (t.type == TokenType::BangBang && !t.newlineBefore) {
            // `value!!` insists that the value is not null.
            advance();
            auto assertion = node<UnaryExpr>(t);
            assertion->op = TokenType::BangBang;
            assertion->operand = std::move(e);
            e = std::move(assertion);
        } else {
            return e;
        }
    }
}

ExprPtr Parser::primary() {
    const Token& t = advance();
    switch (t.type) {
        case TokenType::Int: {
            auto e = node<LiteralExpr>(t);
            e->type = LiteralExpr::Type::Int;
            e->intValue = t.intValue;
            return e;
        }
        case TokenType::Float: {
            auto e = node<LiteralExpr>(t);
            e->type = LiteralExpr::Type::Float;
            e->floatValue = t.floatValue;
            return e;
        }
        case TokenType::String: {
            auto e = node<LiteralExpr>(t);
            e->type = LiteralExpr::Type::String;
            e->stringValue = t.stringValue;
            return e;
        }
        case TokenType::True:
        case TokenType::False: {
            auto e = node<LiteralExpr>(t);
            e->type = LiteralExpr::Type::Bool;
            e->boolValue = t.type == TokenType::True;
            return e;
        }
        case TokenType::Null:
            return node<LiteralExpr>(t);

        case TokenType::Identifier: {
            if (t.text == "object" && anonymousObjectNext()) {
                // object : Listener { ... }: a class with no name and the one object of it.
                DepthGuard guard(*this);
                HeaderGuard body(*this, false);
                auto e = node<ObjectExpr>(t);
                StmtPtr declared = classDeclaration(false, false, &t);
                e->cls.reset(static_cast<ClassStmt*>(declared.release()));
                return e;
            }
            if (check(TokenType::At) && adjacentToPrevious() && peekNext().type == TokenType::LBrace) {
                // name@ { ... }: a lambda that `return@name` leaves.
                advance();
                const Token& open = advance();
                ExprPtr lambda = lambdaLiteral(open, lambdaHasParams());
                static_cast<FunctionExpr&>(*lambda).fn->label = t.text;
                return lambda;
            }
            auto e = node<NameExpr>(t);
            e->name = t.text;
            return e;
        }

        case TokenType::This:
            return node<ThisExpr>(t);

        case TokenType::InterpString: {
            auto e = node<InterpExpr>(t);
            for (const InterpPart& part : *t.parts) {
                if (part.isExpr) {
                    Parser sub(part.tokens, file_);
                    sub.depth_ = depth_;
                    e->parts.push_back(sub.parseSingleExpression());
                } else {
                    auto literal = node<LiteralExpr>(t);
                    literal->type = LiteralExpr::Type::String;
                    literal->stringValue = part.text;
                    e->parts.push_back(std::move(literal));
                }
            }
            return e;
        }

        case TokenType::Super: {
            expect(TokenType::Dot, "'.' after 'super'");
            const Token& name = expectName("a superclass method name");
            auto e = node<SuperExpr>(t);
            e->method = name.text;
            return e;
        }

        case TokenType::LParen: {
            HeaderGuard inside(*this, false);
            ExprPtr e = expression();
            expect(TokenType::RParen, "')'");
            return e;
        }

        case TokenType::LBracket: {
            HeaderGuard inside(*this, false);
            auto e = node<ListExpr>(t);
            while (!check(TokenType::RBracket)) {
                e->items.push_back(expression());
                if (!match(TokenType::Comma)) break;
            }
            expect(TokenType::RBracket, "']' to end the list");
            return e;
        }

        case TokenType::LBrace:
            return braceExpression(t);

        case TokenType::Fun: {
            auto e = node<FunctionExpr>(t);
            e->fn = functionRest("<lambda>", t);
            return e;
        }

        case TokenType::Return:
        case TokenType::Throw:
        case TokenType::Break:
        case TokenType::Continue:
            return jumpExpression(t);

        case TokenType::When:
            return whenExpression(t);

        case TokenType::If:
        case TokenType::Try: {
            // As a value: the statement is read as usual and gives what its block came to.
            DepthGuard guard(*this);
            auto e = node<StatementExpr>(t);
            p_--;
            e->statement = statement();
            return e;
        }

        default:
            throw error(t, "expected an expression, found " + describe(t));
    }
}

// `return`, `throw`, `break` and `continue` can stand where a value is expected, most often
// after `?:`. The keyword has already been read.
ExprPtr Parser::jumpExpression(const Token& keyword) {
    auto e = node<JumpExpr>(keyword);
    switch (keyword.type) {
        case TokenType::Return: {
            auto s = node<ReturnStmt>(keyword);
            if (check(TokenType::At) && adjacentToPrevious()) {
                advance();
                s->label = expect(TokenType::Identifier, "the name of a lambda after 'return@'").text;
            }
            if (startsExpression()) s->value = expression();
            e->jump = std::move(s);
            break;
        }
        case TokenType::Throw: {
            auto s = node<ThrowStmt>(keyword);
            s->value = expression();
            e->jump = std::move(s);
            break;
        }
        case TokenType::Break: {
            auto s = node<BreakStmt>(keyword);
            s->label = jumpLabel();
            e->jump = std::move(s);
            break;
        }
        default: {
            auto s = node<ContinueStmt>(keyword);
            s->label = jumpLabel();
            e->jump = std::move(s);
            break;
        }
    }
    return e;
}

// `when (subject) { ... }`, `when subject { ... }` or `when { ... }`. The keyword has been read.
ExprPtr Parser::whenExpression(const Token& keyword) {
    DepthGuard guard(*this);
    auto e = node<WhenExpr>(keyword);
    if (!check(TokenType::LBrace)) {
        if (check(TokenType::LParen) && peekNext().type == TokenType::Val) {
            // `when (val shape = next())` names the subject for the branches.
            advance();
            advance();
            e->subjectName = expect(TokenType::Identifier, "a name for the subject of 'when'").text;
            expect(TokenType::Eq, "'=' after the name");
            HeaderGuard inside(*this, false);
            e->subject = expression();
            expect(TokenType::RParen, "')'");
        } else {
            HeaderGuard header(*this, true);
            e->subject = expression();
        }
    }
    expect(TokenType::LBrace, "'{' to begin the branches of 'when'");
    HeaderGuard inside(*this, false);
    for (;;) {
        skipSemicolons();
        if (check(TokenType::RBrace) || check(TokenType::Eof)) break;
        if (!e->branches.empty() && e->branches.back().isElse)
            throw error(peek(), "'else' must be the last branch of a 'when'");

        WhenBranch branch;
        branch.line = peek().line;
        branch.col = peek().col;
        if (match(TokenType::Else)) {
            branch.isElse = true;
        } else {
            do {
                WhenCondition condition;
                const TokenType t = peek().type;
                if (e->subject && (t == TokenType::Is || t == TokenType::NotIs)) {
                    advance();
                    condition.kind = WhenCondition::Kind::Is;
                    condition.negated = t == TokenType::NotIs;
                    condition.type = checkedType();
                } else if (e->subject && (t == TokenType::In || t == TokenType::NotIn)) {
                    advance();
                    condition.kind = WhenCondition::Kind::In;
                    condition.negated = t == TokenType::NotIn;
                    condition.value = expression();
                } else {
                    condition.value = expression();
                }
                branch.conditions.push_back(std::move(condition));
            } while (match(TokenType::Comma));
        }
        expect(TokenType::ThinArrow, "'->' after the condition of a 'when' branch");
        // After the arrow a '{' opens a block, whose last expression is the branch's value.
        if (check(TokenType::LBrace)) branch.body = block();
        else branch.body = statement();
        e->branches.push_back(std::move(branch));
    }
    expect(TokenType::RBrace, "'}' to end the 'when'");
    return e;
}

// Whether the next token, on the same line, can begin an expression.
bool Parser::startsExpression() const {
    const Token& t = peek();
    if (t.newlineBefore) return false;
    switch (t.type) {
        case TokenType::Int:
        case TokenType::Float:
        case TokenType::String:
        case TokenType::InterpString:
        case TokenType::True:
        case TokenType::False:
        case TokenType::Null:
        case TokenType::Identifier:
        case TokenType::This:
        case TokenType::Super:
        case TokenType::LParen:
        case TokenType::LBracket:
        case TokenType::LBrace:
        case TokenType::Fun:
        case TokenType::Bang:
        case TokenType::BangBang:
        case TokenType::Minus:
        case TokenType::Tilde:
        case TokenType::Return:
        case TokenType::Throw:
        case TokenType::Break:
        case TokenType::Continue:
        case TokenType::When:
        case TokenType::If:
        case TokenType::Try:
            return true;
        default:
            return false;
    }
}

ExprPtr Parser::mapLiteral(const Token& open) {
    auto map = node<MapExpr>(open);
    while (!check(TokenType::RBrace)) {
        const Token& k = advance();
        ExprPtr key;
        switch (isKeyword(k.type) ? TokenType::Identifier : k.type) {
            case TokenType::Identifier:
            case TokenType::String: {
                auto literal = node<LiteralExpr>(k);
                literal->type = LiteralExpr::Type::String;
                literal->stringValue = k.type == TokenType::String ? k.stringValue : k.text;
                key = std::move(literal);
                break;
            }
            case TokenType::Int: {
                auto literal = node<LiteralExpr>(k);
                literal->type = LiteralExpr::Type::Int;
                literal->intValue = k.intValue;
                key = std::move(literal);
                break;
            }
            case TokenType::Float: {
                auto literal = node<LiteralExpr>(k);
                literal->type = LiteralExpr::Type::Float;
                literal->floatValue = k.floatValue;
                key = std::move(literal);
                break;
            }
            case TokenType::LBracket:
                key = expression();
                expect(TokenType::RBracket, "']' after computed key");
                break;
            default:
                throw error(k, "expected a map key, found " + describe(k));
        }
        expect(TokenType::Colon, "':' after map key");
        ExprPtr value = expression();
        map->entries.emplace_back(std::move(key), std::move(value));
        if (!match(TokenType::Comma)) break;
    }
    expect(TokenType::RBrace, "'}' to end the map");
    return map;
}

// A '{' where a value is expected is a map or a lambda. What follows it decides:
//   {:}                 the empty map
//   { key: value, ... } a map, when a key and ':' come first
//   { a, b -> ... }     a lambda with parameters, when a '->' closes its first line
//   { ... }  and  {}    a lambda whose one parameter, if it is given one, is `it`
ExprPtr Parser::braceExpression(const Token& open) {
    if (check(TokenType::Colon) && peekNext().type == TokenType::RBrace) {
        advance();
        advance();
        return node<MapExpr>(open);
    }
    if (check(TokenType::RBrace)) return lambdaLiteral(open, false);
    if (lambdaHasParams()) return lambdaLiteral(open, true);
    if (startsMapEntry()) {
        HeaderGuard inside(*this, false);
        return mapLiteral(open);
    }
    return lambdaLiteral(open, false);
}

// True if a lambda may start here as the last argument of the call just parsed.
bool Parser::trailingLambdaNext() const {
    return peek().type == TokenType::LBrace && !peek().newlineBefore && !headerExpr_;
}

// After a '{': does a map entry come next, that is a key followed by ':'?
bool Parser::startsMapEntry() const {
    if (isKeyword(peek().type)) return peekNext().type == TokenType::Colon;
    switch (peek().type) {
        case TokenType::Identifier:
        case TokenType::String:
        case TokenType::Int:
        case TokenType::Float:
            return peekNext().type == TokenType::Colon;
        case TokenType::LBracket: {
            // A computed key, `{ [expr]: value }`: find the matching ']'.
            int nesting = 0;
            for (size_t i = p_; i < toks_.size(); i++) {
                TokenType type = toks_[i].type;
                if (type == TokenType::Eof) return false;
                if (type == TokenType::LBracket || type == TokenType::LParen || type == TokenType::LBrace) nesting++;
                if (type == TokenType::RBracket || type == TokenType::RParen || type == TokenType::RBrace) nesting--;
                if (nesting == 0) return i + 1 < toks_.size() && toks_[i + 1].type == TokenType::Colon;
            }
            return false;
        }
        default:
            return false;
    }
}

// After a '{': does the lambda name its parameters, `a, b ->` or `(key, value) ->`? The '->'
// has to come outside any brackets, before the lambda could have ended, and on the first line
// unless the list is broken after a comma, as Kotlin code lays out a long one.
bool Parser::lambdaHasParams() const {
    if (peek().type != TokenType::Identifier && peek().type != TokenType::LParen) return false;
    int nesting = 0;
    for (size_t i = p_; i < toks_.size(); i++) {
        const Token& t = toks_[i];
        if (i > p_ && nesting == 0 && t.newlineBefore && toks_[i - 1].type != TokenType::Comma) return false;
        switch (t.type) {
            case TokenType::LParen:
            case TokenType::LBracket:
            case TokenType::LBrace:
                nesting++;
                break;
            case TokenType::RParen:
            case TokenType::RBracket:
                nesting--;
                break;
            case TokenType::RBrace:
                if (nesting == 0) return false;
                nesting--;
                break;
            case TokenType::ThinArrow:
                if (nesting == 0) return true;
                break;
            case TokenType::Semicolon:
                if (nesting == 0) return false;
                break;
            case TokenType::Eof:
                return false;
            default:
                break;
        }
    }
    return false;
}

// The rest of a lambda literal, after its '{'.
ExprPtr Parser::lambdaLiteral(const Token& open, bool hasParams) {
    DepthGuard guard(*this);
    auto e = node<FunctionExpr>(open);
    auto decl = std::make_unique<FunctionDecl>();
    decl->line = open.line;
    decl->col = open.col;
    decl->name = "<lambda>";
    decl->isLambda = true;

    auto addParam = [&](const Token& at, const std::string& name) {
        if (name != "_" && std::find(decl->params.begin(), decl->params.end(), name) != decl->params.end())
            throw error(at, "duplicate parameter '" + name + "'");
        // Kotlin's "not used" marker may be repeated; each gets a name nothing can refer to.
        decl->params.push_back(name == "_" ? "_" + std::to_string(decl->params.size()) : name);
        Node position;
        position.line = at.line;
        position.col = at.col;
        decl->paramPositions.push_back(position);
    };

    if (hasParams) {
        do {
            if (check(TokenType::LParen)) {
                // (a, b): the parts of one argument.
                const Token& at = peek();
                std::vector<std::string> pattern = destructuringPattern();
                decl->paramPatterns.resize(decl->params.size());
                decl->paramPatterns.push_back(std::move(pattern));
                addParam(at, "_");
                decl->paramTypes.emplace_back();
            } else {
                const Token& param = expect(TokenType::Identifier, "a parameter name");
                addParam(param, param.text);
                decl->paramTypes.push_back(optionalType());
            }
            decl->paramDefaults.push_back(nullptr);
        } while (match(TokenType::Comma));
        if (!decl->paramPatterns.empty()) decl->paramPatterns.resize(decl->params.size());
        expect(TokenType::ThinArrow, "'->' after the lambda's parameters");
    } else {
        // No parameter list: the lambda takes nothing, or one value that it calls `it`.
        addParam(open, "it");
        decl->paramTypes.emplace_back();
        decl->paramDefaults.push_back(node<LiteralExpr>(open));
    }

    {
        HeaderGuard body(*this, false);
        for (;;) {
            skipSemicolons();
            if (check(TokenType::RBrace) || check(TokenType::Eof)) break;
            decl->body.push_back(declaration());
        }
    }
    expect(TokenType::RBrace, "'}' to end the lambda");

    // The value of a lambda is the value of its last expression, an `if` or `try` included.
    if (!decl->body.empty() && (decl->body.back()->kind == StmtKind::If || decl->body.back()->kind == StmtKind::Try)) {
        auto value = std::make_unique<StatementExpr>();
        value->line = decl->body.back()->line;
        value->col = decl->body.back()->col;
        value->statement = std::move(decl->body.back());
        auto result = std::make_unique<ReturnStmt>();
        result->line = value->line;
        result->col = value->col;
        result->value = std::move(value);
        result->implicit = true;
        decl->body.back() = std::move(result);
    } else if (!decl->body.empty() && decl->body.back()->kind == StmtKind::Expr) {
        auto& last = static_cast<ExprStmt&>(*decl->body.back());
        auto result = std::make_unique<ReturnStmt>();
        result->line = last.line;
        result->col = last.col;
        result->value = std::move(last.expr);
        result->implicit = true;
        decl->body.back() = std::move(result);
    }

    e->fn = std::move(decl);
    return e;
}

}  // namespace astra
