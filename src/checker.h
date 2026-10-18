#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast.h"

namespace astra {

struct Namespace;

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
// For now it runs beside the old rules, with `astra check`, and stops nothing: where a type
// is not written and cannot be worked out, it is unknown, and an unknown type is never an
// error. As more of the program carries its types, more is checked.
class Checker {
public:
    Checker();
    ~Checker();

    // A file of the program, once it has been parsed. The statements stay the caller's, and
    // must outlive the checker. `ns` is the namespace its top-level names go in.
    void addUnit(const std::vector<StmtPtr>& statements, const std::string& file, const std::string& fileKey,
                 Namespace* ns, bool prelude);

    // An `import` in a file: what it imports, and how the file reaches it.
    void addImport(const std::string& fileKey, const std::string& alias, bool all, Namespace* ns);

    // Checks what was added since the last run: what a file uses must have been added by
    // then. Diagnostics are in the order found, without repeats.
    void run();

    std::vector<CheckDiagnostic> diagnostics;

    // Whether what is found in the standard library is reported too (`astra check --std`).
    bool reportStandardLibrary = false;

    // The rule of M6.5 part 3, step 5, before it is switched on (`astra check --typed`): a
    // parameter of a named function, a method or a class header has a type written.
    bool requireTypes = false;

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

    // For an extension function: its receiver's type as written; null for anything else.
    const TypeAnnotation* extensionReceiver(const FunctionDecl& fn) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace astra
