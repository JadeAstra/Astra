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
    Call, Member, Index, List, Map, Range, Is, Function, IncDec, Elvis, Jump, Cast, In, When, Statement, Object, ClassOf,
};

struct Expr : Node {
    const ExprKind kind;
    explicit Expr(ExprKind k) : kind(k) {}
    virtual ~Expr() = default;
};

using ExprPtr = std::unique_ptr<Expr>;

// `@JsonField("id")` written on a declaration (D153): an object of the annotation's class, made
// as a call to it is when reflection reads it. `name` as written: `JsonField`, `ui.Tag`.
struct Annotation : Node {
    std::string name;
    ExprPtr call;  // a CallExpr
};
using Annotations = std::vector<Annotation>;

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

// A written type, as in `x : int`, `x : Shape?`, `list<string>` or `(int) -> bool`. Its
// position is that of the type name. When the program runs only `name` and `nullable` are
// looked at; the type checker reads the rest.
struct TypeAnnotation : Node {
    std::string name;       // empty when no type was written; "Function" for a function type
    bool nullable = false;  // ends in '?'
    // `open List<int>`: a list, map or set that may be changed through this name; one written
    // without it may only be read (D32). `accepts Button`: a type argument of an open type
    // that takes a Button, the list holding Button or any type above it (D34).
    bool isOpen = false;
    bool accepts = false;
    // `int | string` (D40): the types it may be, and `name` is "|". `null` among them makes it
    // nullable.
    std::vector<TypeAnnotation> unionOf;

    // `list<int>`, `map<string, Shape>`: the type arguments. `list<*>` has one star.
    std::vector<TypeAnnotation> args;
    bool star = false;  // this is the `*` of `list<*>`: any type there

    // A function type, `(int, string) -> bool` or `Grid.() -> unit`: `params` are the parameter
    // types, `result` the result type, and `receiver` the type of the object it runs on (absent
    // when there is none).
    std::vector<TypeAnnotation> params;
    std::vector<TypeAnnotation> result;    // one when it is a function type, otherwise empty
    std::vector<TypeAnnotation> receiver;  // one for `Grid.() -> unit`, otherwise empty
    bool isSuspend = false;                // `suspend () -> unit`: of a function that may wait
    // A function type's `throws IOException | TimeoutException`: the checked exceptions a
    // function of it may throw (D52). None written, none.
    std::vector<TypeAnnotation> throwsTypes;

    // The name is a type parameter of the class or function around it (`T` in `class Box<T>`):
    // any value fits it when the program runs.
    bool typeParam = false;

    bool present() const { return !name.empty() || star; }
    bool isFunction() const { return !result.empty(); }
};

// One type parameter: `T` in `class Box<T>`, or `T : Comparable<T>` with a bound.
struct TypeParam : Node {
    std::string name;
    TypeAnnotation bound;  // absent when none is written
};

// `fun name(a : type, b = value) : returnType { ... }`. Types and default values are optional.
// The access words: who may use a member of a class.
enum AccessWord : uint8_t { kAccessWordNone, kAccessWordPublic, kAccessWordPrivate, kAccessWordProtected, kAccessWordInternal };

inline uint8_t accessWord(const std::string& word) {
    return word == "public" ? kAccessWordPublic : word == "private" ? kAccessWordPrivate
           : word == "protected" ? kAccessWordProtected : word == "internal" ? kAccessWordInternal : kAccessWordNone;
}

struct FunctionDecl : Node {
    std::string name;
    std::vector<TypeParam> typeParams;  // `fun <T> first(items : list<T>) : T`
    std::vector<std::string> params;
    std::vector<TypeAnnotation> paramTypes;  // one per parameter
    std::vector<ExprPtr> paramDefaults;      // one per parameter; null when it has no default value
    std::vector<Node> paramPositions;        // one per parameter, for error messages
    std::vector<Annotations> paramAnnotations;  // one per parameter, or none at all (D153)
    Annotations annotations;
    TypeAnnotation returnType;
    std::vector<StmtPtr> body;
    bool isStatic = false;

    // `vararg items`: the parameter that gathers any number of arguments into a list, or -1.
    int varargIndex = -1;
    TypeAnnotation varargType;  // `vararg items : int`: the type of each argument

