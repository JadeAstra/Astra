#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "bytecode.h"
#include "cache.h"

namespace astra {

class VM;
class Checker;

// A name declared at the top level of a file.
struct GlobalName {
    enum Visibility : uint8_t { Public, Internal, Private };

    int slot = -1;
    Visibility visibility = Public;
    bool isVal = false;
    bool declared = false;  // a declaration of it has been seen; otherwise it has only been used
    std::string file;       // the file that declares it

    // A function with several forms (overloads, D56): each form is in a slot of its own, by
    // its signature, and the name's slot holds the set of them, which calls the form a call's
    // arguments fit. Empty for a name declared once.
    bool isFunction = false;
    std::string firstSignature;  // of the first form, until a second one comes
    std::vector<std::pair<std::string, int>> forms;  // signature and slot, in the order written

    int formSlot(const std::string& signature) const {
        for (const auto& [written, at] : forms)
            if (written == signature) return at;
        return -1;
    }
};

// What tells the forms of a name apart: the types of its parameters as written,
// `(float, float)`; one without a type is `*`.
std::string signatureText(const FunctionDecl& fn);

// A type as written, as signatures show it: the last part of a class's name, `List<int>`, `string?`.
std::string annotationText(const TypeAnnotation& t);

// A folder of source files. Its files share their top-level names; other folders see the
// public ones when they import it, and the internal ones too if they belong to the same library.
struct Namespace {
    std::string directory;
    std::string library;
    std::unordered_map<std::string, GlobalName> names;  // public and internal
};

// How long compiling took, by part, in milliseconds: shown when ASTRA_TIMES is set, for the
// budgets of D145. A part inside another of the same kind is counted once.
struct CompileTimes {
    double parse = 0, check = 0, compile = 0;
    double cacheCheck = 0, cacheRead = 0;  // a kept program: its files looked at, and it read in
    int unitsReused = 0, unitsCompiled = 0;  // units of the program put back from the cache, and compiled
    bool preludeAttached = false;            // the standard library came compiled with astra.exe
};
extern CompileTimes compileTimes;

// Set by `astra --attach-prelude`, which compiles the standard library to attach to astra.exe:
// the copy already attached, if any, is not used for that.
extern bool compilingPrelude;

// Reads a whole file; returns false if it cannot be opened. Strips a UTF-8 byte order mark.
bool readFile(const std::string& path, std::string& out);

// Compiles source files into one Program. A session outlives a single compile so the shell
// can keep adding code that sees earlier globals. All of its entry points throw CompileError.
class CompilerSession {
public:
    // The VM supplies interned strings for the constants; it must outlive the program.
    // Every file is parsed and handed to the checker before anything is compiled; without a
    // checker given (`astra check` gives one, to read what it found) the session has its own.
    explicit CompilerSession(VM& vm, Checker* checker = nullptr);
    ~CompilerSession();

    Checker* checker = nullptr;

    Program program;

    // Whether what the type checker finds in the program is a compile error (true but for
    // `astra check`, which lists everything it finds).
    bool stopAtTypeErrors = true;

    // Directories searched for an import that is not found next to the importing file.
    std::vector<std::string> libraryPaths;

    // The files the program was compiled from, and the folders whose files it took, for the
    // cache of compiled programs (D145).
    const std::vector<SourceRead>& sourcesRead() const { return sourcesRead_; }
    std::vector<FolderRead> foldersRead() const;

    // Compiles a file and everything it imports. Returns the entry module index.
    int compileFile(const std::string& path);

    // The standard library compiled, as its image to be attached to astra.exe (D145, point 3);
    // empty if it cannot be made.
    std::string preludeImage();

    // Compiles source text as a new module. In shell mode a trailing expression becomes the
    // module's result.
    int compileSource(const std::string& source, const std::string& name, const std::string& baseDirectory,
                      bool shell = false);

private:
    friend class ModuleCompiler;

    VM& vm_;
    std::unique_ptr<Checker> ownChecker_;

