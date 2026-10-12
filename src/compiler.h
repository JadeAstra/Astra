#pragma once

#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "bytecode.h"

namespace astra {

class VM;

// Reads a whole file; returns false if it cannot be opened. Strips a UTF-8 byte order mark.
bool readFile(const std::string& path, std::string& out);

// Compiles source files into one Program. A session outlives a single compile so the REPL
// can keep adding code that sees earlier globals. All of its entry points throw CompileError.
class CompilerSession {
public:
    // The VM supplies interned strings for the constants; it must outlive the program.
    explicit CompilerSession(VM& vm);

    Program program;

    // Directories searched for an import that is not found next to the importing file.
    std::vector<std::string> libraryPaths;

    // Compiles a file and everything it imports. Returns the entry module index.
    int compileFile(const std::string& path);

    // Compiles source text as a new module. In REPL mode a trailing expression becomes the
    // module's result.
    int compileSource(const std::string& source, const std::string& name, const std::string& baseDirectory,
                      bool repl = false);

private:
    friend class ModuleCompiler;

    VM& vm_;
    std::unordered_map<std::string, int> globals_;
    std::unordered_set<std::string> constGlobals_;
    std::vector<std::tuple<std::string, std::string, int, int>> globalAssignments_;  // name, file, line, col
    std::unordered_map<std::string, int> modules_;  // canonical path -> module index

    FunctionProto* compileUnit(const std::string& source, const std::string& name, const std::string& directory,
                               bool repl);
    int loadModule(const std::string& path, const std::string& displayPath, const std::string& file, const Node* at);
    int importModule(const std::string& path, const std::string& fromDirectory, const std::string& file,
                     const Node& at);
    int globalSlot(const std::string& name);
    int declareGlobal(const std::string& name, bool isVal, const std::string& file, const Node& at);
    void checkConstAssignments();
};

}  // namespace astra
