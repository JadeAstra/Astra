#include "parser.h"

#include <algorithm>

namespace astra {

namespace {

const int kMaxNesting = 256;

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

StmtPtr Parser::declaration() {
    DepthGuard guard(*this);
    switch (peek().type) {
        case TokenType::Identifier: {
            // `let x = 1` would otherwise fail with a puzzling message about a missing line break.
            const Token& word = peek();
            const Token& after = peekNext();
            if ((word.text == "let" || word.text == "const") && after.type == TokenType::Identifier &&
                !after.newlineBefore)
                throw error(word, "'" + word.text + "' is not a keyword; declare variables with 'val' (immutable) or 'var'");
            if (word.text == "fn" && after.type == TokenType::Identifier && !after.newlineBefore)
                throw error(word, "'fn' is not a keyword; declare functions with 'fun'");
            return statement();
        }
        case TokenType::Class:
            return classDeclaration();
        case TokenType::Fun:
            if (peekNext().type == TokenType::Identifier) {
                const Token& kw = advance();
                const Token& name = advance();
                auto s = node<FunctionStmt>(kw);
                s->fn = functionRest(name.text, kw);
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

// Parses ": type" or ": type?" if a ':' comes next; otherwise returns an absent annotation.
TypeAnnotation Parser::optionalType() {
    TypeAnnotation type;
    if (!match(TokenType::Colon)) return type;

    const Token& name = expect(TokenType::Identifier, "a type name after ':'");
    type.name = name.text;
    type.line = name.line;
    type.col = name.col;
    type.nullable = match(TokenType::Question);
    return type;
}

// A `val`/`var` declaration. As a class field, a val may be left for the initializer to set.
std::unique_ptr<VarStmt> Parser::varDeclaration(bool isField) {
    const Token& kw = advance();
    const Token& name = expect(TokenType::Identifier, isField ? "a field name" : "a variable name");
    auto s = node<VarStmt>(name);
    s->name = name.text;
    s->isVal = kw.type == TokenType::Val;

    s->type = optionalType();

    if (match(TokenType::Eq)) {
        s->init = expression();
    } else if (s->isVal && !isField) {
        throw error(peek(), "val '" + name.text + "' must be initialized");
    }
    endStatement();
    return s;
}

StmtPtr Parser::classDeclaration() {
    advance();
    const Token& name = expect(TokenType::Identifier, "a class name");
    auto s = node<ClassStmt>(name);
    s->name = name.text;

    // Header: class Name(val a : T, var b : U)
    if (match(TokenType::LParen)) {
        s->hasHeader = true;
        while (!check(TokenType::RParen)) {
            // `val x : T` and `var x : T` are fields; a bare `x : T` is only a parameter.
            bool isField = check(TokenType::Val) || check(TokenType::Var);
            bool isVal = check(TokenType::Val);
            if (isField) advance();
            const Token& paramName = expect(TokenType::Identifier, isField ? "a field name" : "'val', 'var' or a parameter name");

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
        s->hasSuper = true;
        s->superName = expect(TokenType::Identifier, "a superclass name").text;

        // : Super(args) passes arguments to the superclass initializer.
        if (check(TokenType::LParen) && !peek().newlineBefore) {
            const Token& open = advance();
            s->hasSuperArgs = true;
            while (!check(TokenType::RParen)) {
                s->superArgs.push_back(expression());
                if (!match(TokenType::Comma)) break;
            }
            expect(TokenType::RParen, "')' after the superclass arguments");
            if (s->superArgs.size() > 255) throw error(open, "a call cannot have more than 255 arguments");
        }
    }

    // The body is optional: `class Point(val x : int, val y : int)` is complete as it stands.
    if (!check(TokenType::LBrace)) {
        endStatement();
        return s;
    }

    expect(TokenType::LBrace, "'{' to begin the class body");
    for (;;) {
        skipSemicolons();
        if (check(TokenType::RBrace) || check(TokenType::Eof)) break;

        if (check(TokenType::Val) || check(TokenType::Var)) {
            s->fields.push_back(varDeclaration(true));
        } else if (check(TokenType::Static) && (peekNext().type == TokenType::Val || peekNext().type == TokenType::Var)) {
            throw error(peek(), "static fields cannot be declared; assign them after the class (" + s->name + ".name = value)");
        } else if (check(TokenType::Identifier) && (peek().text == "get" || peek().text == "set") &&
                   peekNext().type == TokenType::Identifier) {
            // `get` and `set` are only special here, so they stay usable as ordinary names.
            s->methods.push_back(accessor());
        } else {
            s->methods.push_back(method());
        }
    }
    expect(TokenType::RBrace, "'}' to end the class body");
    return s;
}

// A property: `get name() : type { ... }` or `set name(value : type) { ... }`.
std::unique_ptr<FunctionDecl> Parser::accessor() {
    const Token& kind = advance();
    bool isGetter = kind.text == "get";
    const Token& name = expect(TokenType::Identifier, "a property name");

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
    bool isStatic = match(TokenType::Static);
    bool isOperator = match(TokenType::Operator);
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
        } else if (op == "[]=") {
            if (arity != 2) throw error(opToken, "op []= takes exactly two parameters (index, value)");
        } else if (arity != 1) {
            throw error(opToken, "op " + op + " takes exactly one parameter");
        }
        return decl;
    }

    const Token& name = expect(TokenType::Identifier, "a method name");
    auto decl = functionRest(name.text, start);
    decl->isStatic = isStatic;
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
        const Token& param = expect(TokenType::Identifier, "a parameter name");
        if (std::find(decl->params.begin(), decl->params.end(), param.text) != decl->params.end())
            throw error(param, "duplicate parameter '" + param.text + "'");
        decl->params.push_back(param.text);
        Node position;
        position.line = param.line;
        position.col = param.col;
        decl->paramPositions.push_back(position);
        decl->paramTypes.push_back(optionalType());
        if (match(TokenType::Eq)) decl->paramDefaults.push_back(expression());
        else decl->paramDefaults.push_back(nullptr);
        if (!match(TokenType::Comma)) break;
    }
    expect(TokenType::RParen, "')' after the parameter list");
    if (decl->params.size() > 255) throw error(start, "a function cannot have more than 255 parameters");
    decl->returnType = optionalType();

    if (match(TokenType::Arrow)) {
        auto ret = node<ReturnStmt>(peek());
        ret->value = expression();
        decl->body.push_back(std::move(ret));
    } else {
        decl->body = std::move(block()->statements);
    }
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

StmtPtr Parser::statement() {
    const Token& t = peek();
    switch (t.type) {
        case TokenType::LBrace:
            return block();

        case TokenType::If:
            return ifStatement();

        case TokenType::While: {
            advance();
            auto s = node<WhileStmt>(t);
            s->condition = expression();
            s->body = block();
            return s;
        }

        case TokenType::For: {
            advance();
            auto s = node<ForInStmt>(t);
            bool paren = match(TokenType::LParen);
            s->variable = expect(TokenType::Identifier, "a loop variable name").text;
            expect(TokenType::In, "'in'");
            s->iterable = expression();
            if (paren) expect(TokenType::RParen, "')'");
            s->body = block();
            return s;
        }

        case TokenType::Return: {
            advance();
            auto s = node<ReturnStmt>(t);
            if (!check(TokenType::Semicolon) && !check(TokenType::RBrace) && !check(TokenType::Eof) &&
                !peek().newlineBefore)
                s->value = expression();
            endStatement();
            return s;
        }

        case TokenType::Break:
            advance();
            endStatement();
            return node<BreakStmt>(t);

        case TokenType::Continue:
            advance();
            endStatement();
            return node<ContinueStmt>(t);

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
            expect(TokenType::Catch, "'catch' after the try block");
            if (match(TokenType::LParen)) {
                s->catchVariable = expect(TokenType::Identifier, "an exception variable name").text;
                s->hasCatchVariable = true;
                expect(TokenType::RParen, "')'");
            } else if (check(TokenType::Identifier)) {
                s->catchVariable = advance().text;
                s->hasCatchVariable = true;
            }
            s->handler = block();
            return s;
        }

        case TokenType::Import: {
            advance();
            auto s = node<ImportStmt>(t);
            s->path = expect(TokenType::String, "a module path string").stringValue;
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

StmtPtr Parser::ifStatement() {
    const Token& kw = advance();
    auto s = node<IfStmt>(kw);
    s->condition = expression();
    s->then = block();
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
        case TokenType::PercentEq: {
            const Token& op = advance();
            if (left->kind != ExprKind::Name && left->kind != ExprKind::Member && left->kind != ExprKind::Index)
                throw error(op, "invalid assignment target");
            auto e = node<AssignExpr>(op);
            e->op = op.type;
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
    while (check(TokenType::OrOr)) {
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
    while (check(TokenType::AndAnd)) {
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
    while (check(TokenType::EqEq) || check(TokenType::BangEq)) {
        const Token& op = advance();
        ExprPtr right = comparison();
        left = binary(op, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::comparison() {
    ExprPtr left = range();
    for (;;) {
        TokenType t = peek().type;
        if (t == TokenType::Lt || t == TokenType::LtEq || t == TokenType::Gt || t == TokenType::GtEq) {
            const Token& op = advance();
            ExprPtr right = range();
            left = binary(op, std::move(left), std::move(right));
        } else if (t == TokenType::Is) {
            const Token& op = advance();
            auto e = node<IsExpr>(op);
            e->value = std::move(left);
            e->type = range();
            left = std::move(e);
        } else {
            return left;
        }
    }
}

ExprPtr Parser::range() {
    ExprPtr left = additive();
    if (check(TokenType::DotDot) || check(TokenType::DotDotEq)) {
        const Token& op = advance();
        auto e = node<RangeExpr>(op);
        e->inclusive = op.type == TokenType::DotDotEq;
        e->start = std::move(left);
        e->end = additive();
        return e;
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
    ExprPtr left = unary();
    while (check(TokenType::Star) || check(TokenType::Slash) || check(TokenType::Percent)) {
        const Token& op = advance();
        ExprPtr right = unary();
        left = binary(op, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::unary() {
    if (check(TokenType::Bang) || check(TokenType::Minus)) {
        DepthGuard guard(*this);
        const Token& op = advance();
        auto e = node<UnaryExpr>(op);
        e->op = op.type;
        e->operand = unary();
        return e;
    }
    return postfix();
}

ExprPtr Parser::postfix() {
    ExprPtr e = primary();
    for (;;) {
        const Token& t = peek();
        if (t.type == TokenType::LParen && !t.newlineBefore) {
            advance();
            auto call = node<CallExpr>(t);
            while (!check(TokenType::RParen)) {
                // `name = value` passes an argument by name.
                if (check(TokenType::Identifier) && peekNext().type == TokenType::Eq) {
                    const Token& argName = advance();
                    advance();
                    for (const std::string& earlier : call->names)
                        if (earlier == argName.text) throw error(argName, "argument '" + argName.text + "' is given more than once");
                    call->names.push_back(argName.text);
                    call->namedCount++;
                } else {
                    if (call->namedCount > 0)
                        throw error(peek(), "positional arguments must come before named ones");
                    call->names.emplace_back();
                }
                call->args.push_back(expression());
                if (!match(TokenType::Comma)) break;
            }
            expect(TokenType::RParen, "')' after arguments");
            if (call->args.size() > 255) throw error(t, "a call cannot have more than 255 arguments");
            call->callee = std::move(e);
            e = std::move(call);
        } else if (t.type == TokenType::LBracket && !t.newlineBefore) {
            advance();
            auto index = node<IndexExpr>(t);
            index->index = expression();
            expect(TokenType::RBracket, "']' after index");
            index->object = std::move(e);
            e = std::move(index);
        } else if (t.type == TokenType::Dot) {
            advance();
            const Token& name = expect(TokenType::Identifier, "a member name after '.'");
            auto member = node<MemberExpr>(name);
            member->name = name.text;
            member->object = std::move(e);
            e = std::move(member);
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
            const Token& name = expect(TokenType::Identifier, "a superclass method name");
            auto e = node<SuperExpr>(t);
            e->method = name.text;
            return e;
        }

        case TokenType::LParen: {
            ExprPtr e = expression();
            expect(TokenType::RParen, "')'");
            return e;
        }

        case TokenType::LBracket: {
            auto e = node<ListExpr>(t);
            while (!check(TokenType::RBracket)) {
                e->items.push_back(expression());
                if (!match(TokenType::Comma)) break;
            }
            expect(TokenType::RBracket, "']' to end the list");
            return e;
        }

        case TokenType::LBrace:
            return mapLiteral(t);

        case TokenType::Fun: {
            auto e = node<FunctionExpr>(t);
            e->fn = functionRest("<lambda>", t);
            return e;
        }

        default:
            throw error(t, "expected an expression, found " + describe(t));
    }
}

ExprPtr Parser::mapLiteral(const Token& open) {
    auto map = node<MapExpr>(open);
    while (!check(TokenType::RBrace)) {
        const Token& k = advance();
        ExprPtr key;
        switch (k.type) {
            case TokenType::Identifier:
            case TokenType::String: {
                auto literal = node<LiteralExpr>(k);
                literal->type = LiteralExpr::Type::String;
                literal->stringValue = k.type == TokenType::Identifier ? k.text : k.stringValue;
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

}  // namespace astra