    // For each function or method of the C++ that the standard library declares: how many
    // arguments the C++ takes, how many its declarations cover between them, and the first.
    struct ExternalForms {
        int min = 0, max = 0, lowest = 0, highest = 0;
        std::string file;
        int line = 0, col = 0;
    };
    std::map<std::string, ExternalForms> externalForms_;

    // Names with several forms whose forms have been defined, and whose set is made next.
    std::vector<std::pair<GlobalName*, std::string>> pendingForms_;

    // Each file's statements, parsed once (by its key), and those of source text that is not a
    // file: the checker reads them, then the compiler. They live as long as the session.
    std::unordered_map<std::string, std::unique_ptr<std::vector<StmtPtr>>> parsed_;
    std::vector<std::unique_ptr<std::vector<StmtPtr>>> sources_;
    std::vector<SourceRead> sourcesRead_;
    std::map<std::string, FolderRead> foldersRead_;
    void noteFolder(const std::string& folder, const std::vector<std::filesystem::path>& files);
    std::unordered_set<std::string> gathered_;  // files handed to the checker
    bool gatheringShell_ = false;               // the unit being gathered is an input of the shell

    // What the shell's inputs have imported so far: each input sees what those before it did.
    struct ShellImport {
        Namespace* ns;
        std::string alias, path;
        bool all;
    };
    std::vector<ShellImport> shellImports_;
    void check();
    std::vector<StmtPtr>* parseFile(const std::string& key, const std::string& path, const std::string& name);
    // Files read and parsed ahead, a few at a time on threads of their own, for parseFile to
    // take: the files a file imports, before they are gathered in order.
    struct Prefetched {
        bool read = false;
        std::string source;
        uintmax_t size = 0;
        long long time = 0;
        uint64_t skeleton = 0;
        std::unique_ptr<std::vector<StmtPtr>> statements;
        std::unique_ptr<CompileError> error;
    };
    std::unordered_map<std::string, Prefetched> prefetched_;
    void prefetch(const std::vector<std::pair<std::string, std::string>>& files);  // key and name
    bool findImport(const std::string& path, const std::string& fromDirectory, const std::string& file,
                    std::string& found, bool& folder) const;
    void gather(const std::string& key, const std::string& name, const std::string& directory,
                const std::vector<StmtPtr>& statements, Namespace* ns, bool prelude);
    FunctionProto* compileStatements(const std::vector<StmtPtr>& statements, const std::string& name,
                                     const std::string& directory, const std::string& fileKey, bool shell, Namespace* ns,
                                     bool predeclared = false);

