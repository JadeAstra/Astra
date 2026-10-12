#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lexer.h"

namespace astra {

struct Node {
    int line = 0;
    int col = 0;
};

// ---- Expressions ----

enum class ExprKind {
    Literal, Interp, Name, This, Super, Unary, Binary, Logical, Ternary, Assign,
    Call, Member, Index, List, Map, Range, Is, Function, IncDec,
};

struct Expr : Node {
    const ExprKind kind;
    explicit Expr(ExprKind k) : kind(k) {}
    virtual ~Expr() = default;
};

using ExprPtr = std::unique_ptr<Expr>;

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

// A written type, as in `x : int` or `x : Shape?`. Its position is that of the type name.
struct TypeAnnotation : Node {
    std::string name;       // empty when no type was written
    bool nullable = false;  // ends in '?'

    bool present() const { return !name.empty(); }
};

// `fun name(a : type, b = value) : returnType { ... }`. Types and default values are optional.
struct FunctionDecl : Node {
    std::string name;
    std::vector<std::string> params;
    std::vector<TypeAnnotation> paramTypes;  // one per parameter
    std::vector<ExprPtr> paramDefaults;      // one per parameter; null when it has no default value
    std::vector<Node> paramPositions;        // one per parameter, for error messages
    TypeAnnotation returnType;
    std::vector<StmtPtr> body;
    bool isStatic = false;

    // A lambda literal, `{ a, b -> ... }` or `{ ... it ... }`. Its value is its last expression,
    // and a name it does not declare may belong to the object it is run on (its receiver).
    bool isLambda = false;

    // In a class: `get name()` and `set name(value)` define a property instead of a method.
    enum class Accessor { None, Get, Set };
    Accessor accessor = Accessor::None;
};

struct LiteralExpr : Expr {
    enum class Type { Null, Bool, Int, Float, String };
    Type type = Type::Null;
    bool boolValue = false;
    int64_t intValue = 0;
    double floatValue = 0;
    std::string stringValue;
    LiteralExpr() : Expr(ExprKind::Literal) {}
};

struct InterpExpr : Expr {
    std::vector<ExprPtr> parts;
    InterpExpr() : Expr(ExprKind::Interp) {}
};

struct NameExpr : Expr {
    std::string name;
    NameExpr() : Expr(ExprKind::Name) {}
};

struct ThisExpr : Expr {
    ThisExpr() : Expr(ExprKind::This) {}
};

struct SuperExpr : Expr {
    std::string method;
    SuperExpr() : Expr(ExprKind::Super) {}
};

struct UnaryExpr : Expr {
    TokenType op = TokenType::Minus;
    ExprPtr operand;
    UnaryExpr() : Expr(ExprKind::Unary) {}
};

struct BinaryExpr : Expr {
    TokenType op = TokenType::Plus;
    ExprPtr left, right;
    BinaryExpr() : Expr(ExprKind::Binary) {}
};

struct LogicalExpr : Expr {
    bool isAnd = false;
    ExprPtr left, right;
    LogicalExpr() : Expr(ExprKind::Logical) {}
};

struct TernaryExpr : Expr {
    ExprPtr condition, then, otherwise;
    TernaryExpr() : Expr(ExprKind::Ternary) {}
};

// `op` is Eq for plain assignment, or one of the compound tokens (PlusEq, ...).
struct AssignExpr : Expr {
    ExprPtr target;
    TokenType op = TokenType::Eq;
    ExprPtr value;
    AssignExpr() : Expr(ExprKind::Assign) {}
};

// x++ and x--. The target is a variable, a field or an element. These are parsed where an
// expression can appear but only allowed as a statement of their own: they have no value.
struct IncDecExpr : Expr {
    ExprPtr target;
    bool increment = false;
    IncDecExpr() : Expr(ExprKind::IncDec) {}
};

struct CallExpr : Expr {

    ExprPtr callee;
    std::vector<ExprPtr> args;

    // One per argument: the name for `f(name = value)`, empty for a positional argument.
    // Positional arguments always come first.
    std::vector<std::string> names;
    size_t namedCount = 0;

