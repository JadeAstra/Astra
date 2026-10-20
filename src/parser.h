#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "lexer.h"

namespace astra {

// Recursive-descent parser. Statements end at a line break or ';'. A line break also stops
// an expression from continuing with '(', '[', binary '-', '++' or '--', the forms that could
// equally well start the next statement.
class Parser {
public:
    Parser(const std::vector<Token>& tokens, std::string file);
    std::vector<StmtPtr> parseProgram();

private:
    const std::vector<Token>& toks_;
    std::string file_;
    size_t p_ = 0;
    int depth_ = 0;

    // True while parsing the condition of an `if`, `while` or `for`: a '{' there opens the
    // body, so it cannot also be a lambda passed to the call just before it. Parentheses and
    // brackets switch it off again for what they enclose.
    bool headerExpr_ = false;
    struct HeaderGuard;

    struct DepthGuard;

    const Token& peek() const { return toks_[p_]; }
    const Token& peekNext() const { return toks_[p_ + 1 < toks_.size() ? p_ + 1 : toks_.size() - 1]; }
    const Token& advance();
    bool check(TokenType type) const { return toks_[p_].type == type; }
    bool match(TokenType type);
    const Token& expect(TokenType type, const char* what);

    // Where only a name can come (after a '.', as the name of a member being declared, as the
    // name of an argument) a keyword is a name like any other: `token.class`, `Key.if`.
    static bool isKeyword(TokenType type);
    static bool isName(const Token& t) { return t.type == TokenType::Identifier || isKeyword(t.type); }
    const Token& expectName(const char* what);
    bool word(const char* text) const { return peek().type == TokenType::Identifier && peek().text == text; }
    bool nextWord(const char* text) const { return peekNext().type == TokenType::Identifier && peekNext().text == text; }
    CompileError error(const Token& at, const std::string& message) const;

    void endStatement();
    void skipSemicolons();
    void skipAnnotations();

    StmtPtr declaration();
    StmtPtr externalDeclaration();
    TypeAnnotation optionalType();
    TypeAnnotation typeExpression();
    TypeAnnotation checkedType();
    bool tightNullable_ = false;     // after `is` and `as`, the '?' of a nullable type must touch the type
    bool adjacentToPrevious() const;
    // How many more type argument lists the '>>' or '>>>' at hand closes, besides the inner one
    // that read it.
    int pendingTypeClose_ = 0;
    std::vector<TypeAnnotation> typeArgumentsRest();  // after '<': the type arguments and '>'
    std::vector<TypeParam> typeParameters();            // `<T, U : Bound>`; the '<' is next

    // The type parameters in reach, innermost last: a written type naming one is marked.
    // A class or function puts its type parameters in reach for as long as it lasts.
    std::vector<std::vector<std::string>> typeParamScopes_;
    struct TypeParamScope {
        Parser& parser;
        TypeParamScope(Parser& p, const std::vector<TypeParam>& params) : parser(p) {
            parser.typeParamScopes_.emplace_back();
            for (const TypeParam& t : params) parser.typeParamScopes_.back().push_back(t.name);
        }
        ~TypeParamScope() { parser.typeParamScopes_.pop_back(); }
    };
    bool isTypeParam(const std::string& name) const;
    bool typeArgumentsBeforeCall(std::vector<TypeAnnotation>& into);  // `f<int>(`: reads them if so
    std::unique_ptr<VarStmt> varDeclaration(bool isField);
    std::unique_ptr<VarStmt> constDeclaration(bool isField);
    bool nextIsObject_ = false;  // the class about to be parsed is `object Name`
    std::vector<std::string> destructuringPattern();
    void fieldAccessors(VarStmt& field);
    std::string jumpLabel();
    void addDataMembers(ClassStmt& s, const Token& at);
    StmtPtr classDeclaration(bool isEnum = false, bool isInterface = false, const Token* anonymous = nullptr);
    bool anonymousObjectNext() const;
    bool inInterface_ = false;  // a method may be declared without a body
    bool externalBody_ = false;  // in `external object`: the C++ provides the methods
    bool abstractMember_ = false;  // so may the member being read now, which is marked abstract
    void classMember(ClassStmt& s, bool inStatic);
    std::unique_ptr<FunctionDecl> method();
    std::unique_ptr<FunctionDecl> accessor();
    std::string operatorName();
    std::unique_ptr<FunctionDecl> functionRest(std::string name, const Token& start);
    std::unique_ptr<BlockStmt> block();
    StmtPtr statement();
    StmtPtr ifStatement();
    void rejectLambdaInHeader(const char* keyword);

    ExprPtr parseSingleExpression();
    ExprPtr expression();
    ExprPtr assignment();
    ExprPtr ternary();
    ExprPtr logicalOr();
    ExprPtr logicalAnd();
    ExprPtr equality();
    ExprPtr comparison();
    ExprPtr elvis();
    ExprPtr pairing();
    ExprPtr range();
    ExprPtr bitOr();
    ExprPtr bitXor();
    ExprPtr bitAnd();
    ExprPtr shift();
    ExprPtr additive();
    ExprPtr multiplicative();
    ExprPtr cast();
    ExprPtr unary();
    ExprPtr postfix();
    ExprPtr primary();
    ExprPtr jumpExpression(const Token& keyword);
    ExprPtr whenExpression(const Token& keyword);
    bool startsExpression() const;
    ExprPtr mapLiteral(const Token& open);
    ExprPtr braceExpression(const Token& open);
    ExprPtr lambdaLiteral(const Token& open, bool hasParams);
    bool lambdaHasParams() const;
    bool startsMapEntry() const;
    bool trailingLambdaNext() const;
};

}  // namespace astra
