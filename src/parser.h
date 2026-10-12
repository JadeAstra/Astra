#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "lexer.h"

namespace astra {

// Recursive-descent parser. Statements end at a line break or ';'. A line break also stops
// an expression from continuing with '(', '[' or binary '-', the only infix forms that could
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

    struct DepthGuard;

    const Token& peek() const { return toks_[p_]; }
    const Token& peekNext() const { return toks_[p_ + 1 < toks_.size() ? p_ + 1 : toks_.size() - 1]; }
    const Token& advance();
    bool check(TokenType type) const { return toks_[p_].type == type; }
    bool match(TokenType type);
    const Token& expect(TokenType type, const char* what);
    CompileError error(const Token& at, const std::string& message) const;

    void endStatement();
    void skipSemicolons();

    StmtPtr declaration();
    TypeAnnotation optionalType();
    std::unique_ptr<VarStmt> varDeclaration(bool isField);
    StmtPtr classDeclaration();
    std::unique_ptr<FunctionDecl> method();
    std::unique_ptr<FunctionDecl> accessor();
    std::string operatorName();
    std::unique_ptr<FunctionDecl> functionRest(std::string name, const Token& start);
    std::unique_ptr<BlockStmt> block();
    StmtPtr statement();
    StmtPtr ifStatement();

    ExprPtr parseSingleExpression();
    ExprPtr expression();
    ExprPtr assignment();
    ExprPtr ternary();
    ExprPtr logicalOr();
    ExprPtr logicalAnd();
    ExprPtr equality();
    ExprPtr comparison();
    ExprPtr range();
    ExprPtr additive();
    ExprPtr multiplicative();
    ExprPtr unary();
    ExprPtr postfix();
    ExprPtr primary();
    ExprPtr mapLiteral(const Token& open);
};

}  // namespace astra
