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
    enum Kind : uint8_t { Unknown, Any, Int, Float, Number, String, Bool, Bytes, Range, TypeValue, Function, List, Map, Class, TypeParam };
    Kind kind = Unknown;
    bool nullable = false;
    // A class: where its global is, in a namespace by `global` or, for a private one, in the
    // file `privateFile`; its type arguments in `args`.
    const Namespace* ns = nullptr;
    std::string privateFile;
    std::string global;
    std::string typeParam;  // a type parameter: its name
    std::vector<RuntimeType> args;
};

// The value of a constant (R2, D139), worked out when the program is checked.
struct ConstValue {
    enum Kind : uint8_t { None, Int, Float, Bool, String };
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

    // For a call of a name with several forms (overloads, D56): the declaration of the form it
    // means, where that could be told; null otherwise, and the form is chosen when it runs.
    const FunctionDecl* chosenForm(const CallExpr& call) const;

    // For a method whose name has several forms among the classes it is related to: the
    // declaration, as high up as it goes, of the form it is (the one it overrides, or itself),
    // whose signature it is known by when the program runs. Null where the name has one form.
    const FunctionDecl* formRoot(const FunctionDecl& method);

    // The forms of a method name a class has, its own and those it inherits, by their roots;
    // empty where the name has one form.
    std::vector<const FunctionDecl*> formRoots(const ClassStmt& cls, const std::string& name);

    // For a list or map literal: the type it is made as, where that is known.
    bool literalType(const Expr& literal, RuntimeType& out) const;

    // For a call of a generic function or class written in Astra: what it gives the type
    // parameters, in their order, an Unknown where that was not worked out.
    bool typeArguments(const Node& call, std::vector<RuntimeType>& out) const;

    // For a call of a function the C++ provides (`external`): what its declaration says it
    // gives, its type parameters filled in, and its name; false where that is nothing, or not
    // known.
    bool externalResult(const Node& call, RuntimeType& out, std::string& name) const;

    // For an extension function: its receiver's type as written; null for anything else.
    const TypeAnnotation* extensionReceiver(const FunctionDecl& fn) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace astra