    // ---- Units kept compiled (D145) ----
    // What each module is, for putting a kept unit's modules back: a file's key, "folder:" and
    // the folder's path for the module its files share, "<worker>" and a worker's script; and
    // the index of each.
    std::vector<std::string> moduleKeys_;
    std::unordered_map<std::string, int> moduleByKey_;
    void noteModule(int index, const std::string& key);
    // What compiling each file depended on, noted while gathering: the files it imports (a
    // folder's, all of them), and the other files gathered from its folder, whose names it sees.
    std::unordered_map<std::string, uint64_t> textHashes_;
    std::unordered_map<std::string, uint64_t> skeletons_;  // the hash of what other files see of each
    std::unordered_map<std::string, std::set<std::string>> fileUses_;
    std::unordered_map<std::string, std::string> directoryOf_;
    std::unordered_map<std::string, std::set<std::string>> filesByDirectory_;
    std::vector<std::pair<std::string, uint64_t>> compiledFrom(const std::vector<std::string>& files) const;
    // Units compiled this time, kept once the whole program is; only within compileFile.
    struct ToKeep {
        std::string unit;
        std::vector<std::string> files;
        std::vector<int> modules;
    };
    std::vector<ToKeep> toKeep_;
    bool keepingUnits_ = false;
    // 1: the unit was put back; 0: it was not, and nothing was done; 2: its names were declared
    // already, but it has to be compiled after all.
    int reuseUnit(const std::string& unit, const std::vector<std::string>& files, const std::vector<int>& modules,
                  const std::vector<const std::vector<StmtPtr>*>& statements, const std::vector<std::string>& names,
                  const std::string& directory, Namespace* ns);
    int putBack(const std::string& image, const std::vector<std::string>& files, const std::vector<int>& modules,
                const std::vector<const std::vector<StmtPtr>*>& statements, const std::vector<std::string>& names,
                const std::string& directory, Namespace* ns);
    void numberNames(std::unordered_map<int, std::string>& slotKeys, std::unordered_map<int, std::string>& libraries) const;
    bool unitImage(const ToKeep& unit, const std::unordered_map<int, std::string>& slotKeys,
                   const std::unordered_map<int, std::string>& libraries, std::string& head,
                   std::vector<const FunctionProto*>& protos);
    static std::string preludeSource();
    bool usePrelude(const std::string& prelude, const std::string& image);
    void keepUnits();
    int slotFor(const std::string& key);
    // Before checking: the units whose code will be put back, whose bodies are then not checked
    // again; and, for one that could not be after all, its bodies checked before it is compiled.
    std::map<std::string, std::vector<std::string>> foldersGathered_;  // a folder imported whole, and its files
    std::unordered_map<std::string, KeptUnit> planned_;
    std::unordered_map<std::string, std::vector<std::string>> plannedFiles_;
    std::unordered_map<std::string, uint64_t> summaries_;  // each file's, as this compile has it
    std::vector<std::string> dependencies(const std::vector<std::string>& files) const;
    void settleReuse();
    void planReuse();
    void planUnit(const std::string& unit, const std::vector<std::string>& files);
    void checkSkipped(const std::vector<std::string>& files);
    void reportNew(size_t before);
    int libraryNumber(const std::string& library, const std::string& file, int line);
    std::unordered_map<std::string, std::unique_ptr<Namespace>> namespaces_;  // by folder
    Namespace* builtins_ = nullptr;                                           // the prelude and the built-in names
    std::unordered_map<std::string, std::unordered_map<std::string, GlobalName>> privates_;  // by file: its private names
    std::unordered_map<int, std::string> globalClassTypes_;                   // global slot -> the class it is declared as
    std::unordered_map<std::string, std::vector<std::string>> enums_;         // enum (see enumKey) -> its entries

    // Assignments to globals, checked against `val` once everything has been compiled.
    std::vector<std::tuple<const GlobalName*, std::string, std::string, int, int>> globalAssignments_;  // name, file, line, col
    std::unordered_map<std::string, int> modules_;  // canonical path -> module index
    int classCount_ = 0;  // classes and libraries given a number so far (see Op::MemberAccess)
    std::unordered_map<std::string, int> libraryIds_;  // library -> its number

    // For a file loaded as part of its folder: the modules of that folder, in the order they
    // run. Importing the file alone runs them all, since its classes are set up by the first.
    std::unordered_map<std::string, std::shared_ptr<std::vector<int>>> units_;

    // A file that has been read and parsed but not compiled yet.
    struct Pending;
    Namespace* namespaceOf(const std::string& directory);
    int newSlot(const std::string& name);
    void predeclare(Namespace& ns, const std::string& fileKey, const std::string& file,
                    const std::vector<StmtPtr>& statements, bool shell);
    void loadFolder(const std::string& folder, const std::string& displayPath, const std::string& file, const Node& at,
                    std::vector<int>& modules);

    // `into`: the namespace the file's names go in, if not that of its folder.
    FunctionProto* compileUnit(const std::string& source, const std::string& name, const std::string& directory,
                               const std::string& fileKey, bool shell, Namespace* into = nullptr);
    // The script of a worker, compiled into the program. Returns its module index.
    int loadWorker(const std::string& path, const std::string& fromDirectory, const std::string& file, const Node& at);
    int loadModule(const std::string& path, const std::string& displayPath, const std::string& file, const Node* at);

    // Loads what an `import` names: a file, or every file of a folder. Gives the modules to
    // run, in order, and returns the namespace whose names the importer now sees.
    Namespace* importModule(const std::string& path, const std::string& fromDirectory, const std::string& file,
                            const Node& at, std::vector<int>& modules);
    void checkConstAssignments();
};

}  // namespace astra
