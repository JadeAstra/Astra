#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast.h"

namespace astra {

struct Namespace;

// A type as the compiler writes it into a program for when it runs (M6.6 part 1): what a list
// or map literal is made to hold, and the like.
struct RuntimeType {
    enum Kind : uint8_t { Unknown, Any, Int, Float, Number, String, Bool, Bytes, Range, TypeValue, Function, List, Map, Class, TypeParam, Char, Byte, Index, Set };
    Kind kind = Unknown;
    bool nullable = false;
    uint8_t storage = 0;  // Int or Float kept at a storage width (D148): kTypeInt16 and the rest
    bool frozen = false;  // a List, Map or Set: FrozenList and the others (D26)
    // A class: where its global is, in a namespace by `global` or, for a private one, in the
    // file `privateFile`; its type arguments in `args`.
    const Namespace* ns = nullptr;
    std::string privateFile;
    std::string global;
    bool value = false;     // a class that is a value type
    std::string typeParam;  // a type parameter: its name
    std::vector<RuntimeType> args;
};

// The value of a constant (R2, D139), worked out when the program is checked.
struct ConstValue {
    enum Kind : uint8_t { None, Int, Float, Bool, String, Char, Byte };  // a Char's or Byte's in `i`
    Kind kind = None;
    int64_t i = 0;
    double d = 0;
    bool b = false;
    std::string s;
};

// One mistake the type checker found.
struct CheckDiagnostic {
    std::string file;
    int line = 0;
    int col = 0;
    std::string message;
};

// The type checker (M6.5): a pass of its own over the whole program, after it has been
// parsed and before it is compiled, that works out the type of every expression and reports
// what cannot be right (a wrong argument, a member that does not exist, an int put where a
// string goes, a value that may be null used as if it could not be).
//
// What it finds is a compile error: the program does not run (M6.5 part 3, step 5). `astra
// check` lists all of it. A parameter has its type written; a local's type is worked out from
// its first value, and a value whose type cannot be known is `any`, used through a cast.
// A member of a class as reflection lists it (D38), for the description the compiler writes.
struct ReflectedMember {
    std::string name;
    const Annotations* annotations = nullptr;  // those written on it (D153)
    const FunctionDecl* fn = nullptr;   // a method's or an init's; null for a field and a generated init
    RuntimeType type;                   // a field's type, a method's result; Unknown where it gives nothing
    int flags = 0;                      // a field's: 1 it may be set, 2 a property worked out by its getter
    std::vector<std::string> paramNames;
    std::vector<RuntimeType> params;
    std::vector<bool> optional;         // the parameter has a default
    std::vector<const Annotations*> paramAnnotations;  // each parameter's, where any are written
};
struct ReflectedClass {
    std::vector<ReflectedMember> fields, methods, constructors;
    std::vector<RuntimeType> kinds;     // an enum class's (D39)
};

class Checker {
public:
    Checker();
    ~Checker();

    // A file of the program, once it has been parsed. The statements stay the caller's, and
    // must outlive the checker. `ns` is the namespace its top-level names go in.
    void addUnit(const std::vector<StmtPtr>& statements, const std::string& file, const std::string& fileKey,
                 Namespace* ns, bool prelude, bool shell = false);

    // The shell's last input, taken back because it did not compile: the names it declared
    // are as they were before it.
    void forgetShellUnit();

    // A file whose compiled code is put back from the cache (D145), with nothing it was compiled
    // from changed: its declarations are read, but its bodies are not checked again. What was
    // learned from them that other files rely on, which of its functions run a lambda they are
    // given there and then (R1), is handed back as inPlaceFacts gave it. checkBodiesOf checks
    // such a file after all, when its code could not be put back.
    void skipBodiesOf(const std::string& fileKey, std::string inPlaceFacts);
    void checkBodiesOf(const std::string& fileKey);
    std::string inPlaceFacts(const std::string& fileKey);
    // What other files can learn of a file's functions only from their bodies: the result
    // types not written, and the lambda parameters run there and then. With the file's text
    // outside those bodies it makes the file's summary (D145).
    std::string summaryFacts(const std::string& fileKey);

    // An `import` in a file: what it imports, and how the file reaches it.
    void addImport(const std::string& fileKey, const std::string& alias, bool all, Namespace* ns);

    // Checks what was added since the last run: what a file uses must have been added by
    // then. Diagnostics are in the order found, without repeats.
    void run();

    std::vector<CheckDiagnostic> diagnostics;

    // Whether what is found in the standard library is reported too (`astra check --std`).
    bool reportStandardLibrary = false;

    // A parameter of a named function, a method or a class header has a type written (M6.5
    // part 3, step 5).
    bool requireTypes = true;

    // For a read of a constant (`maxRows`, `Limits.max`), or of a val whose value is constant
    // and the same for every read (top level, object, static or local): its value, to be
    // written in its place. False for anything else.
    bool constantAt(const Expr& at, ConstValue& out);

