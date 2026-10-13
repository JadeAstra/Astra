#pragma once

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "bytecode.h"

namespace astra {

class VM;

// A name declared at the top level of a file.
struct GlobalName {
    enum Visibility : uint8_t { Public, Internal, Private };

    int slot = -1;
    Visibility visibility = Public;
    bool isVal = false;
    bool declared = false;  // a declaration of it has been seen; otherwise it has only been used
    std::string file;       // the file that declares it
};

// A folder of source files. Its files share their top-level names; other folders see the
// public ones when they import it, and the internal ones too if they belong to the same library.
struct Namespace {
    std::string directory;
    std::string library;
    std::unordered_map<std::string, GlobalName> names;  // public and internal
};

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
    std::unordered_map<std::string, std::unique_ptr<Namespace>> namespaces_;  // by folder
    Namespace* builtins_ = nullptr;                                           // the prelude and the built-in names
    std::unordered_map<std::string, std::unordered_map<std::string, GlobalName>> privates_;  // by file: its private names
    std::unordered_map<int, std::string> globalClassTypes_;                   // global slot -> the class it is declared as
    std::unordered_map<std::string, std::vector<std::string>> enums_;         // enum (see enumKey) -> its entries

    // Assignments to globals, checked against `val` once everything has been compiled.
    std::vector<std::tuple<const GlobalName*, std::string, std::string, int, int>> globalAssignments_;  // name, file, line, col
    std::unordered_map<std::string, int> modules_;  // canonical path -> module index

    // For a file loaded as part of its folder: the modules of that folder, in the order they
    // run. Importing the file alone runs them all, since its classes are set up by the first.
    std::unordered_map<std::string, std::shared_ptr<std::vector<int>>> units_;

    // A file that has been read and parsed but not compiled yet.
    struct Pending;
    Namespace* namespaceOf(const std::string& directory);
    int newSlot(const std::string& name);
    void predeclare(Namespace& ns, const std::string& fileKey, const std::string& file,
                    const std::vector<StmtPtr>& statements, bool repl);
    void loadFolder(const std::string& folder, const std::string& displayPath, const std::string& file, const Node& at,
                    std::vector<int>& modules);

    FunctionProto* compileUnit(const std::string& source, const std::string& name, const std::string& directory,
                               const std::string& fileKey, bool repl);
    int loadModule(const std::string& path, const std::string& displayPath, const std::string& file, const Node* at);

    // Loads what an `import` names: a file, or every file of a folder. Gives the modules to
    // run, in order, and returns the namespace whose names the importer now sees.
    Namespace* importModule(const std::string& path, const std::string& fromDirectory, const std::string& file,
                            const Node& at, std::vector<int>& modules);
    void checkConstAssignments();
};

}  // namespace astra