    // In an interface: `fun name(params)` with no body, for the implementing class to supply.
    bool isAbstract = false;

    // Written `override`: it takes the place of a member the class would otherwise be given.
    bool isOverride = false;

    // Written `suspend`: it may wait, setting its flow aside, and only code that may wait too
    // calls it (R1, D30).
    bool isSuspend = false;

    // Written `mutating`, in a value type (M6.6 part 4): it may give the value's var fields new
    // values, and is called on a place, which holds the result.
    bool isMutating = false;

    // `throws IOException | TimeoutException` written after the result type (D54): the checked
    // exceptions it may throw, which callers handle. Required on a library's public functions
    // and where there is no body; anywhere else a check.
    std::vector<TypeAnnotation> throwsTypes;

    // Written `infix` (D72): a member or an extension of one parameter, called `a name b`.
    bool isInfix = false;

    // Written `inline` (D59): copied into each call with the lambdas given to it.
    bool isInline = false;

    // Written `tail` (D29): each call it makes of itself is in tail position, made a loop.
    bool isTail = false;

    // Where its body is among the file's tokens, from its first to past its last: what a
    // file's summary leaves out, other files seeing only what is learned from it (D145).
    size_t bodyFrom = 0, bodyTo = 0;

    // The access word in front of a member (an AccessWord): who may use it.
    uint8_t access = 0;

    // Made by the compiler rather than written (the `copy` of a data class), so the rule about
    // saying `override` does not apply to it.
    bool generated = false;

    // For a lambda literal: the names `return@name` can use for it. One written before it
    // (`name@ { ... }`), and that of the function it is passed to.
    std::string label;
    std::string calleeLabel;

    // One per parameter when any is written `(a, b)`: the names its parts go to. Empty otherwise.
    std::vector<std::vector<std::string>> paramPatterns;

    // A lambda literal, `{ a, b -> ... }` or `{ ... it ... }`. Its value is its last expression,
    // and a name it does not declare may belong to the object it is run on (its receiver).
    bool isLambda = false;

    // In a class: `get name()` and `set name(value)` define a property instead of a method.
    enum class Accessor { None, Get, Set };
    Accessor accessor = Accessor::None;
};

struct LiteralExpr : Expr {
    enum class Type { Null, Bool, Int, Float, String, Char };  // a Char's code point is intValue
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
    // `this@Outer`, `this@grid` (D57): the object of the class Outer around the code, or of the
    // block given to grid. Empty for a bare `this`.
    std::string label;
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
    bool nullSafe = false;  // `+=?` and the like: nothing is done when the target is null (L118)
    AssignExpr() : Expr(ExprKind::Assign) {}
};

// x++ and x--. The target is a variable, a field or an element. These are parsed where an
// expression can appear but only allowed as a statement of their own: they have no value.
struct IncDecExpr : Expr {
    ExprPtr target;
    bool increment = false;
    bool nullSafe = false;  // `++?` and `--?`: nothing is done when the target is null (L118)
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

    // Type arguments written at the call: `emptyList<int>()`, `Box<string>(null)`.
    std::vector<TypeAnnotation> typeArgs;

    // One per argument: `*list` passes each element of the list as an argument of its own.
    std::vector<char> spread;
    bool hasSpread = false;

    // Written `a name b` (D72): `name` must be an infix function.
    bool isInfix = false;
    CallExpr() : Expr(ExprKind::Call) {}
};

struct MemberExpr : Expr {
    ExprPtr object;
    std::string name;
    bool safe = false;  // `object?.name`: null when the object is null
    MemberExpr() : Expr(ExprKind::Member) {}
};

struct IndexExpr : Expr {
    ExprPtr object, index;
    // `counts[word, default = 0]`: what a map's entry starts from when its key is not there;
    // null when none is written (L118).
    ExprPtr defaultValue;
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
    bool descending = false;  // `10 downTo 1`
    ExprPtr step;             // `... step 2`; null for a step of one
    RangeExpr() : Expr(ExprKind::Range) {}
};

// `value is Type` and `value !is Type`.
struct IsExpr : Expr {
    ExprPtr value;
    TypeAnnotation type;
    bool negated = false;
    IsExpr() : Expr(ExprKind::Is) {}
};