    // For an expression made only of literals and constants (`rows * 2`, `"v" + major`): its
    // value, worked out as the VM would. False for anything else, and where working it out
    // would fail (a division by zero), which is left to happen when the program runs.
    bool folded(const Expr& e, ConstValue& out);
    // Whether `e` is an int constant given where a byte goes, and which byte (D19).
    bool byteAt(const Expr& e, uint8_t& out) const;
    // For a call of a C++ function with named arguments or a lambda after it: the argument
    // each parameter takes in turn (-1 for one not given, passed as null); null otherwise.
    const std::vector<int>* argumentOrder(const CallExpr& call) const;

    // For a call of a name with several forms (overloads, D56): the declaration of the form it
    // means, where that could be told; null otherwise, and the form is chosen when it runs.
    const FunctionDecl* chosenForm(const CallExpr& call) const;
    // A call of a class's name that chose a secondary constructor: the static function made of it.
    const FunctionDecl* secondaryConstructor(const CallExpr& call) const;
    // Bundles of named arguments (D143): the names `...bundle` passes on at a call; whether a
    // call is params(...), a bundle's copy(...), or `+` is of two bundles.
    const std::vector<std::string>* bundleExpansion(const Expr& bundle) const;
    bool bundleMake(const Node& at) const;
    bool bundleCopy(const Node& at) const;
    bool bundleAdd(const Node& at) const;

    // Whether an object of the class can change in what its == and hashCode see, so that it
    // cannot be a key of a map or a set (D26): the compiler marks the class so for the checks
    // made when the program runs.
    bool unstableKey(const ClassStmt& cls);

    // A kind of an enum class named bare where one is expected (D39): `Click(1, 2)` for an Event,
    // `is Click`. For such a name or type, the class's full name (`Event.Click`); null otherwise.
    const std::string* kindName(const Node& at) const;

    // A type written with a name `type` gave it (D41): the type it names, written out, for the
    // checks made when the program runs; null for any other.
    const TypeAnnotation* aliasExpansion(const TypeAnnotation& use) const;

    // A data class extending a data class (D65): how many of its header's fields are its
    // parent's, which it starts with; 0 for any other class.
    size_t inheritedDataFields(const ClassStmt& cls);

    // For a method whose name has several forms among the classes it is related to: the
    // declaration, as high up as it goes, of the form it is (the one it overrides, or itself),
    // whose signature it is known by when the program runs. Null where the name has one form.
    const FunctionDecl* formRoot(const FunctionDecl& method);

    // The forms of a method name a class has, its own and those it inherits, by their roots;
    // empty where the name has one form.
    std::vector<const FunctionDecl*> formRoots(const ClassStmt& cls, const std::string& name);

    // For a list or map literal: the type it is made as, where that is known.
    bool literalType(const Expr& literal, RuntimeType& out) const;
    // `val out : open List<T> = []`: the type it is declared as, which names a type parameter.
    bool declaredLiteralType(const Expr& literal, RuntimeType& out) const;

    // For a call of a generic function or class written in Astra: what it gives the type
    // parameters, in their order, an Unknown where that was not worked out.
    bool typeArguments(const Node& call, std::vector<RuntimeType>& out) const;

    // For a call of a function the C++ provides (`external`): what its declaration says it
    // gives, its type parameters filled in, and its name; false where that is nothing, or not
    // known.
    bool externalResult(const Node& call, RuntimeType& out, std::string& name) const;

    // For `pts.add(Point(1, 2))` on a list of a value type: whether the value made is the
    // list's item type, so that it can be made in the list (M6.6 part 3).
    bool addsNewValue(const Node& call) const;

    // Whether an expression of a value type is a place a change is made through (M6.6 part 4):
    // `p` in `p.x = 4` for a var p, `pts[i]`, `obj.pos`.
    bool isValuePlace(const Expr& e) const;

    // `5.seconds`: a read of an extension property, compiled as a call of its getter.
    bool extensionGetter(const MemberExpr& e) const;

    // An atomic field's change (D92), one operation: `x.hits += 1`, `best = max(best, v)`,
    // `x.hits.compareAndSet(a, b)`. `object` null for the object the code runs on.
    struct AtomicFieldOp {
        uint8_t kind = 0;  // 0 add, 1 subtract, 2 max, 3 min, 4 compare and set
        const Expr* object = nullptr;
        std::string field;
        const Expr* operand = nullptr;
        const Expr* expected = nullptr;
    };
    bool atomicField(const Expr& e, AtomicFieldOp& out) const;

    // A for through an Iterator: 0 any other; 1 over an Iterator itself; 2 over what an object's
    // iterator() gives.
    int protocolLoop(const ForInStmt& loop) const;

