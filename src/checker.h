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
// parsed and compiled, that works out the type of every expression and reports what cannot
// be right (a wrong argument, a member that does not exist, an int put where a string goes,
// a value that may be null used as if it could not be).
//
// For now it runs beside the old rules, with `astra check`, and stops nothing: where a type
// is not written and cannot be worked out, it is unknown, and an unknown type is never an
// error. As more of the program carries its types, more is checked.
class Checker {
public:
    Checker();
    ~Checker();

    // A file of the program, once it has been parsed (and compiled). The checker keeps its
    // syntax tree. `ns` is the namespace its top-level names go in.
    void addUnit(std::vector<StmtPtr> statements, const std::string& file, const std::string& fileKey, Namespace* ns,
                 bool prelude);

    // An `import` in a file: what it imports, and how the file reaches it.
    void addImport(const std::string& fileKey, const std::string& alias, bool all, Namespace* ns);

    // Checks everything added. Diagnostics are in the order found, without repeats.
    void run();

    std::vector<CheckDiagnostic> diagnostics;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace astra