// `Image#`, the class named Image, or `x#`, the class of the object x (D48); `Box<int>#` with type
// arguments. Which of the two the checker says (Checker::namedClassOf).
struct ClassOfExpr : Expr {
    ExprPtr value;
    std::vector<TypeAnnotation> typeArgs;
    ClassOfExpr() : Expr(ExprKind::ClassOf) {}
};

// `value as Type` insists on the type; `value as? Type` gives null instead of an error.
struct CastExpr : Expr {
    ExprPtr value;
    TypeAnnotation type;
    bool safe = false;
    CastExpr() : Expr(ExprKind::Cast) {}
};

// `value in container` and `value !in container`: container.contains(value).
struct InExpr : Expr {
    ExprPtr value, container;
    bool negated = false;
    InExpr() : Expr(ExprKind::In) {}
};

// `left ?: right`: left, unless it is null.
struct ElvisExpr : Expr {
    ExprPtr left, right;
    ElvisExpr() : Expr(ExprKind::Elvis) {}
};

struct FunctionExpr : Expr {
    std::unique_ptr<FunctionDecl> fn;
    FunctionExpr() : Expr(ExprKind::Function) {}
};

// ---- Statements ----

enum class StmtKind {
    Expr, Var, Block, If, While, ForIn, Return, Break, Continue, Throw, Try, Import, Function, Class, TypeAlias, Extend,
};

struct Stmt : Node {
    const StmtKind kind;

    // For a declaration at the top level of a file: 0 public, 1 internal (its library only),
    // 2 private (its file only).
    uint8_t visibility = 0;
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

    bool isOverride = false;  // a field written `override`, as for FunctionDecl
    bool isAbstract = false;  // `abstract val x`: for a subclass to supply
    bool isLateinit = false;  // `lateinit var x : T`: given its value after the object is made (D36)
    bool isLazy = false;      // `lazy val x = ...`: its value worked out when first read (D37)
    Annotations annotations;  // (D153)
    uint8_t access = 0;       // the access word in front of a field of a class, as for FunctionDecl
    uint8_t setAccess = 0;    // the one in front of `set` under it (`private set`): who may assign

    // A field of a class with accessors written under it, Kotlin's way:
    //   var gap = 4
    //     set(value) { field = value; repaint() }
    // In them `field` is the stored value, which the property has when it has an initial value.
    std::unique_ptr<FunctionDecl> getter, setter;

    // `val (a, b) = pair`: the names the parts of the value go to, `_` for one to skip.
    // Empty for an ordinary declaration.
    std::vector<std::string> pattern;
    ExprPtr init;  // may be null (var only)
    bool isVal = false;
    // `const maxRows = 1000`: a val whose value is worked out when compiling, from literals
    // and other constants, and written into each use (R2, D139).
    bool isConst = false;

    TypeAnnotation type;

    // `external val String.size : int`: a property the C++ provides on a built-in type, and the
    // type parameters it names (`external val <K, V> Map<K, V>.keys : List<K>`).
    bool isExternal = false;
    TypeAnnotation receiverType;
    std::vector<TypeParam> typeParams;

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
    std::string label;      // `outer@ while ...`, for break@outer and continue@outer
    bool doWhile = false;   // `do { ... } while condition`: the body runs before the first test
    ExprPtr condition;
    std::unique_ptr<BlockStmt> body;
    WhileStmt() : Stmt(StmtKind::While) {}
};

struct ForInStmt : Stmt {
    std::string label;
    std::string variable;
    std::vector<std::string> pattern;  // `for (key, value) in pairs`: as for VarStmt
    ExprPtr iterable;
    std::unique_ptr<BlockStmt> body;
    ForInStmt() : Stmt(StmtKind::ForIn) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value;  // may be null

    // The last expression of a lambda literal, which is its value, rather than a `return`
    // somebody wrote.
    bool implicit = false;

    // `return@name value`: leaves the lambda of that name, not the function around it.
    std::string label;
    ReturnStmt() : Stmt(StmtKind::Return) {}
};

struct BreakStmt : Stmt {
    std::string label;  // `break@outer`: the loop of that name rather than the innermost
    BreakStmt() : Stmt(StmtKind::Break) {}
};

struct ContinueStmt : Stmt {
    std::string label;
    ContinueStmt() : Stmt(StmtKind::Continue) {}
};