    // The last argument is a lambda written after the ')': `button("OK") { ... }`.
    bool trailingLambda = false;
    CallExpr() : Expr(ExprKind::Call) {}
};

struct MemberExpr : Expr {
    ExprPtr object;
    std::string name;
    MemberExpr() : Expr(ExprKind::Member) {}
};

struct IndexExpr : Expr {
    ExprPtr object, index;
    IndexExpr() : Expr(ExprKind::Index) {}
};

struct ListExpr : Expr {
    std::vector<ExprPtr> items;
    ListExpr() : Expr(ExprKind::List) {}
};

struct MapExpr : Expr {
    std::vector<std::pair<ExprPtr, ExprPtr>> entries;
    MapExpr() : Expr(ExprKind::Map) {}
};

struct RangeExpr : Expr {
    ExprPtr start, end;
    bool inclusive = false;
    RangeExpr() : Expr(ExprKind::Range) {}
};

struct IsExpr : Expr {
    ExprPtr value, type;
    IsExpr() : Expr(ExprKind::Is) {}
};

struct FunctionExpr : Expr {
    std::unique_ptr<FunctionDecl> fn;
    FunctionExpr() : Expr(ExprKind::Function) {}
};

// ---- Statements ----

enum class StmtKind {
    Expr, Var, Block, If, While, ForIn, Return, Break, Continue, Throw, Try, Import, Function, Class,
};

struct Stmt : Node {
    const StmtKind kind;
    explicit Stmt(StmtKind k) : kind(k) {}
    virtual ~Stmt() = default;
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt() : Stmt(StmtKind::Expr) {}
};

// `val name : type = init` (immutable) or `var name : type = init`. The type is optional.
struct VarStmt : Stmt {
    std::string name;
    ExprPtr init;  // may be null (var only)
    bool isVal = false;

    TypeAnnotation type;

    VarStmt() : Stmt(StmtKind::Var) {}
};

struct BlockStmt : Stmt {
    std::vector<StmtPtr> statements;
    BlockStmt() : Stmt(StmtKind::Block) {}
};

struct IfStmt : Stmt {
    ExprPtr condition;
    StmtPtr then;
    StmtPtr otherwise;  // may be null
    IfStmt() : Stmt(StmtKind::If) {}
};

struct WhileStmt : Stmt {
    ExprPtr condition;
    std::unique_ptr<BlockStmt> body;
    WhileStmt() : Stmt(StmtKind::While) {}
};

struct ForInStmt : Stmt {
    std::string variable;
    ExprPtr iterable;
    std::unique_ptr<BlockStmt> body;
    ForInStmt() : Stmt(StmtKind::ForIn) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value;  // may be null
    ReturnStmt() : Stmt(StmtKind::Return) {}
};

struct BreakStmt : Stmt {
    BreakStmt() : Stmt(StmtKind::Break) {}
};

struct ContinueStmt : Stmt {
    ContinueStmt() : Stmt(StmtKind::Continue) {}
};

struct ThrowStmt : Stmt {
    ExprPtr value;
    ThrowStmt() : Stmt(StmtKind::Throw) {}
};

struct TryStmt : Stmt {
    std::unique_ptr<BlockStmt> body;
    bool hasCatchVariable = false;
    std::string catchVariable;
    std::unique_ptr<BlockStmt> handler;
    TryStmt() : Stmt(StmtKind::Try) {}
};

struct ImportStmt : Stmt {
    std::string path;
    ImportStmt() : Stmt(StmtKind::Import) {}
};

struct FunctionStmt : Stmt {
    std::unique_ptr<FunctionDecl> fn;
    FunctionStmt() : Stmt(StmtKind::Function) {}
};

// One entry of a class header: `val x : T` / `var x : T` declares a field; a bare `x : T`
// is only a parameter of the generated initializer. Either may end in `= value`.
struct HeaderParam : Node {
    std::string name;
    TypeAnnotation type;
    bool isField = false;

    // With a generated initializer this is the argument's default, making it optional. In a
    // class that writes its own init it is simply the field's initial value.
    ExprPtr defaultValue;
};

// class Name(val a : T, var b, c) : Super(args) { fields and methods }
struct ClassStmt : Stmt {
    std::string name;

    // Fields can be listed in a header after the name. They are ordinary declared fields,
    // and a class that writes no init of its own gets one that takes the header's entries
    // in order.
    bool hasHeader = false;
    std::vector<HeaderParam> header;
    size_t headerFieldCount = 0;  // the first so many of `fields` come from the header

    bool hasSuper = false;
    std::string superName;
    bool hasSuperArgs = false;        // `: Super(...)`: arguments for the superclass initializer
    std::vector<ExprPtr> superArgs;

    std::vector<std::unique_ptr<VarStmt>> fields;  // header fields, then the body's `val`/`var`
    std::vector<std::unique_ptr<FunctionDecl>> methods;
    ClassStmt() : Stmt(StmtKind::Class) {}
};

}  // namespace astra