    // `Image#` and `x#` (D48): 0 where it is the class of the object x; 1 where it names a class
    // or a built-in type, written in `out`; 2 where it names a class declared inside a function,
    // whose name gives the class itself.
    int namedClassOf(const ClassOfExpr& e, RuntimeType& out) const;
    // A class's own members, as reflection lists them (D38); false where the class is not known.
    bool reflected(const ClassStmt& cls, ReflectedClass& out);

    // Where an item of a list of values is read where it is, not copied (M6.6 part 4,
    // value-lists.html section 6): `pts[i]`, a for loop's variable; and the parameters, and
    // `this`, a function copies as it starts, should it be given an item where it is.
    bool readsInPlace(const IndexExpr& index) const;
    // Whether a field read (a member, or a bare name in a method) gives a value kept inside its
    // object where it is (M6.6 part 5): where nothing that may change values of its type runs
    // while it is in use.
    // A lambda run on an object its type says (`Grid.() -> unit`): `this` in it is that object (D58).
    bool runsOnObject(const FunctionDecl& lambda) const;
    // A lambda without an arrow given a function type that takes nothing: it has no `it`, which
    // is then that of a lambda around it (D55).
    bool hasNoIt(const FunctionDecl& lambda) const;
    // A lambda given where an interface with one abstract method is expected (D28): the
    // interface as the running program names it, and the method the lambda becomes.
    bool samConversion(const Expr& lambda, RuntimeType& iface, std::string& method, int& arity) const;
    // `Action { ... }`, an interface with one abstract method called with a lambda: the call is
    // that lambda, given as the interface (D28).
    bool samCall(const CallExpr& call) const;
    // A call of the function it is written in, on the same object, which nothing overrides:
    // as its last step, a loop (D29).
    bool callsItself(const CallExpr& call) const;
    // Any call of the function it is written in, on the same object; `overridden` when a
    // subclass has one of its own (D29, for `tail fun`).
    bool selfCall(const CallExpr& call, bool& overridden) const;

    // A call of an inline function (D59): the function, which parameter each argument goes to,
    // and the file it is written in, whose names its body sees.
    struct InlineCall {
        const FunctionDecl* fn = nullptr;
        std::vector<int> paramOf;
        std::string fileKey;
        Namespace* ns = nullptr;
        const ClassStmt* owner = nullptr;  // a member's class
        std::string ownerName;             // as the program names it: `Outer.Inner`
        bool extension = false;
        bool prelude = false;              // the standard library's
    };
    bool inlineCall(const CallExpr& call, InlineCall& out) const;
    // Whether a lambda given for this parameter of an inline function is copied in with it: the
    // body only calls it, or passes it on to an inline function that copies it in (D59).
    bool copiedIn(const FunctionDecl& fn, int param) const;
    // `block(obj, ...)` and `obj.block(...)`: a function value of a type run on an object,
    // called on one (D58).
    bool callsOnObject(const CallExpr& call) const;
    bool memberLocalCall(const CallExpr& call) const;
    // `a downTo b` or `r step n`, the standard library's (D72): which of them.
    bool rangeWord(const CallExpr& call, std::string& which) const;
    bool readsFieldInPlace(const Node& read) const;

    // A field written without its type: the type the checker works out for it, where that is an
    // int, a float or a bool, so that its objects keep it as a payload (M6.6 part 6).
    bool inferredFieldType(const VarStmt& field, RuntimeType& out) const;
    // A field with no type written whose value is of a value type, null or not (G16).
    bool inferredValueFieldType(const VarStmt& field, RuntimeType& out) const;
    bool loopsInPlace(const ForInStmt& loop) const;
    bool loopPassesOn(const ForInStmt& loop) const;  // its variable's one use is to be passed on
    std::vector<int> paramsOwned(const FunctionDecl& fn) const;
    // Whether a lambda gives a value to a variable of the code around it (`saved = it`).
    bool keepsOutside(const FunctionDecl& lambda) const;
    // An inline function's own loop that may read its items where they are and hand them to the
    // lambdas it was given (G15): every use of its variable an argument or what is returned; the
    // lists a function or lambda may change, the functions it was given counted or not; and
    // whether a lambda may give back a value (the item where it is, perhaps).
    bool loopPassesAllOn(const ForInStmt& loop) const;
    bool changesLists(const FunctionDecl& fn, bool countGiven) const;
    bool mayGiveValue(const FunctionDecl& lambda) const;
    bool ownsThis(const FunctionDecl& fn) const;

    // Results built where they go (M6.6 part 4): whether a function or lambda gives a value of a
    // value type, so may be given where its result goes; whether a call makes a value of a
    // value type; whether `pts[i] = f(...)` may build the result in the item.
    bool returnsValue(const FunctionDecl& fn) const;
    bool constructsValue(const CallExpr& call) const;
    bool buildsInPlace(const AssignExpr& assign) const;

    // For an extension function: its receiver's type as written; null for anything else.
    const TypeAnnotation* extensionReceiver(const FunctionDecl& fn) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace astra