struct ThrowStmt : Stmt {
    ExprPtr value;
    ThrowStmt() : Stmt(StmtKind::Throw) {}
};

// One `catch` of a try: `catch (e : NotFound) { ... }`. Without a type it catches every error.
struct CatchClause {
    bool hasVariable = false;
    std::string variable;
    TypeAnnotation type;
    std::unique_ptr<BlockStmt> body;
};

struct TryStmt : Stmt {
    std::unique_ptr<BlockStmt> body;
    std::vector<CatchClause> catches;        // empty when there is only a finally
    std::unique_ptr<BlockStmt> finalizer;    // null when there is no finally
    TryStmt() : Stmt(StmtKind::Try) {}
};

// `return`, `throw`, `break` or `continue` where a value is expected, as in
// `val user = find(id) ?: return`. It never produces a value: control goes elsewhere.
struct JumpExpr : Expr {
    StmtPtr jump;
    JumpExpr() : Expr(ExprKind::Jump) {}
};

// An `if` or a `try` where a value is expected: `val w = if wide { 200 } else { 100 }`. Its
// value is that of the block that ran, which is the block's last expression.
struct StatementExpr : Expr {
    StmtPtr statement;
    StatementExpr() : Expr(ExprKind::Statement) {}
};

// One test of a `when` branch: a value the subject equals, `is Type` or `in container`.
// Without a subject it is a condition in its own right.
struct WhenCondition {
    enum class Kind { Value, Is, In };
    Kind kind = Kind::Value;
    bool negated = false;   // !is, !in
    ExprPtr value;          // Value and In
    TypeAnnotation type;    // Is
};

struct WhenBranch : Node {
    std::vector<WhenCondition> conditions;  // any of them selects the branch; empty for `else`
    bool isElse = false;
    StmtPtr body;  // a block, or one statement
};

// when (subject) { 1, 2 -> a;  is Circle -> b;  in 3..9 -> c;  else -> d }
// Its value is that of the branch taken, or null when none is.
struct WhenExpr : Expr {
    ExprPtr subject;          // null for `when { condition -> ... }`
    std::string subjectName;  // `when (val name = subject)`
    std::vector<WhenBranch> branches;
    WhenExpr() : Expr(ExprKind::When) {}
};

struct ImportStmt : Stmt {
    std::string path;

    // The name its contents are reached by: `jade.Button` after `import jade`, `ui.Button`
    // after `import jade as ui`. Empty when the path gives no usable name.
    std::string alias;

    // `import jade.*`: its names can be written bare as well.
    bool all = false;
    ImportStmt() : Stmt(StmtKind::Import) {}
};

struct FunctionStmt : Stmt {
    std::unique_ptr<FunctionDecl> fn;

    // `fun Shape.describe() { ... }`: an extension function. It becomes a method of the type
    // named, a class or a built-in type, for every value of that type; `this` is the value.
    std::string receiver;
    bool receiverNullable = false;  // `fun string?.orEmpty()`: null takes it too
    TypeAnnotation receiverType;    // the receiver in full, type arguments included: `List<T>`

    // `external fun print(vararg items : any?)`: provided by the C++, declared in Astra so that its
    // types and parameter names are known. Only in the standard library; it has no body.
    bool isExternal = false;
    FunctionStmt() : Stmt(StmtKind::Function) {}
};

// One entry of a class header: `val x : T` / `var x : T` declares a field; a bare `x : T`
// is only a parameter of the generated initializer. Either may end in `= value`.
struct HeaderParam : Node {
    std::string name;
    TypeAnnotation type;
    bool isField = false;
    Annotations annotations;  // (D153)

    // With a generated initializer this is the argument's default, making it optional. In a
    // class that writes its own init it is simply the field's initial value.
    ExprPtr defaultValue;
};

// One entry of an enum: `NORTH`, or `NORTH(0, -1)` when the enum has a header.
struct EnumEntry : Node {
    std::string name;
    std::vector<ExprPtr> args;
};

// class Name(val a : T, var b, c) : Super(args) { fields and methods }
// `type Grid = List<List<int>>`, `type Table<T> = Map<string, List<T>>` (D41): another way of
// writing a type, not a new one.
struct TypeAliasStmt : Stmt {
    std::string name;
    std::vector<TypeParam> typeParams;
    TypeAnnotation target;
    TypeAliasStmt() : Stmt(StmtKind::TypeAlias) {}
};

// `extend Person, Car : Weighed { val name : string get() => model }` (D33): types someone else
// wrote fit an interface of this program; names that differ are bridged by property getters.
struct ExtendStmt : Stmt {
    std::vector<TypeAnnotation> types;
    TypeAnnotation interfaceType;
    std::vector<std::unique_ptr<VarStmt>> bridges;  // each with a getter and nothing stored
    ExtendStmt() : Stmt(StmtKind::Extend) {}
};

struct ClassStmt : Stmt {
    std::string name;
    Annotations annotations;     // (D153)
    bool isAnnotation = false;   // `annotation JsonField(val name : string = "")` (D153)

    // `external object Math { ... }`: an object the C++ provides, declared so that its
    // functions are checked and its constants known; nothing of it is compiled.
    bool isExternal = false;
    bool hasStaticBlock = false;  // its one `static { }` block has been read (D67)
    // `enum class Event` or `enum interface Shape` (D39): a fixed set of kinds, the classes and
    // objects that extend it, all declared in its file (Kotlin's sealed class and interface).
    bool isEnumClass = false;

    // The number the compiler knows the class by where private members are concerned
    // (0 until it is given one); see Op::MemberAccess.
    mutable int privateId = 0;

    // For a class declared inside another: the access word in front of it (an AccessWord).
    uint8_t access = 0;

    // `object Name { ... }`: the class is made, then its one instance, and the name is the instance.
    bool isObject = false;

    // `enum Name { A, B }`: a class whose only instances are the entries listed first in its body.
    bool isEnum = false;
    std::vector<EnumEntry> entries;

    // `data Name(val a, val b)`: compared and printed by its header fields, which are also
    // the parts it comes apart into.
    bool isData = false;

    // `value Point(var x : int, var y : int)` (M6.6 part 2): a data class whose values have no
    // identity and never change once made.
    bool isValue = false;

    // Fields can be listed in a header after the name. They are ordinary declared fields,
    // and a class that writes no init of its own gets one that takes the header's entries
    // in order.
    bool hasHeader = false;
    std::vector<HeaderParam> header;
    size_t headerFieldCount = 0;  // the first so many of `fields` come from the header

    // `interface Name { ... }`: methods for classes to supply or inherit; never has instances.
    bool isInterface = false;

    // `abstract class`: may leave members for its subclasses to write, and is never created
    // itself.
    bool isAbstract = false;

    // `init { ... }` blocks, each with the number of fields declared above it: they run when an
    // object is made, in order with the fields' initial values.
    std::vector<std::pair<size_t, std::unique_ptr<BlockStmt>>> initBlocks;

    // What follows the ':' : at most one superclass and any number of interfaces, in the order
    // written. Which is which is settled when the program runs, except that only a superclass
    // can be given arguments. `superName` is the first of them.
    std::vector<std::string> supertypes;
    std::vector<TypeAnnotation> supertypeTypes;  // one per supertype, with its type arguments

    // `class Box<T>`: the type parameters, in reach of the header, the supertypes and the body.
    std::vector<TypeParam> typeParams;

    bool hasSuper = false;
    std::string superName;
    bool hasSuperArgs = false;        // `: Super(...)`: arguments for the superclass initializer
    std::vector<ExprPtr> superArgs;

    std::vector<std::unique_ptr<VarStmt>> fields;  // header fields, then the body's `val`/`var`
    std::vector<std::unique_ptr<VarStmt>> staticFields;  // `static val`/`static var`, in the order written
    std::vector<std::unique_ptr<FunctionDecl>> methods;

    // Classes, enums, interfaces and objects declared in the body. Each is a static member of
    // this class, reached as `Outer.Inner`.
    std::vector<std::unique_ptr<ClassStmt>> nested;
    ClassStmt() : Stmt(StmtKind::Class) {}
};

// `object : Listener { ... }` where a value is expected: a class with no name, and its one instance.
struct ObjectExpr : Expr {
    std::unique_ptr<ClassStmt> cls;
    ObjectExpr() : Expr(ExprKind::Object) {}
};

}  // namespace astra
