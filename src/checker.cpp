// The type checker: see checker.h.
//
// It works in two steps. First every file's declarations are gathered: classes with their
// fields, methods, statics, nested classes and type parameters; top-level functions, values
// and variables; extension functions. Then every body is checked, each expression given a
// type. Types not written are worked out where they can be (a variable from its initial
// value, a function's result from its body, a generic's type arguments from what it is
// given, a lambda's parameters from where it is passed), on demand and quietly, so that what
// is found while working one out is reported once, when that body's own turn comes.
//
// Everything the checker cannot see into is Unknown, and Unknown fits everything: the
// built-in functions and the members of the built-in types (until their declarations are
// written in M6.5's part 3), names in a lambda passed where the checker cannot tell whether
// it runs on an object, and anything not written whose type cannot be worked out. Inside a
// generic, a type parameter is as lenient as Unknown; at each use it is replaced by what it
// stands for there, and that is checked.

#include "checker.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <functional>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "compiler.h"
#include "value.h"

#include "section_compiler.h"  // last: this file's own code goes in its section (D161)

namespace astra {

namespace {

struct ClassInfo;

// ---- Types ----

struct FuncInfo;

struct Type {
    enum Kind : uint8_t {
        Unknown,    // not known: fits everything, and everything fits it
        Any,        // any value but null (`any?` takes null too)
        Nothing,    // never a value: a jump, a function that never returns
        Unit,       // what a function that gives nothing gives
        Null,       // the type of `null` alone
        Bool, Int, Float, Number, String, Bytes, Range,
        TypeValue,  // `Type`: a type, as the program runs (M6.6 part 1)
        List, Map,
        Function,
        Class,
        TypeParam,  // `T` inside the class or function that declares it
        Char,       // one character (D24)
        Byte,       // 0 to 255 (D19)
        Index,      // a position: an int, or ^k from the end (D146)
        Set,        // Set [T] (M6.6 part 8)
        Union,      // `int | string` (D40): the types it may be in args; `nullable` for null among them
        Bundle,     // `params(bold = true, size = 14)` (D143): its names in `fields`, their types in args
    };
    Kind kind = Unknown;
    bool nullable = false;
    // An Int or Float kept at a storage width (D148), kTypeInt16 and the rest: read as an int or
    // float, an int or float fitting it, its range checked where it is kept.
    uint8_t storage = 0;
    bool frozen = false;               // List, Map, Set: FrozenList and the others, which nobody may change (D26)
    // List, Map, Set: `open List<int>`, which may be changed through this name; without it, one
    // that may only be read (D32).
    bool open = false;
    bool accepts = false;              // a type argument of an open type: `accepts Button` (D34)
    ClassInfo* cls = nullptr;          // Class
    std::vector<Type> args;            // List [T], Map [K, V], Class: its type arguments, Function: the parameters
    std::shared_ptr<Type> result;      // Function: the result
    std::shared_ptr<Type> receiver;    // Function: the object it runs on (`Grid.() -> unit`)
    bool knownParams = false;          // Function: whether the parameters are known
    int required = -1;                 // Function: how many of them a call must give, those after having defaults; -1 all
    bool suspend = false;              // Function: `suspend () -> unit`, a function that may wait (R1)
    std::vector<Type> throws;          // Function: the checked exceptions it may throw, as written (D52)
    FuncInfo* fnRef = nullptr;         // Function: the function this value is, where it is known (D35)
    std::string name;                  // TypeParam: its name
    std::vector<std::string> fields;   // Bundle: its names, in the order of `args`
    std::shared_ptr<Type> bound;       // TypeParam: its bound, when one is written

    static Type of(Kind k) {
        Type t;
        t.kind = k;
        return t;
    }
    bool unknown() const { return kind == Unknown; }
    bool known() const { return kind != Unknown; }
};

Type unknownType() { return Type{}; }

// A value as it is read: a storage width is an int or a float once read (D148); what is read
// through `accepts Button` may be anything above a Button (D34).
Type readType(Type t) {
    if (t.accepts) {
        Type any = Type::of(Type::Any);
        any.nullable = true;
        return any;
    }
    t.storage = 0;
    return t;
}

Type withNull(Type t, bool nullable = true) {
    if (t.kind == Type::Unknown || t.kind == Type::Null || t.kind == Type::Nothing) return t;
    t.nullable = nullable;
    return t;
}

Type listOf(Type item) {
    Type t = Type::of(Type::List);
    t.args.push_back(std::move(item));
    return t;
}

// A list made here, which its maker may change (D32).
Type openListOf(Type item) {
    Type t = listOf(std::move(item));
    t.open = true;
    return t;
}

// What a generic's type parameters stand for at one use: `T` -> int for a Box<int>.
using Env = std::unordered_map<std::string, Type>;

// A type with the type parameters it names replaced by what `env` gives them.
Type subst(const Type& t, const Env& env) {
    if (env.empty()) return t;
    if (t.kind == Type::TypeParam) {
        auto found = env.find(t.name);
        if (found == env.end()) return t;
        Type r = found->second;
        if (t.nullable) r = withNull(r);
        return r;
    }
    if (t.args.empty() && !t.result && !t.receiver) return t;
    Type r = t;
    for (Type& a : r.args) a = subst(a, env);
    if (t.result) r.result = std::make_shared<Type>(subst(*t.result, env));
    if (t.receiver) r.receiver = std::make_shared<Type>(subst(*t.receiver, env));
    return r;
}

// Whether a type still names a type parameter somewhere in it.
bool mentionsTypeParam(const Type& t) {
    if (t.kind == Type::TypeParam) return true;
    for (const Type& a : t.args)
        if (mentionsTypeParam(a)) return true;
    if (t.result && mentionsTypeParam(*t.result)) return true;
    if (t.receiver && mentionsTypeParam(*t.receiver)) return true;
    return false;
}

// ---- What the program declares ----

struct Unit;

struct FuncInfo {
    std::string name;
    const FunctionDecl* decl = nullptr;  // null for a class's generated initializer
    Unit* unit = nullptr;
    ClassInfo* owner = nullptr;          // a method's class
    bool isStatic = false;
    bool isExtension = false;
    bool isExternal = false;             // provided by the C++, declared with `external`
    const FunctionStmt* stmt = nullptr;  // an extension's declaration
    Type receiver;                       // an extension's receiver

    std::vector<std::string> typeParams;
    std::vector<std::string> paramNames;
    std::vector<Type> params;
    std::vector<bool> hasDefault;
    int vararg = -1;
    Type varargItem;

    bool resultWritten = false;
    Type result;
    int resultState = 0;  // 0 not worked out, 1 being worked out, 2 known
    bool resolved = false;

    bool isLambda() const { return decl != nullptr && decl->isLambda; }
};

struct FieldInfo {
    std::string name;
    const VarStmt* decl = nullptr;
    const HeaderParam* header = nullptr;
    Unit* unit = nullptr;
    ClassInfo* owner = nullptr;
    bool isVal = false;
    bool typeWritten = false;
    Type type;
    int state = 0;  // as FuncInfo::resultState
    FuncInfo* getterFn = nullptr;  // the accessor written under it, once its class is checked
};

struct ClassInfo {
    std::string name;
    const ClassStmt* decl = nullptr;
    Unit* unit = nullptr;
    ClassInfo* outer = nullptr;
    // A class declared inside a function, or an anonymous object's: what `this@Name` may name
    // around it, innermost first (D57).
    std::vector<std::pair<std::string, Type>> outerThis;
    bool isInterface = false, isObject = false, isEnum = false, isData = false, isAbstract = false;
    bool isValue = false;  // `value Point(...)` (M6.6 part 2)
    bool isEnumClass = false;  // `enum class Event`, `enum interface Shape` (D39)
    std::vector<std::string> typeParams;
    // Declared at the top of a file, or inside a class that is: its class is a global, which the
    // compiler can name in a type it writes for when the program runs (M6.6 part 1).
    bool global = false;

    ClassInfo* superclass = nullptr;
    std::vector<Type> superArgs;          // what the superclass's type parameters are given
    std::vector<ClassInfo*> interfaces;
    std::vector<std::vector<Type>> interfaceArgs;
    bool unknownSuper = false;  // a supertype the checker cannot see: unknown members may come from it
    bool supersResolved = false;

    std::unordered_map<std::string, FieldInfo*> fields;
    std::unordered_map<std::string, FieldInfo*> statics;
    std::unordered_map<std::string, std::vector<FuncInfo*>> methods;
    std::unordered_map<std::string, std::vector<FuncInfo*>> staticMethods;
    std::unordered_map<std::string, FuncInfo*> getters;  // `get name()`: a property
    std::unordered_map<std::string, FuncInfo*> setters;  // `set name(value)`
    std::vector<FuncInfo*> inits;
    bool headerInit = false;          // the generated initializer, from the header
    FuncInfo* headerFunc = nullptr;   // ... as a function, once worked out
    std::unordered_map<std::string, ClassInfo*> nested;
};

struct ImportInfo {
    std::string alias;
    bool all = false;
    Namespace* ns = nullptr;
};

// What a top-level name of a file stands for.
struct GlobalSym {
    FieldInfo* var = nullptr;            // a val or var
    std::vector<FuncInfo*> funcs;        // a function
    ClassInfo* cls = nullptr;            // a class, interface, enum or object
    const TypeAliasStmt* alias = nullptr;  // `type Grid = ...` (D41)
    Unit* aliasUnit = nullptr;
};

struct Unit {
    std::string file, fileKey;
    Namespace* ns = nullptr;
    bool prelude = false;
    bool shell = false;  // one input of the shell, which may declare a name again
    // Its bodies are not checked: its compiled code is put back from the cache (D145). What
    // they say of its functions' lambda parameters comes with it, one line per function.
    bool skipBodies = false;
    std::vector<std::string> inPlaceFacts;
    const std::vector<StmtPtr>* statements = nullptr;  // the compiler's, which outlive the checker's use
    std::vector<ImportInfo> imports;
};

// ---- Checking a body ----

struct Local {
    Type type;
    bool isVal = false;
    // A parameter of a named function, which R1 follows to see whether the function only calls
    // it (a lambda passed for it then runs in its caller's place).
    FuncInfo* paramOf = nullptr;
    int paramIndex = -1;
    // A constant of the file's top level, which the file's code sees as a local (R2).
    FieldInfo* constant = nullptr;
    // A val with a value, whose reads are folded where the value is constant.
    const VarStmt* val = nullptr;
    // A lambda's parameter: a place, where a list of values hands the lambda its own item
    // (M6.6 part 4, value-lists.html section 8).
    bool lambdaParam = false;
    // A catch clause's variable: `throw e` gives again what the try could throw (D35).
    void* catchOf = nullptr;
    int catchClause = -1;
    // The variable of a for loop over a list: whose uses decide whether it may be the item
    // itself (M6.6 part 4).
    const ForInStmt* loopOf = nullptr;
    // A parameter of a value type, of the function or lambda that declares it.
    const FunctionDecl* paramOfDecl = nullptr;
};

// Why nothing may wait in some code, whatever the function around it says (R1).
enum NoWait : uint8_t { CanWait, Getter, Setter, ToString, Equals, HashCode, ObjectMade, StaticValue, SortComparison, ParallelSection, LazyValue, SequenceBlock };

// One function, lambda or file top level being checked.
struct FnCtx {
    FnCtx* parent = nullptr;      // the code around a lambda
    FuncInfo* fn = nullptr;       // null for a lambda and the top level
    ClassInfo* cls = nullptr;     // the class whose members bare names reach
    bool isStatic = false;
    bool isLambda = false;
    bool topLevel = false;
    bool returnKnown = false;     // whether returns are checked against `expected`
    Type expected;
    std::vector<Type>* returns = nullptr;  // collected, when the result is being worked out
    bool returnsBare = false;
    // The object bare names are looked for on first: an extension's receiver, or that of a
    // lambda passed as `Grid.() -> unit`.
    bool hasReceiver = false;
    Type receiver;
    // A lambda passed where the checker cannot tell whether it runs on an object: a bare name
    // it does not declare may belong to that object, so it is unknown.
    bool receiverUnknown = false;
    // Waiting (R1): the lambda or anonymous function this is, whose place decides whether it
    // may wait; whether it was given a suspend type; and why nothing may wait here.
    const FunctionDecl* placeDecl = nullptr;
    bool suspendLambda = false;
    NoWait noWait = CanWait;
    // Where what is thrown or called here is noted (D35): the function's own scope, or a try's.
    void* throwScope = nullptr;
};

const char* article(const std::string& word) {
    if (word.empty()) return "a";
    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(word[0])));
    return (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') ? "an" : "a";
}

// The arguments of a call, as the checker reads them.
// A call with no named arguments. Made on first use (M6.5 part 5: nothing of the compiler runs
// as a built program starts).
const std::vector<std::string>& noNames() {
    static const std::vector<std::string> none;
    return none;
}

struct Args {
    const std::vector<ExprPtr>& exprs;
    const std::vector<std::string>& names;
    bool trailing = false;
    bool spread = false;
};

}  // namespace

struct Checker::Impl {
    std::vector<std::unique_ptr<Unit>> units;
    std::unordered_map<std::string, Unit*> unitsByKey;
    std::vector<std::unique_ptr<ClassInfo>> classStore;
    std::vector<std::unique_ptr<FuncInfo>> funcStore;
    std::vector<std::unique_ptr<FieldInfo>> fieldStore;

    std::unordered_map<Namespace*, std::unordered_map<std::string, GlobalSym>> byNamespace;
    std::unordered_map<std::string, std::unordered_map<std::string, GlobalSym>> privates;  // by file key
    Namespace* builtinNs = nullptr;
    // Extension functions: receiver type name -> name -> functions. A receiver that is a
    // type parameter (`fun <T> T.twice()`) is filed under "<T>": it is for every type.
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<FuncInfo*>>> extensions;
    std::unordered_map<const ClassStmt*, ClassInfo*> classOf;
    // What the C++ provides on the built-in types, declared with `external`: methods, which
    // come before extensions when the program runs, and properties. By type name.
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<FuncInfo*>>> builtinMethods;
    struct BuiltinProp {
        const VarStmt* decl = nullptr;
        Unit* unit = nullptr;
        bool resolved = false;
        Type receiver, type;
        std::vector<std::string> typeParams;
    };
    std::unordered_map<std::string, std::unordered_map<std::string, BuiltinProp>> builtinProps;
    std::unordered_map<const FunctionDecl*, FuncInfo*> funcOf;
    bool reportPrelude = false;
    bool requireTypes = true;
    std::vector<std::unique_ptr<Unit>> pendingHolders;  // imports seen before their file is added

    std::vector<CheckDiagnostic>* out = nullptr;
    std::set<std::tuple<std::string, int, int, std::string>> seen;
    int silent = 0;

    // The body being checked.
    Unit* unit = nullptr;
    FnCtx* ctx = nullptr;
    std::vector<std::unordered_map<std::string, Local>> scopes;
    // Classes declared in a block, innermost last. Not cleared for a body checked inside the
    // block, so the methods of a local class see it, and so do the functions beside it.
    std::vector<std::unordered_map<std::string, ClassInfo*>> localClasses;
    // Narrowed types of locals, innermost last: `x` after `x != null` or `x is Box`.
    std::vector<std::unordered_map<std::string, Type>> narrowed;
    // The type parameters in reach, innermost last, with their bounds.
    std::vector<std::unordered_map<std::string, std::shared_ptr<Type>>> tparams;

    // ---- Reporting ----

    void report(const Node& at, const std::string& message) {
        if (silent > 0 || unit == nullptr || (unit->prelude && !reportPrelude)) return;
        auto key = std::make_tuple(unit->file, at.line, at.col, message);
        if (!seen.insert(key).second) return;
        out->push_back(CheckDiagnostic{unit->file, at.line, at.col, message});
    }

    // ---- Describing types ----

    std::string argText(const Type& t) const { return t.unknown() ? std::string("*") : text(t); }

    std::string text(const Type& t) const {
        std::string s;
        switch (t.kind) {
            case Type::Unknown: return "a value of unknown type";
            case Type::Any: s = "any"; break;
            case Type::Nothing: return "nothing";
            case Type::Unit: return "unit";
            case Type::Null: return "null";
            case Type::Bool: s = "bool"; break;
            case Type::Int:
            case Type::Float: s = t.storage != 0 ? typeText(t.storage, "") : t.kind == Type::Int ? "int" : "float"; break;
            case Type::Char: s = "char"; break;
            case Type::Byte: s = "byte"; break;
            case Type::Index: s = "Index"; break;
            case Type::Number: s = "Number"; break;
            case Type::String: s = "string"; break;
            case Type::Bytes: s = "Bytes"; break;
            case Type::Range: s = "Range"; break;
            case Type::TypeValue: s = "Type"; break;
            case Type::List: s = std::string(t.frozen ? "Frozen" : "") + "List<" + (t.args.empty() ? std::string("*") : argText(t.args[0])) + ">"; break;
            case Type::Set: s = std::string(t.frozen ? "Frozen" : "") + "Set<" + (t.args.empty() ? std::string("*") : argText(t.args[0])) + ">"; break;
            case Type::Map:
                s = std::string(t.frozen ? "Frozen" : "") + "Map<" + (t.args.size() < 2 ? std::string("*") : argText(t.args[0])) + ", " +
                    (t.args.size() < 2 ? std::string("*") : argText(t.args[1])) + ">";
                break;
            case Type::Function:
                if (!t.knownParams) {
                    s = "Function";
                } else {
                    s = t.receiver ? argText(*t.receiver) + ".(" : "(";
                    for (size_t i = 0; i < t.args.size(); i++) s += (i ? ", " : "") + argText(t.args[i]);
                    s += ") -> " + (t.result ? argText(*t.result) : std::string("*"));
                    for (size_t i = 0; i < t.throws.size(); i++) s += (i ? " | " : " throws ") + argText(t.throws[i]);
                    if (t.suspend) s = "suspend " + s;
                    if (t.nullable) s = "(" + s + ")";
                }
                break;
            case Type::Class:
                s = t.cls ? t.cls->name : "?";
                if (!t.args.empty()) {
                    s += "<";
                    for (size_t i = 0; i < t.args.size(); i++) s += (i ? ", " : "") + argText(t.args[i]);
                    s += ">";
                }
                break;
            case Type::TypeParam: s = t.name; break;
            case Type::Union:
                for (size_t i = 0; i < t.args.size(); i++) s += (i ? " | " : "") + argText(t.args[i]);
                if (t.nullable) s += " | null";
                return s;
            case Type::Bundle:
                s = "params(";
                for (size_t i = 0; i < t.args.size() && i < t.fields.size(); i++) s += (i ? ", " : "") + t.fields[i] + " : " + argText(t.args[i]);
                s += ")";
                break;
        }
        if (t.open) s = "open " + s;
        if (t.accepts) s = "accepts " + s;
        if (t.nullable) s += "?";
        return s;
    }

    // "an int", "a string?", "null".
    std::string describe(const Type& t) const {
        if (t.kind == Type::Null) return "null";
        if (t.kind == Type::Unknown || t.kind == Type::Nothing || t.kind == Type::Unit) return text(t);
        std::string s = text(t);
        return std::string(article(s)) + " " + s;
    }

    // ---- Relations between types ----

    bool isSubclass(ClassInfo* sub, ClassInfo* sup) {
        if (sub == nullptr || sup == nullptr) return false;
        std::unordered_set<ClassInfo*> visited;
        std::function<bool(ClassInfo*)> walk = [&](ClassInfo* c) -> bool {
            if (c == nullptr || !visited.insert(c).second) return false;
            if (c == sup) return true;
            resolveSupers(c);
            if (walk(c->superclass)) return true;
            for (ClassInfo* i : c->interfaces)
                if (walk(i)) return true;
            // An interface an extend in reach fits the class to (D33).
            if (sup->isInterface && !extendsDeclared.empty())
                for (ExtendInfo& e : extendsDeclared)
                    if (extendInReach(e) && resolveExtend(e) &&
                        std::find(e.types.begin(), e.types.end(), c) != e.types.end() && walk(e.iface))
                        return true;
            return false;
        };
        return walk(sub);
    }

    // ---- extend (D33) ----

    struct ExtendInfo {
        const ExtendStmt* stmt = nullptr;
        Unit* unit = nullptr;
        int resolved = 0;  // 0 not yet, 1 resolved, 2 being resolved
        std::vector<ClassInfo*> types;
        ClassInfo* iface = nullptr;
    };
    std::vector<ExtendInfo> extendsDeclared;

    bool resolveExtend(ExtendInfo& e) {
        if (e.resolved == 1) return e.iface != nullptr;
        if (e.resolved == 2) return false;
        e.resolved = 2;
        e.iface = findClass(*e.unit, e.stmt->interfaceType.name, nullptr);
        for (const TypeAnnotation& t : e.stmt->types) e.types.push_back(findClass(*e.unit, t.name, nullptr));
        e.resolved = 1;
        return e.iface != nullptr && e.iface->isInterface;
    }

    // Whether an extend holds where the code being checked is: in its folder and the files
    // importing it, with its access word; the standard library's everywhere.
    bool extendInReach(const ExtendInfo& e) const { return declarationInReach(e.unit, e.stmt->visibility); }

    bool declarationInReach(const Unit* from, uint8_t visibility) const {
        if (from == nullptr || from->prelude || unit == nullptr) return true;
        if (visibility == 2) return from->fileKey == unit->fileKey;
        if (from->ns == unit->ns) return true;
        for (const ImportInfo& imp : unit->imports)
            if (imp.ns == from->ns) return visibility == 0 || (imp.ns != nullptr && unit->ns != nullptr && imp.ns->library == unit->ns->library);
        return false;
    }

    // Each extend: an interface, classes, and what the classes lack bridged by its getters.
    std::map<std::pair<ClassInfo*, ClassInfo*>, const ExtendStmt*> extendsSeen;

    void checkExtend(const ExtendStmt& s) {
        ExtendInfo* info = nullptr;
        for (ExtendInfo& e : extendsDeclared)
            if (e.stmt == &s) info = &e;
        if (info == nullptr) return;
        resolveExtend(*info);
        if (info->iface == nullptr || !info->iface->isInterface) {
            report(s.interfaceType, "an extend fits types to an interface; " + s.interfaceType.name + " is not one (D33)");
            return;
        }
        ClassInfo* iface = info->iface;
        std::set<std::string> bridged;
        for (const auto& b : s.bridges) bridged.insert(b->name);
        for (size_t t = 0; t < s.types.size(); t++) {
            ClassInfo* k = info->types[t];
            if (k == nullptr) {
                report(s.types[t], "an extend fits a class to an interface; " + s.types[t].name + " is not a class of the program or a library " +
                                       "(a built-in type is not yet) (D33)");
                continue;
            }
            // Two of the same class and interface are one when they say the same, an error otherwise.
            auto [seenAt, first] = extendsSeen.emplace(std::make_pair(k, iface), &s);
            if (!first && seenAt->second != &s) {
                std::set<std::string> other;
                for (const auto& b : seenAt->second->bridges) other.insert(b->name);
                if (other != bridged)
                    report(s, "a second extend of " + k->name + " : " + iface->name + ", with other getters than the one on line " +
                                  std::to_string(seenAt->second->line) + ": there is one way a class fits an interface (D33)");
            }
            // What the interface leaves for the class: it has it, or a getter here gives it.
            const bool wasOwn = std::exchange(ownMembersOnly, true);
            std::unordered_set<ClassInfo*> visited;
            std::function<void(ClassInfo*)> need = [&](ClassInfo* i) {
                if (i == nullptr || !visited.insert(i).second) return;
                resolveSupers(i);
                const Type self = classType(k);
                for (const auto& [name, field] : i->fields) {
                    if (field->decl != nullptr && (field->decl->getter || field->decl->init)) continue;  // comes with the interface
                    if (!lookupMember(self, name).found() && !bridged.count(name))
                        report(s.types[t], k->name + " does not fit " + iface->name + ": it has no '" + name + "'; a get() in the extend " +
                                               "can give it from what it has (D33)");
                }
                for (const auto& [name, forms] : i->methods)
                    for (FuncInfo* f : forms)
                        if (f->decl != nullptr && f->decl->isAbstract && !lookupMember(self, name, true).found())
                            report(s.types[t], k->name + " does not fit " + iface->name + ": it has no " + name + "() (D33)");
                for (ClassInfo* above : i->interfaces) need(above);
            };
            need(iface);
            // A getter here bridges a name the class lacks; it replaces nothing of the class's.
            for (const auto& b : s.bridges) {
                if (lookupMember(classType(k), b->name).found())
                    report(*b, "'" + b->name + "' is " + k->name + "'s own: an extend does not replace what a class has (D33)");
            }
            ownMembersOnly = wasOwn;
            for (const auto& b : s.bridges) {
                FuncInfo* getter = newFunc(*b->getter, info->unit, k);
                checkFunctionBody(getter, nullptr);
            }
        }
    }

    bool hasUnknownSuper(ClassInfo* c) {
        std::unordered_set<ClassInfo*> visited;
        std::function<bool(ClassInfo*)> walk = [&](ClassInfo* k) -> bool {
            if (k == nullptr || !visited.insert(k).second) return false;
            resolveSupers(k);
            if (k->unknownSuper) return true;
            if (walk(k->superclass)) return true;
            for (ClassInfo* i : k->interfaces)
                if (walk(i)) return true;
            return false;
        };
        return walk(c);
    }

    // What a for over an object of a class goes through (Kotlin's protocol): an Iterator<T>
    // gives Ts, and so does an object whose iterator() gives one; an iterator() giving a list or
    // another built-in collection gives what that holds. A loop through an Iterator is noted for
    // the compiler, which calls hasNext() and next() as it would any methods: a task may be set
    // aside in them, as in a channel's.
    std::unordered_map<const ForInStmt*, bool> protocolLoops;  // true: through the object's iterator()
    Type iterationItem(const Type& over, const ForInStmt& loop) {
        // An Iterator, or any object with hasNext() and next(), as Kotlin's convention has it.
        auto itemOf = [&](const Type& t, Type& item) {
            if (t.kind != Type::Class || t.cls == nullptr || t.nullable) return false;
            Member has = lookupMember(t, "hasNext", true), next = lookupMember(t, "next", true);
            if (has.methods.size() != 1 || next.methods.size() != 1) return false;
            resolveFunc(next.methods[0]);
            if (!next.methods[0]->params.empty()) return false;
            item = readType(subst(resultOf(next.methods[0]), next.methodEnvs.empty() ? next.env : next.methodEnvs[0]));
            return true;
        };
        Type item;
        if (itemOf(over, item)) {
            if (silent == 0) protocolLoops[&loop] = false;
            return item;
        }
        Member m = lookupMember(withNull(over, false), "iterator", true);
        if (m.methods.size() != 1) return unknownType();
        resolveFunc(m.methods[0]);
        if (!m.methods[0]->params.empty()) return unknownType();
        const Type made = subst(resultOf(m.methods[0]), m.methodEnvs.empty() ? m.env : m.methodEnvs[0]);
        if (itemOf(made, item)) {
            if (silent == 0) protocolLoops[&loop] = true;
            return item;
        }
        if (made.nullable) return unknownType();
        if ((made.kind == Type::List || made.kind == Type::Set) && !made.args.empty()) return readType(made.args[0]);
        if (made.kind == Type::Map) return mapEntryOf(made);
        if (made.kind == Type::Range) return Type::of(Type::Int);
        if (made.kind == Type::String) return Type::of(Type::Char);
        return unknownType();
    }

    // A class type seen as one of the classes above it: `IntBox` as a `Box<int>`.
    Type asSuper(const Type& from, ClassInfo* sup) {
        if (from.cls == sup) return from;
        std::unordered_set<ClassInfo*> visited;
        std::function<bool(const Type&, Type&)> walk = [&](const Type& t, Type& out) -> bool {
            if (t.cls == nullptr || !visited.insert(t.cls).second) return false;
            if (t.cls == sup) {
                out = t;
                return true;
            }
            resolveSupers(t.cls);
            Env env = envOf(t);
            if (t.cls->superclass != nullptr) {
                Type s = classType(t.cls->superclass);
                for (const Type& a : t.cls->superArgs) s.args.push_back(subst(a, env));
                if (walk(s, out)) return true;
            }
            for (size_t i = 0; i < t.cls->interfaces.size(); i++) {
                Type s = classType(t.cls->interfaces[i]);
                for (const Type& a : t.cls->interfaceArgs[i]) s.args.push_back(subst(a, env));
                if (walk(s, out)) return true;
            }
            return false;
        };
        Type found;
        if (walk(from, found)) return found;
        return classType(sup);
    }

    // Whether a value of type `from` can go where `to` is asked for.
    // Whether every class of `inner` is one of `outer`'s, or below one (D52).
    bool throwsWithin(const std::vector<Type>& inner, const std::vector<Type>& outer) {
        for (const Type& t : inner) {
            if (t.kind != Type::Class || t.cls == nullptr) continue;
            bool covered = false;
            for (const Type& o : outer) covered = covered || (o.kind == Type::Class && o.cls != nullptr && isSubclass(t.cls, o.cls));
            if (!covered) return false;
        }
        return true;
    }

    bool fits(const Type& from, const Type& to) {
        if (from.unknown() || to.unknown()) return true;
        if (from.kind == Type::TypeParam || to.kind == Type::TypeParam) {
            // Inside a generic a type parameter is not known; outside it is replaced first.
            if (from.kind == Type::Null) return true;
            return !(from.nullable && to.kind != Type::TypeParam && !to.nullable);
        }
        if (from.kind == Type::Nothing) return true;
        if (from.kind == Type::Null) return to.nullable || to.kind == Type::Unit;
        if (from.nullable && !to.nullable) return false;
        if (to.kind == Type::Any) return true;
        if (to.kind == Type::Unit) return true;  // a value nobody looks at
        // A union goes where each of its types does; a value goes where a union is asked for
        // when it fits one of its types (D40).
        if (from.kind == Type::Union) {
            for (const Type& m : from.args)
                if (!fits(m, to)) return false;
            return true;
        }
        if (to.kind == Type::Union) {
            for (const Type& m : to.args)
                if (fits(withNull(from, false), m)) return true;
            return false;
        }
        switch (to.kind) {
            case Type::Bundle:
                // The same names, each of a type that fits.
                if (from.kind != Type::Bundle || from.fields.size() != to.fields.size()) return false;
                for (size_t i = 0; i < to.fields.size(); i++) {
                    auto at = std::find(from.fields.begin(), from.fields.end(), to.fields[i]);
                    if (at == from.fields.end() || !fits(from.args[static_cast<size_t>(at - from.fields.begin())], to.args[i])) return false;
                }
                return true;
            case Type::Number:
                return from.kind == Type::Int || from.kind == Type::Float || from.kind == Type::Number || from.kind == Type::Byte;
            case Type::Index:
                return from.kind == Type::Int || from.kind == Type::Index;  // an int is a position from the start (D146)
            case Type::Set:
                if (from.kind != Type::Set) return false;
                if (to.frozen && !from.frozen) return false;  // what nobody may change (D26)
                if (to.open) return from.open && (to.args.empty() || from.args.empty() || exactArg(from.args[0], to.args[0]));
                if (to.args.empty() || from.args.empty()) return true;
                return fits(from.args[0], to.args[0]) && from.args[0].storage == to.args[0].storage;
            case Type::List:
                if (from.kind != Type::List) return false;
                if (to.frozen && !from.frozen) return false;
                // Changed through this name: only a list that may be, of exactly the type (D32),
                // or of one above what it accepts (D34).
                if (to.open) return from.open && (to.args.empty() || from.args.empty() || exactArg(from.args[0], to.args[0]));
                if (to.args.empty() || from.args.empty()) return true;
                // Items kept at a storage width are kept differently (D148): the widths match.
                return fits(from.args[0], to.args[0]) && from.args[0].storage == to.args[0].storage;  // read through that name only (D32)
            case Type::Map:
                if (from.kind != Type::Map) return false;
                if (to.frozen && !from.frozen) return false;
                if (to.open)
                    return from.open && (to.args.size() < 2 || from.args.size() < 2 ||
                                         (exactArg(from.args[0], to.args[0]) && exactArg(from.args[1], to.args[1])));
                // Read only, a map goes where one of wider keys or values is read (D47).
                if (to.args.size() < 2 || from.args.size() < 2) return true;
                return fits(from.args[0], to.args[0]) && fits(from.args[1], to.args[1]) && from.args[0].storage == to.args[0].storage &&
                       from.args[1].storage == to.args[1].storage;
            case Type::Function: {
                if (from.kind != Type::Function) return false;
                // A function that may wait goes only where waiting is expected (R1).
                if (from.suspend && !to.suspend && to.knownParams) return false;
                if (!to.knownParams || !from.knownParams) return true;
                // A function whose last parameters have defaults can be called with fewer arguments.
                const size_t least = from.required < 0 ? from.args.size() : static_cast<size_t>(from.required);
                if (to.args.size() < least || to.args.size() > from.args.size()) return false;
                for (size_t i = 0; i < to.args.size(); i++)
                    if (!fits(to.args[i], from.args[i])) return false;
                // What it may throw, the type it goes to may (D52).
                if (!throwsWithin(from.throws, to.throws)) return false;
                if (to.result && from.result && to.result->kind != Type::Unit) return fits(*from.result, *to.result);
                return true;
            }
            case Type::Class: {
                if (from.kind != Type::Class) return false;
                if (!isSubclass(from.cls, to.cls)) return false;
                if (to.args.empty()) return true;
                Type above = asSuper(withNull(from, false), to.cls);
                for (size_t i = 0; i < to.args.size() && i < above.args.size(); i++)
                    if (!fits(above.args[i], to.args[i])) return false;
                return true;
            }
            case Type::Int:
                // A byte goes where an int does: 0 to 255 is an int too, as byte arithmetic gives one.
                return from.kind == Type::Int || from.kind == Type::Byte;
            default:
                return from.kind == to.kind;
        }
    }

    // A type argument of an open list, map or set given where one is asked for (D32): exactly
    // that type, the same either way; or, for `accepts T`, a type T goes into (D34).
    bool exactArg(const Type& from, const Type& to) {
        if (from.unknown() || to.unknown()) return true;
        if (to.accepts) {
            Type item = to;
            item.accepts = false;
            Type given = from;
            given.accepts = false;
            return fits(item, given);
        }
        if (from.accepts) return false;
        return fits(from, to) && fits(to, from) && from.storage == to.storage;
    }

    // The type both of two values fit, when there is an easy one; Unknown otherwise.
    Type common(const Type& a, const Type& b) {
        if (a.unknown() || b.unknown()) return unknownType();
        if (a.kind == Type::Nothing) return b;
        if (b.kind == Type::Nothing) return a;
        if (a.kind == Type::Null) return b.kind == Type::Null ? a : withNull(b);
        if (b.kind == Type::Null) return withNull(a);
        Type plainA = withNull(a, false), plainB = withNull(b, false);
        const bool nullable = a.nullable || b.nullable;
        if (fits(plainA, plainB)) return withNull(plainB, nullable);
        if (fits(plainB, plainA)) return withNull(plainA, nullable);
        if ((plainA.kind == Type::Int || plainA.kind == Type::Float) && (plainB.kind == Type::Int || plainB.kind == Type::Float))
            return withNull(Type::of(Type::Number), nullable);
        // Two classes with a class or interface above both: the nearest such.
        if (plainA.kind == Type::Class && plainB.kind == Type::Class) {
            std::vector<ClassInfo*> above;
            std::unordered_set<ClassInfo*> visited;
            std::function<void(ClassInfo*)> walk = [&](ClassInfo* c) {
                if (c == nullptr || !visited.insert(c).second) return;
                above.push_back(c);
                resolveSupers(c);
                walk(c->superclass);
                for (ClassInfo* i : c->interfaces) walk(i);
            };
            walk(plainA.cls);
            for (ClassInfo* c : above)
                if (c->typeParams.empty() && isSubclass(plainB.cls, c)) return withNull(classType(c), nullable);
        }
        // Otherwise the union of the two (D40): `flag ? 1 : "one"` is an int | string.
        if (plainA.kind == Type::Any || plainB.kind == Type::Any || plainA.kind == Type::TypeParam || plainB.kind == Type::TypeParam ||
            plainA.kind == Type::Unit || plainB.kind == Type::Unit)
            return unknownType();
        return makeUnion({plainA, plainB}, nullable);
    }

    // The union of `members` (D40): unions inside it opened up, each type once, `null` as the
    // union's being nullable. One type left is that type.
    Type makeUnion(const std::vector<Type>& members, bool nullable) {
        std::vector<Type> flat;
        std::unordered_set<std::string> seenTypes;
        std::function<void(const Type&)> add = [&](const Type& m) {
            if (m.kind == Type::Union) {
                nullable = nullable || m.nullable;
                for (const Type& inner : m.args) add(inner);
                return;
            }
            if (m.kind == Type::Null) {
                nullable = true;
                return;
            }
            if (m.kind == Type::Nothing) return;
            nullable = nullable || m.nullable;
            const Type plain = withNull(m, false);
            if (seenTypes.insert(text(plain)).second) flat.push_back(plain);
        };
        for (const Type& m : members) add(m);
        for (const Type& m : flat)
            if (m.unknown()) return unknownType();
        for (const Type& m : flat)
            if (m.kind == Type::Any) return withNull(Type::of(Type::Any), nullable);
        if (flat.empty()) return nullable ? Type::of(Type::Null) : Type::of(Type::Nothing);
        if (flat.size() == 1) return withNull(flat[0], nullable);
        // A type that another one takes in says nothing more: int | Number is a Number.
        std::vector<Type> kept;
        for (size_t i = 0; i < flat.size(); i++) {
            bool inOther = false;
            for (size_t j = 0; j < flat.size() && !inOther; j++)
                if (i != j && flat[i].kind != Type::TypeParam && flat[j].kind != Type::TypeParam && fits(flat[i], flat[j]) &&
                    !(fits(flat[j], flat[i]) && j > i))
                    inOther = true;
            if (!inOther) kept.push_back(flat[i]);
        }
        if (kept.size() == 1) return withNull(kept[0], nullable);
        Type u = Type::of(Type::Union);
        u.args = kept;
        u.nullable = nullable;
        return u;
    }

    // What every type of a union has (D40): the nearest type above all of them; unknown when
    // there is none.
    Type unionCommon(const Type& u) {
        Type all;
        bool first = true;
        for (const Type& m : u.args) {
            Type joined = first ? m : common(all, m);
            if (joined.kind == Type::Union || joined.unknown()) return unknownType();
            all = joined;
            first = false;
        }
        return withNull(all, u.nullable);
    }

    // A name `type` gave a type to (D41), where `a` is written: what it names, with the type
    // arguments written there, and the type it names written out for the compiler.
    std::unordered_map<const TypeAnnotation*, std::unique_ptr<TypeAnnotation>> aliasExpansions;
    std::unordered_set<const TypeAliasStmt*> aliasBusy;

    Type expandAlias(const GlobalSym& g, const TypeAnnotation& a, Unit& u) {
        const TypeAliasStmt& alias = *g.alias;
        // A name that refers to itself, inside another type (`List<Json>`), is any there.
        if (aliasBusy.count(&alias)) return unknownType();
        aliasBusy.insert(&alias);
        Env env;
        for (size_t i = 0; i < alias.typeParams.size(); i++)
            env[alias.typeParams[i].name] = i < a.args.size() ? resolve(a.args[i], u) : unknownType();
        if (a.args.size() != alias.typeParams.size())
            report(a, alias.name + " takes " + std::to_string(alias.typeParams.size()) + " type argument" +
                          (alias.typeParams.size() == 1 ? "" : "s") + ", not " + std::to_string(a.args.size()));
        Type t = subst(resolve(alias.target, *g.aliasUnit), env);
        if (aliasExpansions.find(&a) == aliasExpansions.end()) {
            auto expanded = std::make_unique<TypeAnnotation>(writtenOut(alias.target, alias, a.args, *g.aliasUnit));
            expanded->nullable = expanded->nullable || a.nullable;
            aliasExpansions[&a] = std::move(expanded);
        }
        aliasBusy.erase(&alias);
        return a.nullable ? withNull(t) : t;
    }

    // The type an alias names, written out: its type parameters replaced by what is written
    // where it is used, and names of other aliases written out in turn ("any" for one that
    // refers to itself).
    TypeAnnotation writtenOut(const TypeAnnotation& t, const TypeAliasStmt& alias, const std::vector<TypeAnnotation>& given, Unit& u) {
        if (t.typeParam) {
            for (size_t i = 0; i < alias.typeParams.size() && i < given.size(); i++)
                if (alias.typeParams[i].name == t.name) {
                    TypeAnnotation r = given[i];
                    r.nullable = r.nullable || t.nullable;
                    return r;
                }
        }
        TypeAnnotation r = t;
        if (!t.typeParam && t.name.find('.') == std::string::npos && t.name != "|")
            if (const GlobalSym* g = findGlobal(u, t.name); g != nullptr && g->alias != nullptr) {
                if (aliasBusy.count(g->alias) && g->alias != &alias) {
                    r = TypeAnnotation{};
                    r.name = "any";
                    return r;
                }
                if (g->alias == &alias) {
                    r = TypeAnnotation{};
                    r.name = "any";
                    return r;
                }
                aliasBusy.insert(g->alias);
                TypeAnnotation inner = writtenOut(g->alias->target, *g->alias, t.args, *g->aliasUnit);
                aliasBusy.erase(g->alias);
                inner.nullable = inner.nullable || t.nullable;
                return inner;
            }
        for (TypeAnnotation& arg : r.args) arg = writtenOut(arg, alias, given, u);
        for (TypeAnnotation& m : r.unionOf) m = writtenOut(m, alias, given, u);
        for (TypeAnnotation& p : r.params) p = writtenOut(p, alias, given, u);
        for (TypeAnnotation& res : r.result) res = writtenOut(res, alias, given, u);
        return r;
    }

    bool isNumeric(const Type& t) const {
        return !t.nullable && (t.kind == Type::Int || t.kind == Type::Float || t.kind == Type::Number);
    }

    // ---- Inferring type arguments ----

    // Matches what a parameter is declared as against what it is given, filling in the type
    // parameters in `vars`: `list<T>` given a list<int> makes T an int.
    void unify(const Type& param, const Type& actual, Env& env, const std::vector<std::string>& vars) {
        if (actual.unknown() || actual.kind == Type::Nothing) return;
        if (param.kind == Type::TypeParam) {
            if (std::find(vars.begin(), vars.end(), param.name) == vars.end()) return;
            if (actual.kind == Type::Null) return;
            Type value = param.nullable ? withNull(actual, false) : actual;
            auto found = env.find(param.name);
            if (found == env.end()) env[param.name] = value;
            else found->second = common(found->second, value);
            return;
        }
        Type a = actual;
        if (param.kind == Type::Class && a.kind == Type::Class && a.cls != param.cls && param.cls != nullptr)
            a = asSuper(withNull(a, false), param.cls);
        if (param.kind != a.kind) return;
        for (size_t i = 0; i < param.args.size() && i < a.args.size(); i++) unify(param.args[i], a.args[i], env, vars);
        if (param.result && a.result) unify(*param.result, *a.result, env, vars);
        if (param.receiver && a.receiver) unify(*param.receiver, *a.receiver, env, vars);
    }

    // What the type parameters of a class type stand for: Box<int> gives T -> int.
    Env envOf(const Type& t) {
        Env env;
        if (t.kind != Type::Class || t.cls == nullptr) return env;
        for (size_t i = 0; i < t.cls->typeParams.size(); i++)
            env[t.cls->typeParams[i]] = i < t.args.size() ? t.args[i] : unknownType();
        return env;
    }

    // ---- Gathering declarations ----

    std::unordered_map<std::string, GlobalSym>& symbolsFor(Unit& u, uint8_t visibility) {
        if (visibility == 2) return privates[u.fileKey];
        return byNamespace[u.ns];
    }

    // A name declared at the top level: in the shell, an input that declares a name again
    // replaces what it was (the forms of a function declared in one input stay together), and
    // what it was is kept, to be put back if the input does not compile.
    struct ShellUndo {
        std::unordered_map<std::string, GlobalSym>* table;
        std::string name;
        bool existed;
        GlobalSym old;
    };
    std::vector<ShellUndo> shellUndo;
    std::vector<std::pair<std::vector<FuncInfo*>*, size_t>> shellExtensionsUndo;

    GlobalSym& declared(Unit& u, uint8_t visibility, const std::string& name) {
        auto& table = symbolsFor(u, visibility);
        if (!u.shell) return table[name];
        for (const ShellUndo& done : shellUndo)
            if (done.table == &table && done.name == name) return table[name];
        auto found = table.find(name);
        shellUndo.push_back(ShellUndo{&table, name, found != table.end(), found != table.end() ? found->second : GlobalSym{}});
        table[name] = GlobalSym{};
        return table[name];
    }

    void addExtension(Unit& u, std::vector<FuncInfo*>& forms, FuncInfo* f) {
        if (u.shell) shellExtensionsUndo.emplace_back(&forms, forms.size());
        forms.push_back(f);
    }

    void forgetShellUnit() {
        for (auto it = shellUndo.rbegin(); it != shellUndo.rend(); ++it) {
            if (it->existed) (*it->table)[it->name] = it->old;
            else it->table->erase(it->name);
        }
        for (auto it = shellExtensionsUndo.rbegin(); it != shellExtensionsUndo.rend(); ++it) it->first->resize(it->second);
        shellUndo.clear();
        shellExtensionsUndo.clear();
        formsAboveKnown.clear();
        familyKnown.clear();
    }

    FuncInfo* newFunc(const FunctionDecl& decl, Unit* u, ClassInfo* owner) {
        funcStore.push_back(std::make_unique<FuncInfo>());
        FuncInfo* f = funcStore.back().get();
        f->name = decl.name;
        f->decl = &decl;
        f->unit = u;
        f->owner = owner;
        f->isStatic = decl.isStatic;
        for (const TypeParam& t : decl.typeParams) f->typeParams.push_back(t.name);
        funcOf.emplace(&decl, f);
        return f;
    }

    std::unordered_map<const VarStmt*, FieldInfo*> fieldByDecl;  // Checker::inferredFieldType

    FieldInfo* newField(const std::string& name, Unit* u, ClassInfo* owner) {
        fieldStore.push_back(std::make_unique<FieldInfo>());
        FieldInfo* f = fieldStore.back().get();
        f->name = name;
        f->unit = u;
        f->owner = owner;
        return f;
    }

    ClassInfo* gatherClass(const ClassStmt& c, Unit* u, ClassInfo* outer, bool global = false) {
        classStore.push_back(std::make_unique<ClassInfo>());
        ClassInfo* k = classStore.back().get();
        k->global = global;
        k->name = outer ? outer->name + "." + c.name : c.name;
        k->decl = &c;
        k->unit = u;
        k->outer = outer;
        k->isInterface = c.isInterface;
        k->isObject = c.isObject;
        k->isEnum = c.isEnum;
        k->isData = c.isData;
        k->isValue = c.isValue;
        k->isEnumClass = c.isEnumClass;
        k->isAbstract = c.isAbstract;
        for (const TypeParam& t : c.typeParams) k->typeParams.push_back(t.name);
        classOf[&c] = k;

        for (const auto& field : c.fields) {
            FieldInfo* f = newField(field->name, u, k);
            f->decl = field.get();
            f->isVal = field->isVal;
            k->fields[field->name] = f;
        }
        for (const auto& field : c.staticFields) {
            FieldInfo* f = newField(field->name, u, k);
            f->decl = field.get();
            f->isVal = field->isVal;
            k->statics[field->name] = f;
        }
        for (const HeaderParam& p : c.header) {
            if (!p.isField) continue;
            auto found = k->fields.find(p.name);
            if (found != k->fields.end()) found->second->header = &p;
        }
        for (const auto& m : c.methods) {
            FuncInfo* f = newFunc(*m, u, k);
            f->isExternal = c.isExternal;
            if (m->accessor == FunctionDecl::Accessor::Get) k->getters[m->name] = f;
            else if (m->accessor == FunctionDecl::Accessor::Set) k->setters[m->name] = f;
            else if (m->name == "init" && !m->isStatic) k->inits.push_back(f);
            else if (m->isStatic) k->staticMethods[m->name].push_back(f);
            else k->methods[m->name].push_back(f);
        }
        k->headerInit = c.hasHeader && k->inits.empty();
        for (const EnumEntry& e : c.entries) {
            FieldInfo* f = newField(e.name, u, k);
            f->isVal = true;
            f->typeWritten = true;
            f->type = classType(k);
            f->state = 2;
            k->statics[e.name] = f;
        }
        for (const auto& inner : c.nested) k->nested[inner->name] = gatherClass(*inner, u, k, k->global);
        return k;
    }

    void gather(Unit& u) {
        if (u.ns != nullptr && u.ns->directory == "<builtin>") builtinNs = u.ns;
        if (u.shell) {
            // What the input before it declared stays; each input's mistakes are its own.
            shellUndo.clear();
            shellExtensionsUndo.clear();
            seen.clear();
        }
        for (const StmtPtr& stmt : *u.statements) {
            switch (stmt->kind) {
                case StmtKind::Var: {
                    const auto& v = static_cast<const VarStmt&>(*stmt);
                    if (v.isExternal) {
                        BuiltinProp& p = builtinProps[v.receiverType.name][v.name];
                        p.decl = &v;
                        p.unit = &u;
                        for (const TypeParam& t : v.typeParams) p.typeParams.push_back(t.name);
                        break;
                    }
                    if (!v.pattern.empty()) {
                        for (const std::string& name : v.pattern) {
                            FieldInfo* f = newField(name, &u, nullptr);
                            f->isVal = v.isVal;
                            f->state = 2;  // the parts' types are worked out where they are declared
                            declared(u, stmt->visibility, name).var = f;
                        }
                        break;
                    }
                    FieldInfo* f = newField(v.name, &u, nullptr);
                    f->decl = &v;
                    f->isVal = v.isVal;
                    declared(u, stmt->visibility, v.name).var = f;
                    break;
                }
                case StmtKind::Function: {
                    const auto& fs = static_cast<const FunctionStmt&>(*stmt);
                    FuncInfo* f = newFunc(*fs.fn, &u, nullptr);
                    f->isExternal = fs.isExternal;
                    if (!fs.receiver.empty() && fs.isExternal) {
                        f->isExtension = true;
                        f->stmt = &fs;
                        builtinMethods[fs.receiver][fs.fn->name].push_back(f);
                    } else if (!fs.receiver.empty()) {
                        f->isExtension = true;
                        f->stmt = &fs;
                        addExtension(u, extensions[fs.receiverType.typeParam ? std::string("<T>") : fs.receiver][fs.fn->name], f);
                    } else {
                        declared(u, stmt->visibility, fs.fn->name).funcs.push_back(f);
                        if (stmt->visibility == 0) topLevelPublic.insert(fs.fn.get());
                    }
                    break;
                }
                case StmtKind::Class: {
                    const auto& c = static_cast<const ClassStmt&>(*stmt);
                    declared(u, stmt->visibility, c.name).cls = gatherClass(c, &u, nullptr, true);
                    break;
                }
                case StmtKind::Extend:
                    extendsDeclared.push_back(ExtendInfo{static_cast<const ExtendStmt*>(stmt.get()), &u});
                    break;
                case StmtKind::TypeAlias: {
                    const auto& a = static_cast<const TypeAliasStmt&>(*stmt);
                    GlobalSym& g = declared(u, stmt->visibility, a.name);
                    g.alias = &a;
                    g.aliasUnit = &u;
                    break;
                }
                default:
                    break;
            }
        }
    }

    // ---- Finding names ----

    const GlobalSym* findGlobal(Unit& u, const std::string& name) {
        auto p = privates.find(u.fileKey);
        if (p != privates.end()) {
            auto f = p->second.find(name);
            if (f != p->second.end()) return &f->second;
        }
        auto own = byNamespace.find(u.ns);
        if (own != byNamespace.end()) {
            auto f = own->second.find(name);
            if (f != own->second.end()) return &f->second;
        }
        for (const ImportInfo& imp : u.imports) {
            if (!imp.all) continue;
            auto ns = byNamespace.find(imp.ns);
            if (ns == byNamespace.end()) continue;
            auto f = ns->second.find(name);
            if (f != ns->second.end()) return &f->second;
        }
        if (builtinNs != nullptr && builtinNs != u.ns) {
            auto b = byNamespace.find(builtinNs);
            if (b != byNamespace.end()) {
                auto f = b->second.find(name);
                if (f != b->second.end()) return &f->second;
            }
        }
        return nullptr;
    }

    // `ui` in `ui.Button` after `import jade as ui`: the namespace, or null.
    Namespace* findAlias(Unit& u, const std::string& name) {
        for (const ImportInfo& imp : u.imports)
            if (!imp.alias.empty() && imp.alias == name) return imp.ns;
        return nullptr;
    }

    const GlobalSym* findIn(Namespace* ns, const std::string& name) {
        auto found = byNamespace.find(ns);
        if (found == byNamespace.end()) return nullptr;
        auto f = found->second.find(name);
        return f == found->second.end() ? nullptr : &f->second;
    }

    // A class by a written name: `Box`, `Outer.Inner`, `jade.Button`.
    ClassInfo* findClass(Unit& u, const std::string& written, ClassInfo* around) {
        std::vector<std::string> parts;
        size_t start = 0;
        for (;;) {
            size_t dot = written.find('.', start);
            parts.push_back(written.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        ClassInfo* k = nullptr;
        size_t i = 1;
        // A class declared in a block around the code comes first, then one nested in the
        // class around it.
        k = localClass(parts[0]);
        for (ClassInfo* c = around; c != nullptr && k == nullptr; c = c->outer) {
            auto n = c->nested.find(parts[0]);
            if (n != c->nested.end()) k = n->second;
        }
        if (k == nullptr) {
            if (const GlobalSym* g = findGlobal(u, parts[0]); g != nullptr && g->cls != nullptr) {
                k = g->cls;
            } else if (Namespace* ns = findAlias(u, parts[0]); ns != nullptr && parts.size() > 1) {
                const GlobalSym* g2 = findIn(ns, parts[1]);
                if (g2 == nullptr || g2->cls == nullptr) return nullptr;
                k = g2->cls;
                i = 2;
            } else {
                return nullptr;
            }
        }
        for (; i < parts.size() && k != nullptr; i++) {
            auto n = k->nested.find(parts[i]);
            k = n == k->nested.end() ? nullptr : n->second;
        }
        return k;
    }

    ClassInfo* classAround() const { return ctx != nullptr ? ctx->cls : nullptr; }

    // ---- Type parameters in reach ----

    // Saves the type parameters in reach and starts afresh (for a body checked elsewhere).
    // Set just before the body of a function declared inside another is checked.
    bool bodyInside = false;

    struct TParamsSaved {
        Impl& impl;
        std::vector<std::unordered_map<std::string, std::shared_ptr<Type>>> saved;
        explicit TParamsSaved(Impl& i) : impl(i), saved(std::move(i.tparams)) { impl.tparams.clear(); }
        ~TParamsSaved() { impl.tparams = std::move(saved); }
    };

    void pushTypeParams(const std::vector<TypeParam>& params, Unit& u) {
        tparams.emplace_back();
        for (const TypeParam& t : params) tparams.back()[t.name] = nullptr;
        for (const TypeParam& t : params)
            if (t.bound.present()) tparams.back()[t.name] = std::make_shared<Type>(resolve(t.bound, u));
    }

    // A class's type parameters, and those of the classes around it.
    void pushClassTypeParams(ClassInfo* k) {
        if (k == nullptr) return;
        pushClassTypeParams(k->outer);
        pushTypeParams(k->decl->typeParams, *k->unit);
    }

    Type typeParamType(const std::string& name) {
        Type t = Type::of(Type::TypeParam);
        t.name = name;
        for (auto it = tparams.rbegin(); it != tparams.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) {
                t.bound = f->second;
                break;
            }
        }
        return t;
    }

    // ---- Written types ----

    Type resolve(const TypeAnnotation& a, Unit& u) {
        if (!a.present() || a.star) return unknownType();
        if (auto kind = kindClasses.find(&a); kind != kindClasses.end()) return withNull(classType(kind->second), a.nullable);
        Type t;
        if (a.isBundle) {
            t = Type::of(Type::Bundle);
            t.fields = a.bundleNames;
            for (const TypeAnnotation& field : a.args) t.args.push_back(resolve(field, u));
        } else if (a.isFunction()) {
            t.kind = Type::Function;
            t.knownParams = true;
            for (const TypeAnnotation& p : a.params) t.args.push_back(resolve(p, u));
            t.result = std::make_shared<Type>(resolve(a.result[0], u));
            if (!a.receiver.empty()) t.receiver = std::make_shared<Type>(resolve(a.receiver[0], u));
            t.suspend = a.isSuspend;
            for (const TypeAnnotation& thrown : a.throwsTypes) t.throws.push_back(resolve(thrown, u));
        } else if (a.typeParam) {
            t = typeParamType(a.name);
        } else if (a.name == "|") {
            std::vector<Type> members;
            for (const TypeAnnotation& m : a.unionOf) members.push_back(resolve(m, u));
            return makeUnion(members, a.nullable);
        } else {
            const std::string& n = a.name;
            if (n == "int") t.kind = Type::Int;
            else if (n == "float") t.kind = Type::Float;
            else if (n == "char") t.kind = Type::Char;
            else if (n == "byte") t.kind = Type::Byte;
            else if (n == "Index") t.kind = Type::Index;
            else if (n == "int16" || n == "int32" || n == "uint16" || n == "uint32") {
                t.kind = Type::Int;
                t.storage = builtinTypeId(n);
            } else if (n == "float32") {
                t.kind = Type::Float;
                t.storage = kTypeFloat32;
            }
            else if (n == "Number") t.kind = Type::Number;
            else if (n == "string") t.kind = Type::String;
            else if (n == "bool") t.kind = Type::Bool;
            else if (n == "Bytes") t.kind = Type::Bytes;
            else if (n == "Range") t.kind = Type::Range;
            else if (n == "Type") t.kind = Type::TypeValue;
            else if (n == "any") t.kind = Type::Any;
            else if (n == "unit" || n == "Unit") t.kind = Type::Unit;
            else if (n == "nothing" || n == "Nothing") t.kind = Type::Nothing;
            else if (n == "Function") t.kind = Type::Function;
            else if (n == "List" || n == "FrozenList") {
                t.kind = Type::List;
                t.frozen = n == "FrozenList";
                t.args.push_back(a.args.empty() ? unknownType() : resolve(a.args[0], u));
            } else if (n == "Set" || n == "FrozenSet") {
                t.kind = Type::Set;
                t.frozen = n == "FrozenSet";
                t.args.push_back(a.args.empty() ? unknownType() : resolve(a.args[0], u));
            } else if (n == "MutableList" || n == "MutableMap" || n == "MutableSet") {
                const std::string kind = n.substr(7);
                report(a, n + " is written open " + kind + ": a " + kind + " that may be changed through the name it has (D32)");
                t.kind = kind == "List" ? Type::List : kind == "Map" ? Type::Map : Type::Set;
                t.open = true;
                for (const TypeAnnotation& arg : a.args) t.args.push_back(resolve(arg, u));
            } else if (n == "Map" || n == "FrozenMap") {
                t.kind = Type::Map;
                t.frozen = n == "FrozenMap";
                t.args.push_back(a.args.size() < 2 ? unknownType() : resolve(a.args[0], u));
                t.args.push_back(a.args.size() < 2 ? unknownType() : resolve(a.args[1], u));
            } else if (const GlobalSym* g = n.find('.') == std::string::npos && localClass(n) == nullptr ? findGlobal(u, n) : nullptr;
                       g != nullptr && g->alias != nullptr) {
                return expandAlias(*g, a, u);  // D41
            } else if (ClassInfo* k = findClass(u, n, classAround()); k != nullptr) {
                t.kind = Type::Class;
                t.cls = k;
                for (size_t i = 0; i < k->typeParams.size(); i++)
                    t.args.push_back(i < a.args.size() ? resolve(a.args[i], u) : unknownType());
            } else {
                if (std::string advice = removedName(n); !advice.empty()) report(a, advice);
                return unknownType();  // a name only the VM knows
            }
        }
        t.nullable = a.nullable;
        // `open List<int>` (D32), `open List<accepts Button>` (D34).
        if (a.isOpen) {
            if ((t.kind == Type::List || t.kind == Type::Map || t.kind == Type::Set) && !t.frozen) t.open = true;
            else if (t.known()) report(a, "only a List, a Map or a Set is open, which may be changed through the name it has (D32)");
        }
        for (size_t i = 0; i < a.args.size() && i < t.args.size(); i++) {
            if (!a.args[i].accepts) continue;
            const bool slot = t.kind == Type::List || t.kind == Type::Set || (t.kind == Type::Map && i == 1);
            if (t.open && slot) t.args[i].accepts = true;
            else report(a.args[i], "accepts goes in an open list's, set's or map's items, as in open List<accepts Button>: what may be added (D34)");
        }
        return t;
    }

    // What a map of `map`'s type gives in a loop: its entries, MapEntry<K, V> (D64).
    Type mapEntryOf(const Type& map) {
        ClassInfo* k = unit != nullptr ? findClass(*unit, "MapEntry", nullptr) : nullptr;
        if (k == nullptr) return unknownType();
        Type t = classType(k);
        if (map.args.size() == 2) t.args = {map.args[0], map.args[1]};
        return t;
    }

    Type classType(ClassInfo* k) {
        Type t = Type::of(Type::Class);
        t.cls = k;
        return t;
    }

    // A class as seen from inside it: Box<T>.
    Type selfType(ClassInfo* k) {
        Type t = classType(k);
        for (const std::string& p : k->typeParams) t.args.push_back(typeParamType(p));
        return t;
    }

    void resolveSupers(ClassInfo* k) {
        if (k->supersResolved) return;
        k->supersResolved = true;
        TParamsSaved saved(*this);
        FnCtx* savedCtx = ctx;
        FnCtx temp;
        temp.cls = k->outer;
        ctx = &temp;
        pushClassTypeParams(k);
        const ClassStmt& c = *k->decl;
        for (size_t i = 0; i < c.supertypes.size(); i++) {
            ClassInfo* s = findClass(*k->unit, c.supertypes[i], k->outer);
            if (s == nullptr) {
                k->unknownSuper = true;
                continue;
            }
            std::vector<Type> args;
            if (i < c.supertypeTypes.size())
                for (const TypeAnnotation& a : c.supertypeTypes[i].args) args.push_back(resolve(a, *k->unit));
            if (s->isValue) {
                report(c, "'" + k->name + "' cannot extend value '" + s->name + "': a value type has no subclasses");
                continue;
            }
            if (s->isInterface) {
                k->interfaces.push_back(s);
                k->interfaceArgs.push_back(std::move(args));
            } else if (k->superclass == nullptr) {
                k->superclass = s;
                k->superArgs = std::move(args);
            } else {
                k->unknownSuper = true;
            }
        }
        ctx = savedCtx;
    }

    // Fills in a function's parameter types and written result.
    void resolveFunc(FuncInfo* f) {
        if (f->resolved) return;
        f->resolved = true;
        if (f->decl == nullptr) return;
        const FunctionDecl& d = *f->decl;
        TParamsSaved saved(*this);
        FnCtx* savedCtx = ctx;
        FnCtx temp;
        temp.cls = f->owner;
        ctx = &temp;
        pushClassTypeParams(f->owner);
        pushTypeParams(d.typeParams, *f->unit);
        if (f->isExtension && f->stmt != nullptr) f->receiver = resolve(f->stmt->receiverType, *f->unit);
        for (size_t i = 0; i < d.params.size(); i++) {
            f->paramNames.push_back(d.params[i]);
            f->params.push_back(i < d.paramTypes.size() ? resolve(d.paramTypes[i], *f->unit) : unknownType());
            f->hasDefault.push_back(i < d.paramDefaults.size() && d.paramDefaults[i] != nullptr);
        }
        f->vararg = d.varargIndex;
        if (f->vararg >= 0) {
            f->varargItem = resolve(d.varargType, *f->unit);
            f->params[static_cast<size_t>(f->vararg)] = openListOf(f->varargItem);  // made for the call (D32)
        }
        if (d.returnType.present()) {
            f->resultWritten = true;
            f->result = resolve(d.returnType, *f->unit);
            f->resultState = 2;
        } else if (f->isExternal) {
            f->result = Type::of(Type::Unit);  // a built-in that writes no result gives none
            f->resultState = 2;
        }
        ctx = savedCtx;
    }

    // The generated initializer of a class with a header, as a function.
    FuncInfo* headerFunc(ClassInfo* k) {
        if (k->headerFunc != nullptr) return k->headerFunc;
        funcStore.push_back(std::make_unique<FuncInfo>());
        FuncInfo* f = funcStore.back().get();
        f->name = "init";
        f->unit = k->unit;
        f->owner = k;
        f->resolved = true;
        f->resultWritten = true;
        f->result = Type::of(Type::Unit);
        f->resultState = 2;
        TParamsSaved saved(*this);
        FnCtx* savedCtx = ctx;
        FnCtx temp;
        temp.cls = k;
        ctx = &temp;
        pushClassTypeParams(k);
        for (const HeaderParam& p : k->decl->header) {
            f->paramNames.push_back(p.name);
            f->params.push_back(resolve(p.type, *k->unit));
            f->hasDefault.push_back(p.defaultValue != nullptr);
        }
        ctx = savedCtx;
        k->headerFunc = f;
        return f;
    }

    // ---- Members ----

    // What a name is on a class type, found on the class or one above it, with what the type
    // parameters stand for where it was found.
    struct Member {
        FieldInfo* field = nullptr;
        FuncInfo* getter = nullptr;
        FuncInfo* setter = nullptr;
        std::vector<FuncInfo*> methods;
        std::vector<Env> methodEnvs;  // per method: what the type parameters of the class it is in stand for
        Env env;
        bool found() const { return field || getter || setter || !methods.empty(); }
    };

    // What a name is on an object of a class: a field, a property or a method, the nearest
    // first. For a call (`call`), a method of the name comes before a field: `c.tooltip("x")`
    // calls the method where there is also a field called tooltip.
    Member lookupMember(const Type& obj, const std::string& name, bool call = false) {
        Member m;
        if (obj.kind != Type::Class || obj.cls == nullptr) return m;
        if (call) {
            Member methods = methodsOf(obj, name);
            if (!methods.methods.empty()) return methods;
        }
        std::unordered_set<ClassInfo*> visited;
        std::function<bool(ClassInfo*, const Env&)> walk = [&](ClassInfo* c, const Env& env) -> bool {
            if (c == nullptr || !visited.insert(c).second) return false;
            if (auto f = c->fields.find(name); f != c->fields.end()) {
                m.field = f->second;
                m.env = env;
                return true;
            }
            if (auto g = c->getters.find(name); g != c->getters.end()) {
                m.getter = g->second;
                m.env = env;
                return true;
            }
            if (auto ms = c->methods.find(name); ms != c->methods.end()) {
                m.methods = ms->second;
                m.env = env;
                return true;
            }
            if (auto s = c->setters.find(name); s != c->setters.end()) {
                m.setter = s->second;
                m.env = env;
                return true;
            }
            resolveSupers(c);
            auto envFor = [&](ClassInfo* s, const std::vector<Type>& args) {
                Env e;
                for (size_t i = 0; i < s->typeParams.size(); i++) e[s->typeParams[i]] = i < args.size() ? subst(args[i], env) : unknownType();
                return e;
            };
            if (c->superclass != nullptr && walk(c->superclass, envFor(c->superclass, c->superArgs))) return true;
            for (size_t i = 0; i < c->interfaces.size(); i++)
                if (walk(c->interfaces[i], envFor(c->interfaces[i], c->interfaceArgs[i]))) return true;
            // What an extend in reach brings to the class (D33).
            for (ClassInfo* fitted : extendedTo(c))
                if (walk(fitted, Env{})) return true;
            return false;
        };
        walk(obj.cls, envOf(obj));
        if (!m.methods.empty()) addInheritedForms(obj, name, m);
        return m;
    }

    // Whether a member is private to a class the code being checked is not written in.
    bool hiddenHere(const Member& m) {
        ClassInfo* owner = nullptr;
        uint8_t access = kAccessWordNone;
        if (m.field != nullptr) {
            owner = m.field->owner;
            if (m.field->decl != nullptr) access = m.field->decl->access;
        } else if (m.getter != nullptr) {
            owner = m.getter->owner;
            if (m.getter->decl != nullptr) access = m.getter->decl->access;
        } else if (!m.methods.empty()) {
            owner = m.methods[0]->owner;
            if (m.methods[0]->decl != nullptr) access = m.methods[0]->decl->access;
        }
        if (access != kAccessWordPrivate || owner == nullptr) return false;
        for (FnCtx* c = ctx; c != nullptr; c = c->parent)
            for (ClassInfo* k = c->cls; k != nullptr; k = k->outer)
                if (k == owner) return false;
        return true;
    }

    // The methods of a name a class has, its own or above it; none if it has none.
    Member methodsOf(const Type& obj, const std::string& name) {
        Member m;
        std::unordered_set<ClassInfo*> visited;
        std::function<bool(ClassInfo*, const Env&)> walk = [&](ClassInfo* c, const Env& env) -> bool {
            if (c == nullptr || !visited.insert(c).second) return false;
            if (auto ms = c->methods.find(name); ms != c->methods.end()) {
                m.methods = ms->second;
                m.env = env;
                return true;
            }
            resolveSupers(c);
            auto envFor = [&](ClassInfo* s, const std::vector<Type>& args) {
                Env e;
                for (size_t i = 0; i < s->typeParams.size(); i++) e[s->typeParams[i]] = i < args.size() ? subst(args[i], env) : unknownType();
                return e;
            };
            if (c->superclass != nullptr && walk(c->superclass, envFor(c->superclass, c->superArgs))) return true;
            for (size_t i = 0; i < c->interfaces.size(); i++)
                if (walk(c->interfaces[i], envFor(c->interfaces[i], c->interfaceArgs[i]))) return true;
            for (ClassInfo* fitted : extendedTo(c))  // D33
                if (walk(fitted, Env{})) return true;
            return false;
        };
        walk(obj.cls, envOf(obj));
        if (!m.methods.empty()) addInheritedForms(obj, name, m);
        return m;
    }

    // The interfaces extends in reach fit a class to (D33); none while an extend is checked
    // against the class's own members.
    bool ownMembersOnly = false;
    std::vector<ClassInfo*> extendedTo(ClassInfo* c) {
        std::vector<ClassInfo*> fitted;
        if (ownMembersOnly || c == nullptr) return fitted;
        for (ExtendInfo& e : extendsDeclared)
            if (extendInReach(e) && resolveExtend(e) && std::find(e.types.begin(), e.types.end(), c) != e.types.end())
                fitted.push_back(e.iface);
        return fitted;
    }

    // The forms of a method a class has: its own and those of the classes and interfaces
    // above it, the nearest first, a form its parameters' types repeat being one it overrides.
    void addInheritedForms(const Type& obj, const std::string& name, Member& m) {
        m.methods.clear();
        m.methodEnvs.clear();
        std::unordered_set<ClassInfo*> visited;
        std::vector<FuncInfo*> seenMethods;  // nearer ones, those overridden too
        std::function<void(ClassInfo*, const Env&)> walk = [&](ClassInfo* c, const Env& env) {
            if (c == nullptr || !visited.insert(c).second) return;
            if (auto ms = c->methods.find(name); ms != c->methods.end()) {
                for (FuncInfo* f : ms->second) {
                    bool overridden = false;
                    for (FuncInfo* nearer : seenMethods) overridden = overridden || takesPlaceOf(nearer, f);
                    seenMethods.push_back(f);
                    if (overridden) continue;
                    m.methods.push_back(f);
                    m.methodEnvs.push_back(env);
                }
            }
            resolveSupers(c);
            auto envFor = [&](ClassInfo* s, const std::vector<Type>& args) {
                Env e;
                for (size_t i = 0; i < s->typeParams.size(); i++) e[s->typeParams[i]] = i < args.size() ? subst(args[i], env) : unknownType();
                return e;
            };
            if (c->superclass != nullptr) walk(c->superclass, envFor(c->superclass, c->superArgs));
            for (size_t i = 0; i < c->interfaces.size(); i++) walk(c->interfaces[i], envFor(c->interfaces[i], c->interfaceArgs[i]));
            for (ClassInfo* fitted : extendedTo(c)) walk(fitted, Env{});  // D33
        };
        walk(obj.cls, envOf(obj));
        if (!m.methodEnvs.empty()) m.env = m.methodEnvs[0];
    }

    // Whether one method takes the place of another: as many parameters, a vararg in the same
    // place, and each parameter of the same type, or of a type not written in either.
    bool sameForm(FuncInfo* a, FuncInfo* b) {
        resolveFunc(a);
        resolveFunc(b);
        if (a->params.size() != b->params.size() || a->vararg != b->vararg) return false;
        // Two declared in one class are two forms unless they are written alike.
        if (a->owner != nullptr && a->owner == b->owner) return signatureOf(a, {}) == signatureOf(b, {});
        for (size_t i = 0; i < a->params.size(); i++) {
            const Type& x = static_cast<int>(i) == a->vararg ? a->varargItem : a->params[i];
            const Type& y = static_cast<int>(i) == b->vararg ? b->varargItem : b->params[i];
            if (x.unknown() || y.unknown()) continue;
            if (argText(x) != argText(y)) return false;
        }
        return true;
    }

    // Of an operator's several forms (`op +(by : int)`, `op +(other : Level)`), the one the operand
    // fits most narrowly; null where none does, or two as well, or the operand is not known.
    FuncInfo* operatorForm(const std::vector<FuncInfo*>& forms, const Env& env, const std::vector<Type>& args) {
        for (const Type& a : args)
            if (a.unknown()) return nullptr;
        std::vector<FuncInfo*> fitting;
        for (FuncInfo* f : forms) {
            resolveFunc(f);
            if (f->params.size() != args.size()) continue;
            bool ok = true;
            for (size_t i = 0; i < args.size() && ok; i++) ok = fits(args[i], subst(f->params[i], env));
            if (ok) fitting.push_back(f);
        }
        for (FuncInfo* f : fitting) {
            bool narrowest = true;
            for (FuncInfo* g : fitting)
                for (size_t i = 0; i < args.size() && narrowest && g != f; i++)
                    narrowest = fits(subst(f->params[i], env), subst(g->params[i], env));
            if (narrowest) return f;
        }
        return nullptr;
    }

    // Whether a method takes the place of one above it: it is the same form. An override has
    // the parameters of what it overrides; anything else is another form (D56).
    bool takesPlaceOf(FuncInfo* f, FuncInfo* g) { return f == g || sameForm(f, g); }

    // What formsAbove and familyForms found, by class and name; worked out again when more of
    // the program is checked.
    std::map<std::pair<ClassInfo*, std::string>, std::vector<FuncInfo*>> formsAboveKnown, familyKnown;

    // The forms of a method name the classes directly above a class have.
    std::vector<FuncInfo*> formsAbove(ClassInfo* k, const std::string& name) {
        auto known = formsAboveKnown.find({k, name});
        if (known != formsAboveKnown.end()) return known->second;
        resolveSupers(k);
        std::vector<ClassInfo*> supers = k->interfaces;
        if (k->superclass != nullptr) supers.insert(supers.begin(), k->superclass);
        std::vector<FuncInfo*> forms;
        for (ClassInfo* s : supers) {
            Member m;
            addInheritedForms(selfType(s), name, m);
            for (FuncInfo* f : m.methods)
                if (isPlainMethod(f) && std::find(forms.begin(), forms.end(), f) == forms.end()) forms.push_back(f);
        }
        formsAboveKnown[{k, name}] = forms;
        return forms;
    }

    // ---- Methods with several forms, for the compiler ----

    // The classes each class is the superclass or an interface of.
    std::unordered_map<ClassInfo*, std::vector<ClassInfo*>> below;
    size_t belowCounted = 0;

    void countBelow() {
        for (; belowCounted < classStore.size(); belowCounted++) {
            ClassInfo* k = classStore[belowCounted].get();
            resolveSupers(k);
            if (k->superclass != nullptr) below[k->superclass].push_back(k);
            for (ClassInfo* i : k->interfaces) below[i].push_back(k);
        }
    }

    static bool isPlainMethod(const FuncInfo* f) {
        return f->decl != nullptr && !f->isStatic && f->decl->accessor == FunctionDecl::Accessor::None && f->name != "init";
    }

    // The classes above a class, the nearest first: its superclass and interfaces, theirs, ...
    std::vector<ClassInfo*> above(ClassInfo* k) {
        std::unordered_set<ClassInfo*> visited{k};
        std::vector<ClassInfo*> order{k};
        for (size_t i = 0; i < order.size(); i++) {
            ClassInfo* c = order[i];
            resolveSupers(c);
            std::vector<ClassInfo*> supers = c->interfaces;
            if (c->superclass != nullptr) supers.insert(supers.begin(), c->superclass);
            for (ClassInfo* s : supers)
                if (visited.insert(s).second) order.push_back(s);
        }
        order.erase(order.begin());
        return order;
    }

    // The forms a method name has among the classes a class is related to: every class below
    // the highest ones that declare it, above the class or the class itself. One of each.
    std::vector<FuncInfo*> familyForms(ClassInfo* k, const std::string& name) {
        auto known = familyKnown.find({k, name});
        if (known != familyKnown.end()) return known->second;
        countBelow();
        std::vector<ClassInfo*> tops;
        std::vector<ClassInfo*> chain = above(k);
        chain.insert(chain.begin(), k);
        for (ClassInfo* c : chain) {
            if (!c->methods.count(name)) continue;
            bool higher = false;
            for (ClassInfo* a : above(c)) higher = higher || a->methods.count(name) != 0;
            if (!higher) tops.push_back(c);
        }
        std::vector<FuncInfo*> forms;
        std::vector<FuncInfo*> seenMethods;
        std::unordered_set<ClassInfo*> visited;
        std::function<void(ClassInfo*)> walk = [&](ClassInfo* c) {
            if (!visited.insert(c).second) return;
            if (auto ms = c->methods.find(name); ms != c->methods.end()) {
                for (FuncInfo* f : ms->second) {
                    if (!isPlainMethod(f)) continue;
                    bool known = false;
                    for (FuncInfo* g : seenMethods) known = known || takesPlaceOf(f, g);
                    seenMethods.push_back(f);
                    if (!known) forms.push_back(f);
                }
            }
            auto b = below.find(c);
            if (b != below.end())
                for (ClassInfo* d : b->second) walk(d);
        };
        for (ClassInfo* t : tops) walk(t);
        familyKnown[{k, name}] = forms;
        return forms;
    }

    // The highest declaration of the form a method is: what it overrides, or itself.
    FuncInfo* rootOf(FuncInfo* f) {
        if (f->owner == nullptr) return f;
        for (FuncInfo* g : formsAbove(f->owner, f->name))
            if (g != f && takesPlaceOf(f, g)) return rootOf(g);
        return f;
    }

    const FunctionDecl* formRoot(const FunctionDecl& method) {
        auto found = funcOf.find(&method);
        if (found == funcOf.end()) return nullptr;
        FuncInfo* f = found->second;
        if (f->owner == nullptr || !isPlainMethod(f)) return nullptr;
        if (familyForms(f->owner, f->name).size() < 2) return nullptr;
        return rootOf(f)->decl;
    }

    std::vector<const FunctionDecl*> formRoots(const ClassStmt& cls, const std::string& name) {
        std::vector<const FunctionDecl*> roots;
        auto found = classOf.find(&cls);
        if (found == classOf.end()) return roots;
        ClassInfo* k = found->second;
        if (familyForms(k, name).size() < 2) return roots;
        Member m;
        addInheritedForms(selfType(k), name, m);
        for (FuncInfo* f : m.methods) {
            if (!isPlainMethod(f)) continue;
            const FunctionDecl* root = rootOf(f)->decl;
            if (std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
        }
        return roots;
    }

    // What tells a function's forms apart: its parameters' types.
    std::string signatureOf(FuncInfo* f, const Env& env) {
        resolveFunc(f);
        std::string sig;
        for (size_t i = 0; i < f->params.size(); i++) {
            if (i > 0) sig += ", ";
            if (static_cast<int>(i) == f->vararg) sig += "vararg ";
            sig += argText(subst(static_cast<int>(i) == f->vararg ? f->varargItem : f->params[i], env));
        }
        return sig;
    }

    // A form as an error shows it: `area(w : float, h : float)`.
    std::string formText(const std::string& what, FuncInfo* f, const Env& env) {
        resolveFunc(f);
        std::string s = (f->isExtension ? argText(subst(f->receiver, env)) + "." : std::string()) + what + "(";
        for (size_t i = 0; i < f->params.size(); i++) {
            if (i > 0) s += ", ";
            if (static_cast<int>(i) == f->vararg) s += "vararg ";
            s += f->paramNames[i] + " : " + argText(subst(static_cast<int>(i) == f->vararg ? f->varargItem : f->params[i], env));
        }
        return s + ")";
    }

    // The names a built-in type's own methods and properties are declared under.
    static std::vector<std::string> builtinKeys(const Type& t) {
        switch (t.kind) {
            case Type::String: return {"string"};
            case Type::List: return {"List"};
            case Type::Map: return {"Map"};
            case Type::Range: return {"Range"};
            case Type::Bytes: return {"Bytes"};
            case Type::Int: return {"int", "Number"};
            case Type::Float: return {"float", "Number"};
            case Type::Number: return {"Number"};
            case Type::Char: return {"char"};
            case Type::Byte: return {"byte"};
            case Type::Index: return {"Index"};
            case Type::Set: return {"Set"};
            default: return {};
        }
    }

    // A property the C++ provides on a built-in type, with its type parameters filled in from
    // the value's type (a Map<string, int>'s keys are a List<string>).
    bool builtinProperty(const Type& t, const std::string& name, Type& found) {
        for (const std::string& key : builtinKeys(t)) {
            auto type = builtinProps.find(key);
            if (type == builtinProps.end()) continue;
            auto prop = type->second.find(name);
            if (prop == type->second.end()) continue;
            BuiltinProp& p = prop->second;
            if (!p.resolved) {
                p.resolved = true;
                TParamsSaved saved(*this);
                pushTypeParams(p.decl->typeParams, *p.unit);
                p.receiver = resolve(p.decl->receiverType, *p.unit);
                p.type = resolve(p.decl->type, *p.unit);
            }
            Env env;
            unify(p.receiver, withNull(t, false), env, p.typeParams);
            found = finish(p.type, env, p.typeParams);
            return true;
        }
        return false;
    }

    // The extension functions of that name that apply to a value of type `t`: for a built-in
    // type, its own methods (in C++) first, as when the program runs.
    std::vector<FuncInfo*> findExtensions(const Type& t, const std::string& name) {
        for (const std::string& key : builtinKeys(t)) {
            auto type = builtinMethods.find(key);
            if (type == builtinMethods.end()) continue;
            auto found = type->second.find(name);
            if (found != type->second.end()) return found->second;
        }
        auto in = [&](const std::string& key) -> const std::vector<FuncInfo*>* {
            auto e = extensions.find(key);
            if (e == extensions.end()) return nullptr;
            auto f = e->second.find(name);
            return f == e->second.end() ? nullptr : &f->second;
        };
        std::vector<std::string> keys;
        switch (t.kind) {
            case Type::Class: {
                std::unordered_set<ClassInfo*> visited;
                std::function<void(ClassInfo*)> walk = [&](ClassInfo* c) {
                    if (c == nullptr || !visited.insert(c).second) return;
                    keys.push_back(c->name);
                    resolveSupers(c);
                    walk(c->superclass);
                    for (ClassInfo* i : c->interfaces) walk(i);
                };
                walk(t.cls);
                break;
            }
            case Type::List: keys.push_back("List"); break;
            case Type::Map: keys.push_back("Map"); break;
            case Type::String: keys.push_back("string"); break;
            case Type::Bytes: keys.push_back("Bytes"); break;
            case Type::Range: keys.push_back("Range"); break;
            case Type::Int: keys.push_back("int"); keys.push_back("Number"); break;
            case Type::Float: keys.push_back("float"); keys.push_back("Number"); break;
            case Type::Number: keys.push_back("Number"); break;
            case Type::Char: keys.push_back("char"); break;
            case Type::Byte: keys.push_back("byte"); break;
            case Type::Index: keys.push_back("Index"); break;
            case Type::Set: keys.push_back("Set"); break;
            case Type::Bool: keys.push_back("bool"); break;
            case Type::Function: keys.push_back("Function"); break;
            default: break;
        }
        keys.push_back("any");
        keys.push_back("<T>");
        for (const std::string& key : keys) {
            const auto* all = in(key);
            if (all == nullptr) continue;
            // In reach as names are: the file's own folder first, then what it imports, and the
            // standard library's everywhere (D70).
            std::vector<FuncInfo*> reached, ownFolder;
            for (FuncInfo* f : *all) {
                if (!declarationInReach(f->unit, f->stmt != nullptr ? f->stmt->visibility : 0)) continue;
                reached.push_back(f);
                if (unit != nullptr && f->unit != nullptr && !f->unit->prelude && f->unit->ns == unit->ns) ownFolder.push_back(f);
            }
            if (reached.empty()) continue;
            const std::vector<FuncInfo*> reachable = ownFolder.empty() ? reached : ownFolder;
            const auto* found = &reachable;
            // A built-in type's own methods (in C++) come before extensions for every type when
            // the program runs, and the checker does not know them yet (part 3): the standard
            // library's `any?.equals` is not what `"a".equals("A", ignoreCase = true)` calls.
            // The scope functions are no type's own (D58).
            const bool scope = name == "let" || name == "also" || name == "apply" || name == "run" || name == "takeIf" || name == "takeUnless" || name == "to";
            if (t.kind != Type::Class && (key == "any" || key == "<T>") && !scope) {
                std::vector<FuncInfo*> own;
                for (FuncInfo* f : *found)
                    if (f->unit == nullptr || !f->unit->prelude) own.push_back(f);
                if (own.empty()) continue;
                return own;
            }
            return *found;
        }
        return {};
    }

    // What a name the standard library no longer has is written (D74, D130, D140), or "".
    static std::string goneAdvice(const std::string& name) {
        static const std::unordered_map<std::string, std::string> gone = {
            {"length", "'length' is written 'size': every count of things is size (D130)"},
            {"toByteArray", "toByteArray(charset) is written toBytes(charset), and String(bytes, charset) bytes.decodeToString(charset) (D127)"},
            {"push", "push() is written add(), or addAll() for several: one name for each thing (D74)"},
            {"pop", "pop() is written removeLast(): one name for each thing (D74)"},
            {"uppercase", "uppercase() is written upper(): one name for each thing (D74)"},
            {"lowercase", "lowercase() is written lower(): one name for each thing (D74)"},
            {"joinToString", "joinToString() is written join(), which takes the same arguments: one name for each thing (D74)"},
            {"each", "each { } is written forEach { }: one name for each thing (D74)"},
            {"toLong", "toLong() is written toInt(): an int is 64 bits (D74)"},
            {"toLongOrNull", "toLongOrNull() is written toIntOrNull(): an int is 64 bits (D74)"},
            {"toDouble", "toDouble() is written toFloat(): a float is 64 bits (D74)"},
            {"toDoubleOrNull", "toDoubleOrNull() is written toFloatOrNull(): a float is 64 bits (D74)"},
            {"roundToLong", "roundToLong() is written roundToInt(): an int is 64 bits (D74)"},
            {"has", "has(key) is written keys.contains(key), or key in map.keys (D140)"},
            {"containsKey", "containsKey(key) is written keys.contains(key), or key in map.keys (D140)"},
            {"containsValue", "containsValue(value) is written values.contains(value), or value in map.values (D140)"},
            {"slice", "slice(a, b) is written items[a..<b], a slice; items[a..] runs to the end, and items[^k..] takes the last k (D147)"},
            {"substring", "substring(a, b) is written s[a..<b], a slice; s[a..] runs to the end (D147)"},
            {"subList", "subList(a, b) is written items[a..<b], a slice (D147)"},
            {"keys", "keys() is read without brackets: map.keys, a live view of the map's keys (D77)"},
            {"values", "values() is read without brackets: map.values (D77)"},
            {"entries", "entries() is read without brackets: map.entries, a live view of the map's entries (D77)"},
            {"toMutableList", "toMutableList() is written toOpenList(): a copy that can be changed (D46)"},
            {"toMutableMap", "toMutableMap() is written toOpenMap(): a copy that can be changed (D46)"},
            {"toMutableSet", "toMutableSet() is written toOpenSet(): a copy that can be changed (D46)"},
            {"len", "len(x) is written x.size (D74)"},
            {"str", "str(x) is written x.toString(), or \"$x\" (D74)"},
            {"int", "int(x) is written x.toInt() (D74)"},
            {"float", "float(x) is written x.toFloat() (D74)"},
        };
        auto found = gone.find(name);
        return found == gone.end() ? std::string() : found->second;
    }

    // What Java's and Kotlin's names for threads and waiting are in Astra (D81 to D90, D118):
    // each a compile error saying what to write, where nothing of the program declares the name.
    static std::string removedName(const std::string& name) {
        static const std::unordered_map<std::string, std::string> gone = {
            {"Future", "a result to come is a Job<T> in Astra, from task { }, thread { } or a pool's submit { }: then { }, get(timeout = ...) (D89)"},
            {"CompletableFuture", "a result to come is a Job<T> in Astra, made by hand as Job<T>() and given its value with complete(v) or fail(e) (D89)"},
            {"Executor", "threads that take jobs are a ThreadPool(threads = n) in Astra, and one thread taking them in order a JobQueue(name): submit { } gives a Job (D88)"},
            {"ExecutorService", "threads that take jobs are a ThreadPool(threads = n) in Astra, and one thread taking them in order a JobQueue(name): submit { } gives a Job (D88)"},
            {"ScheduledExecutorService", "a ThreadPool runs timed jobs in Astra: pool.after(1.seconds) { }, pool.every(5.seconds) { } (D88)"},
            {"Executors", "Executors.newFixedThreadPool(n) is ThreadPool(threads = n) in Astra, newSingleThreadExecutor() a JobQueue(name) (D88)"},
            {"BlockingQueue", "a queue threads hand things over on is a Channel in Astra: Channel<T>.unbounded(), or Channel<T>(capacity = n) holding n at most (D87)"},
            {"LinkedBlockingQueue", "a LinkedBlockingQueue is Channel<T>.unbounded() in Astra: send, receive, and a for over it (D87)"},
            {"ArrayBlockingQueue", "an ArrayBlockingQueue is a Channel<T>(capacity = n) in Astra (D87)"},
            {"ConcurrentLinkedQueue", "a ConcurrentLinkedQueue is a Channel<T>.unbounded() in Astra, used with trySend and tryReceive (D87)"},
            {"CountDownLatch", "a CountDownLatch is Countdown(n) in Astra: countDown(), await(timeout = ...), count (D118)"},
            {"AtomicInteger", "Java's atomics are one Atomic<T> in Astra: Atomic(0), with get, set, update { }, exchange, compareAndSet, and increment, decrement and add on an int (D85)"},
            {"AtomicLong", "Java's atomics are one Atomic<T> in Astra: Atomic(0), an int being 64 bits (D85)"},
            {"AtomicBoolean", "Java's atomics are one Atomic<T> in Astra: Atomic(false) (D85)"},
            {"AtomicReference", "Java's atomics are one Atomic<T> in Astra: Atomic<Frame?>(null) (D85)"},
            {"TimeUnit", "a time is a Duration in Astra: 16.milliseconds, 2.seconds, 5.minutes (D90)"},
            {"synchronized", "synchronized(x) { } is a Lock's useKey in Astra: val lock = Lock(), then lock.useKey { ... }; data reached only with the key is a LockBox(value) (D86)"},
            {"ReentrantLock", "a ReentrantLock is a Lock() in Astra, used with useKey { } (D86)"},
            {"startCoroutine", "startCoroutine is task { ... } in Astra, which gives a Job of what the block returns (D81)"},
            {"suspendCoroutine", "suspendCoroutine is suspendTask { resume -> ... } in Astra: resume(value) takes the task up again, resume.fail(e) throws e there (D81)"},
            {"Continuation", "a Continuation is suspendTask's resume in Astra, a plain function: resume(value), resume.fail(e) (D81)"},
            {"EmptyCoroutineContext", "a task needs no context in Astra: task { ... } (D81)"},
            // Files and input and output (D91, D107, D119, D127, D131, D136).
            {"File", "a file or directory is a Path in Astra: Path(\"notes.txt\"), Path(directory, name); Path(p).readText(), writeText(text), appendText(text) and exists() in place of File.read, write, append and exists (D107, D136)"},
            {"Files", "Files.x(p) is p.x() in Astra, on the Path: p.readText(), p.readBytes(), p.mkdir(), a.moveTo(b), p.list(), p.walk() (D131)"},
            {"Paths", "Paths.get(s) is Path(s) in Astra (D131)"},
            {"InputStream", "something to read from is a Reader in Astra, bytes and text alike: path.reader(), process.output, socket.reader (D119)"},
            {"OutputStream", "something to write to is a Writer in Astra, bytes and text alike: path.writer(append = ...), process.input, socket.writer (D119)"},
            {"BufferedReader", "a Reader in Astra reads ahead already: path.reader() (D119)"},
            {"BufferedWriter", "a Writer in Astra: path.writer() (D119)"},
            {"PrintWriter", "a Writer in Astra has print and println: path.writer() (D119)"},
            {"ProcessBuilder", "a program is started with Process(command..., directory = ..., environment = {...}, input = ..., output = ..., error = ...) in Astra, or run to its end with Process.run(...) (D91)"},
            {"Charsets", "a character set is a Charset in Astra: Charset.utf8, latin1, ascii, utf16le, utf16be (D127)"},
            {"StandardCharsets", "a character set is a Charset in Astra: Charset.utf8, latin1, ascii, utf16le, utf16be (D127)"},
            // Graphics (D99, D101 to D106, L79 to L86, L101, L102).
            {"Dimension", "a width and a height are a Size in Astra, a value type: Size(width, height) (L82)"},
            {"Rectangle", "a rectangle is a Rect in Astra, a value type: Rect(x, y, width, height) (L84)"},
            {"BufferedImage", "pixels in memory are an Image in Astra: Image(width, height), image[x, y] a Color, image.canvas() to draw on it (L86, L101)"},
            {"Picture", "a Picture is an Image in Astra: Image(width, height, color), image[x, y], image.canvas() (L101)"},
            {"FontMetrics", "a font measures itself in Astra: font.width(text), font.ascent, font.descent, font.leading, font.lineHeight, and font.width(text, scale) in a screen's pixels (L81)"},
            {"Bitmap", "pixels in memory are an Image in Astra: its pixels are changed in image.editPixels { pixels -> pixels[x, y] = color } (L110)"},
            {"Graphics", "what draws is a Canvas in Astra: image.canvas(), surface.canvas(), or the one a widget paints on (D123)"},
            {"Graphics2D", "what draws is a Canvas in Astra: image.canvas(), surface.canvas(), or the one a widget paints on (D123)"},
        };
        auto found = gone.find(name);
        return found == gone.end() ? std::string() : found->second;
    }
    // What Java's members of a file, a stream, a process or a socket are in Astra (D91, D107,
    // D119, D120, D126, D128 to D131, D135, D137, D138), by the class of the standard library
    // they were asked of.
    static std::string ioAdvice(const std::string& cls, const std::string& name) {
        static const std::unordered_map<std::string, std::string> path = {
            {"parentFile", "parentFile is written parent, a Path: path.parent?.name, path.parent?.text (D128)"},
            {"absolutePath", "absolutePath is written absolute, a Path: path.absolute.text for the text (D135)"},
            {"absoluteFile", "absoluteFile is written absolute, a Path (D135)"},
            {"canonicalPath", "canonicalPath is written canonical(), a Path: path.canonical().text for the text (D135)"},
            {"canonicalFile", "canonicalFile is written canonical(), a Path (D135)"},
            {"toAbsolutePath", "toAbsolutePath() is written absolute, a Path (D135)"},
            {"toRealPath", "toRealPath() is written canonical() (D135)"},
            {"path", "the text of a Path is written text: path.text, or \"$path\" (D137)"},
            {"getPath", "the text of a Path is written text: path.text, or \"$path\" (D137)"},
            {"fileName", "fileName is written name (D131)"},
            {"listFiles", "listFiles() is written list(), which gives Paths: dir.list() (D138)"},
            {"walkTopDown", "walkTopDown() is written walk(): every path below, as a sequence (D138)"},
            {"mkdirs", "mkdirs() is written mkdir(), which makes any missing directories above too (D126)"},
            {"renameTo", "renameTo is written moveTo(target), which throws an IOException where it cannot move (D129)"},
            {"toPath", "there is one type for a path in Astra, Path: the call goes (D131)"},
            {"toFile", "there is one type for a path in Astra, Path: the call goes (D131)"},
            {"inputStream", "a file is read with path.reader() (D119)"},
            {"bufferedReader", "a file is read with path.reader(), which reads ahead already (D119)"},
            {"outputStream", "a file is written with path.writer(append = ...) (D119)"},
            {"bufferedWriter", "a file is written with path.writer(append = ...) (D119)"},
            {"printWriter", "a file is written with path.writer(append = ...), which has print and println (D119)"},
            {"isAbsolute", "a path made whole is path.absolute (D135)"},
        };
        static const std::unordered_map<std::string, std::string> stream = {
            {"bufferedReader", "a Reader reads ahead already: the call goes (D119)"},
            {"buffered", "a Reader or Writer is buffered already: the call goes (D119)"},
            {"reader", "this is a Reader already: the call goes (D119)"},
            {"bufferedWriter", "a Writer is buffered already: the call goes (D119)"},
            {"writer", "this is a Writer already: the call goes (D119)"},
            {"printWriter", "a Writer has print and println: the call goes (D119)"},
            {"readAllBytes", "readAllBytes() is written readBytes() (D119)"},
            {"lines", "the lines as a sequence are lineSequence(), and all of them readLines() (D119)"},
        };
        static const std::unordered_map<std::string, std::string> process = {
            {"inputStream", "what a program writes is process.output, a Reader (D91)"},
            {"errorStream", "what a program writes as errors is process.error, a Reader (D91)"},
            {"outputStream", "what a program reads is process.input, a Writer (D91)"},
            {"getInputStream", "what a program writes is process.output, a Reader (D91)"},
            {"getOutputStream", "what a program reads is process.input, a Writer (D91)"},
            {"getErrorStream", "what a program writes as errors is process.error, a Reader (D91)"},
            {"destroy", "a program is ended with kill() (D91)"},
            {"destroyForcibly", "a program is ended with kill() (D91)"},
            {"exitValue", "the exit code is exitCode, null while the program runs, or waitFor() (D91)"},
            {"pid", "a program's number is pid, a property (D91)"},
        };
        static const std::unordered_map<std::string, std::string> socket = {
            {"inputStream", "what comes from the other side is socket.reader (D120)"},
            {"outputStream", "what goes to the other side is socket.writer (D120)"},
            {"getInputStream", "what comes from the other side is socket.reader (D120)"},
            {"getOutputStream", "what goes to the other side is socket.writer (D120)"},
            {"soTimeout", "a read's time limit is readTimeout, a Duration: socket.readTimeout = 5.seconds (D120)"},
            {"setSoTimeout", "a read's time limit is readTimeout, a Duration: socket.readTimeout = 5.seconds (D120)"},
            {"getSoTimeout", "a read's time limit is readTimeout, a Duration (D120)"},
        };
        const std::unordered_map<std::string, std::string>* table = cls == "Path" ? &path
                                                                 : cls == "Reader" || cls == "Writer" ? &stream
                                                                 : cls == "Process" ? &process
                                                                 : cls == "Socket" ? &socket : nullptr;
        if (table == nullptr) return "";
        auto found = table->find(name);
        return found == table->end() ? std::string() : found->second;
    }

    // What Java's members of the graphics types are in Astra (D99, D101 to D106, D141, D144,
    // L79 to L86, L101, L102, L109), by the class of the standard library they were asked of.
    static std::string graphicsAdvice(const std::string& cls, const std::string& name) {
        static const std::unordered_map<std::string, std::string> color = {
            {"getRed", "a colour's parts are properties, bytes: color.red (L79)"},
            {"getGreen", "a colour's parts are properties, bytes: color.green (L79)"},
            {"getBlue", "a colour's parts are properties, bytes: color.blue (L79)"},
            {"getAlpha", "a colour's parts are properties, bytes: color.alpha (L79)"},
            {"getRGB", "the colour as one number is color.argb, 0xAARRGGBB (L79)"},
            {"rgb", "the colour as one number is color.argb, 0xAARRGGBB (L79)"},
            {"getTransparency", "a colour is see-through where color.alpha is below 255 (L79)"},
        };
        static const std::unordered_map<std::string, std::string> font = {
            {"deriveFont", "a font with options changed is font.copy(size = ..., bold = ..., italic = ..., weight = ...) (D144)"},
            {"derive", "a font with options changed is font.copy(size = ..., bold = ..., italic = ...) (D144)"},
            {"style", "a font says its style as isBold and isItalic (L80)"},
            {"getStyle", "a font says its style as isBold and isItalic (L80)"},
            {"isPlain", "a plain font is one that is neither bold nor italic: !font.isBold and !font.isItalic (L80)"},
            {"plain", "a font's style is said by name: Font(name, size), Font(name, size, bold = true, italic = true) (L80)"},
            {"bold", "a font's style is said by name: Font(name, size, bold = true) (L80)"},
            {"italic", "a font's style is said by name: Font(name, size, italic = true) (L80)"},
            {"getSize", "a font's size is font.size (L80)"},
            {"getName", "the family asked for is font.name, the one used font.family (L80)"},
            {"getFamily", "the family used is font.family (L80)"},
            {"getFontName", "the typeface's full name is font.fontName (L80)"},
            {"textFont", "a font measures and is drawn by itself: font.width(text, scale), font.ascent(scale) (L102)"},
            {"stringWidth", "a text's width is font.width(text) (L81)"},
            {"charWidth", "a character's width is font.width(c) (L81)"},
            {"height", "the height of a line is font.lineHeight (L81)"},
            {"getHeight", "the height of a line is font.lineHeight (L81)"},
            {"getAscent", "a font's ascent is font.ascent (L81)"},
            {"getDescent", "a font's descent is font.descent (L81)"},
            {"getLeading", "a font's leading is font.leading (L81)"},
            {"maxAscent", "a font's ascent is font.ascent: no glyph reaches past it (L81)"},
            {"maxDescent", "a font's descent is font.descent: no glyph reaches past it (L81)"},
        };
        static const std::unordered_map<std::string, std::string> canvas = {
            {"drawImage", "a picture is drawn with canvas.draw(picture, x, y), or draw(picture, x, y, w, h, alpha = ...) to scale it (L86)"},
            {"drawImagePart", "a part of a picture is drawn with canvas.drawPart(picture, sx, sy, sw, sh, x, y, w, h) (L86)"},
            {"getFontMetrics", "a font measures itself: canvas.font.width(text), or canvas.measure(text) for its width, ascent, descent and height (L81)"},
            {"fontMetrics", "a font measures itself: canvas.font.width(text), or canvas.measure(text) for its width, ascent, descent and height (L81)"},
            {"style", "a canvas has no style in Astra: a widget's content paints in owner.paintStyle (L109)"},
            {"withStyle", "a canvas has no style in Astra: a widget's content paints in owner.paintStyle (L109)"},
            {"setColor", "the colour is a property: canvas.color = Color.red"},
            {"getColor", "the colour is a property: canvas.color"},
            {"setFont", "the font is a property: canvas.font = Font(name, size)"},
            {"getFont", "the font is a property: canvas.font"},
            {"create", "a canvas for a part is canvas.sub(x, y, width, height), or clipped(x, y, w, h) at the same corner (D123)"},
        };
        static const std::unordered_map<std::string, std::string> image = {
            {"getRGB", "a pixel is image[x, y], a Color: image[x, y].argb for the number (D141)"},
            {"setRGB", "a pixel is set with image[x, y] = color: Color(argb, hasAlpha = true) for a number (D141)"},
            {"getSubimage", "a part of an image is image.crop(x, y, width, height), a new image (L101)"},
            {"createGraphics", "something to draw on an image with is image.canvas() (L101)"},
            {"getGraphics", "something to draw on an image with is image.canvas() (L101)"},
            {"graphics", "something to draw on an image with is image.canvas() (L101)"},
            {"type", "an Image is always four bytes a pixel, red, green, blue and alpha: it has no type (L86)"},
            {"getType", "an Image is always four bytes a pixel, red, green, blue and alpha: it has no type (L86)"},
            {"getWidth", "an image's width is image.width"},
            {"getHeight", "an image's height is image.height"},
            {"drawText", "text is drawn with a canvas: val c = image.canvas(); c.font = font; c.drawString(text, x, y) (L101)"},
            {"fillRect", "an image is drawn on with its canvas: val c = image.canvas(); c.color = color; c.fillRect(x, y, w, h) (L101)"},
            {"blendRect", "an image is drawn on with its canvas, which lays see-through colours over what is there: image.canvas().fillRect(x, y, w, h) (L101)"},
            {"draw", "an image is drawn on with its canvas: image.canvas().draw(picture, x, y) (L101)"},
            {"fromPicture", "a Picture and an Image are one type, Image (L101)"},
            {"toPicture", "a Picture and an Image are one type, Image: the call goes (L101)"},
        };
        static const std::unordered_map<std::string, std::string> size = {
            {"getWidth", "a Size's width is size.width"},
            {"getHeight", "a Size's height is size.height"},
            {"setSize", "a Size is a value: write size = Size(width, height), or set size.width where it is kept (L82)"},
        };
        static const std::unordered_map<std::string, std::string> rect = {
            {"bounds", "a Rect is its own bounds: the call goes (L84)"},
            {"getBounds", "a Rect is its own bounds: the call goes (L84)"},
            {"getX", "a Rect's corner is rect.x and rect.y"},
            {"getY", "a Rect's corner is rect.x and rect.y"},
            {"getWidth", "a Rect's width is rect.width"},
            {"getHeight", "a Rect's height is rect.height"},
            {"getSize", "a Rect's width and height are rect.size, a Size"},
            {"getLocation", "a Rect's corner is rect.location, a Point"},
        };
        static const std::unordered_map<std::string, std::string> point = {
            {"getX", "a Point's parts are point.x and point.y"},
            {"getY", "a Point's parts are point.x and point.y"},
            {"move", "a Point is moved with setLocation(x, y), or translate(dx, dy), where it is kept (L83)"},
            {"getLocation", "a Point is its own location: the call goes (L83)"},
        };
        const std::unordered_map<std::string, std::string>* table = cls == "Color" ? &color
                                                                 : cls == "Font" ? &font
                                                                 : cls == "Canvas" ? &canvas
                                                                 : cls == "Image" ? &image
                                                                 : cls == "Size" ? &size
                                                                 : cls == "Rect" ? &rect
                                                                 : cls == "Point" ? &point : nullptr;
        if (table == nullptr) return "";
        auto found = table->find(name);
        return found == table->end() ? std::string() : found->second;
    }

    // The advice for a member a class does not have, by the first class of the standard library
    // in its line: a Jade NativeCanvas is asked as a Canvas.
    static std::string preludeAdvice(const ClassInfo* k, const std::string& name) {
        for (const ClassInfo* c = k; c != nullptr; c = c->superclass) {
            if (c->unit == nullptr || !c->unit->prelude) continue;
            std::string advice = ioAdvice(c->name, name);
            if (advice.empty()) advice = graphicsAdvice(c->name, name);
            return advice;
        }
        return "";
    }

    static std::string lockAdvice(const std::string& name) {
        if (name == "lock" || name == "unlock" || name == "tryLock" || name == "withLock")
            return "a Lock is used with useKey { }: lock.useKey { ... } takes the key, and gives it back however the block ends (D86)";
        if (name == "wait" || name == "notify" || name == "notifyAll")
            return "inside useKey { }, waitUntil { condition } gives the key back until the condition holds, with no notify to call (D86)";
        return "";
    }

    // Members every object has, which the VM gives without a declaration.
    static bool builtinMember(const std::string& name) {
        return name == "toString" || name == "hashCode" || name == "equals";
    }

    // Whether a member not found on a class should be reported. Every field is declared
    // (R24), so a class's members are known unless one of its supertypes cannot be seen.
    bool reportMissing(ClassInfo* k, const std::string& name) {
        if (hasUnknownSuper(k) || builtinMember(name)) return false;
        if (k->isEnum && (name == "name" || name == "ordinal")) return false;
        return true;
    }

    // Whether every member of a type is known to the checker, so a name not found is not there.
    bool membersKnown(const Type& t) {
        if (t.kind == Type::Class) return t.cls != nullptr && !hasUnknownSuper(t.cls);
        return false;
    }

    // ---- Working out types not written ----

    Type fieldType(FieldInfo* f) {
        if (f->state == 2) return f->type;
        if (f->state == 1) return unknownType();
        f->state = 1;
        Type t;
        Unit* savedUnit = unit;
        FnCtx* savedCtx = ctx;
        auto savedScopes = std::move(scopes);
        auto savedNarrowed = std::move(narrowed);
        TParamsSaved savedParams(*this);
        scopes.clear();
        narrowed.clear();
        unit = f->unit;
        FnCtx temp;
        temp.cls = f->owner;
        temp.topLevel = f->owner == nullptr;
        ctx = &temp;
        pushClassTypeParams(f->owner);
        if (f->decl != nullptr && f->decl->type.present()) {
            f->typeWritten = true;
            t = resolve(f->decl->type, *f->unit);
        } else if (f->header != nullptr && f->header->type.present()) {
            f->typeWritten = true;
            t = resolve(f->header->type, *f->unit);
        } else if (f->decl != nullptr && f->decl->init != nullptr) {
            silent++;
            push();
            t = expr(*f->decl->init);
            pop();
            silent--;
            if (t.kind == Type::Null || t.kind == Type::Nothing) t = unknownType();
        } else if (f->decl != nullptr && f->decl->getter != nullptr && f->decl->getter->returnType.present()) {
            t = resolve(f->decl->getter->returnType, *f->unit);
        }
        unit = savedUnit;
        ctx = savedCtx;
        scopes = std::move(savedScopes);
        narrowed = std::move(savedNarrowed);
        f->type = t;
        f->state = 2;
        return t;
    }

    Type resultOf(FuncInfo* f) {
        resolveFunc(f);
        if (f->resultState == 2) return f->result;
        if (f->resultState == 1) return unknownType();  // it calls itself: part 3 asks for the type written
        if (f->decl == nullptr || f->decl->isAbstract || f->decl->body.empty()) {
            f->result = f->decl != nullptr && f->decl->isAbstract ? unknownType() : Type::of(Type::Unit);
            f->resultState = 2;
            return f->result;
        }
        f->resultState = 1;
        std::vector<Type> returns;
        silent++;
        checkFunctionBody(f, &returns);
        silent--;
        Type r;
        if (returns.empty()) {
            r = Type::of(Type::Unit);
        } else {
            r = returns[0];
            for (size_t i = 1; i < returns.size(); i++) r = common(r, returns[i]);
        }
        if (r.kind == Type::Null) r = unknownType();
        f->result = r;
        f->resultState = 2;
        return r;
    }

    // ---- Scopes ----

    void push() {
        scopes.emplace_back();
        narrowed.emplace_back();
        localClasses.emplace_back();
    }
    void pop() {
        scopes.pop_back();
        narrowed.pop_back();
        localClasses.pop_back();
    }

    ClassInfo* localClass(const std::string& name) {
        for (auto it = localClasses.rbegin(); it != localClasses.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return nullptr;
    }

    Local* findLocal(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    // The type a local has here, narrowed by the tests around this point.
    Type localType(const std::string& name, const Local& l) {
        for (auto it = narrowed.rbegin(); it != narrowed.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return l.type;
    }

    void declareLocal(const std::string& name, Type type, bool isVal) {
        if (scopes.empty()) push();
        scopes.back()[name] = Local{std::move(type), isVal};
        for (auto& n : narrowed) n.erase(name);
    }

    // A local given a new value no longer has what a test showed about the old one.
    void forget(const std::string& name) {
        for (auto& n : narrowed) n.erase(name);
    }

    // ---- Narrowing ----

    // What `cond` being true (or false) says about locals: `x != null`, `x is Box`, and their
    // combinations with `and`, `or` and `!`.
    void narrowFrom(const Expr& cond, bool whenTrue, std::unordered_map<std::string, Type>& into) {
        switch (cond.kind) {
            case ExprKind::Unary: {
                const auto& u = static_cast<const UnaryExpr&>(cond);
                if (u.op == TokenType::Bang) narrowFrom(*u.operand, !whenTrue, into);
                return;
            }
            case ExprKind::Logical: {
                const auto& l = static_cast<const LogicalExpr&>(cond);
                if (l.isAnd == whenTrue) {
                    narrowFrom(*l.left, whenTrue, into);
                    narrowFrom(*l.right, whenTrue, into);
                }
                return;
            }
            case ExprKind::Binary: {
                const auto& b = static_cast<const BinaryExpr&>(cond);
                const bool eq = b.op == TokenType::EqEq;
                const bool ne = b.op == TokenType::BangEq;
                if (!eq && !ne) return;
                const Expr* name = nullptr;
                if (isNullLiteral(*b.right)) name = b.left.get();
                else if (isNullLiteral(*b.left)) name = b.right.get();
                if (name == nullptr) return;
                if (name->kind == ExprKind::This) {
                    if ((ne && whenTrue) || (eq && !whenTrue)) {
                        Type t = thisType();
                        if (t.known()) into[thisKey()] = withNull(t, false);
                    }
                    return;
                }
                if (name->kind == ExprKind::Member ||
                    (name->kind == ExprKind::Name && findLocal(static_cast<const NameExpr&>(*name).name) == nullptr)) {
                    const std::string key = stablePath(*name);
                    if (!key.empty() && ((ne && whenTrue) || (eq && !whenTrue))) {
                        silent++;
                        Type t = expr(*name);
                        silent--;
                        if (t.known()) into[key] = withNull(t, false);
                    }
                    return;
                }
                if (name->kind != ExprKind::Name) return;
                // x != null when true, or x == null when false: x is not null.
                if ((ne && whenTrue) || (eq && !whenTrue)) {
                    const std::string& n = static_cast<const NameExpr&>(*name).name;
                    if (Local* l = findLocal(n)) {
                        Type t = localType(n, *l);
                        if (t.known()) into[n] = withNull(t, false);
                    }
                }
                return;
            }
            case ExprKind::Is: {
                const auto& i = static_cast<const IsExpr&>(cond);
                if (i.negated == whenTrue) {
                    // Not of that type: a local that is a union loses it (D40).
                    if (i.value->kind != ExprKind::Name || findLocal(static_cast<const NameExpr&>(*i.value).name) == nullptr) return;
                    silent++;
                    const Type now = expr(*i.value);
                    silent--;
                    if (now.kind != Type::Union) return;
                    const Type gone = resolve(i.type, *unit);
                    if (!gone.known()) return;
                    std::vector<Type> rest;
                    for (const Type& m : now.args)
                        if (!fits(m, gone)) rest.push_back(m);
                    into[static_cast<const NameExpr&>(*i.value).name] = makeUnion(rest, now.nullable && !gone.nullable);
                    return;
                }
                if (i.value->kind == ExprKind::This) {
                    if (!thisType().known()) return;
                    Type t = resolve(i.type, *unit);
                    if (t.known()) into[thisKey()] = t;
                    return;
                }
                if (i.value->kind == ExprKind::Member ||
                    (i.value->kind == ExprKind::Name && findLocal(static_cast<const NameExpr&>(*i.value).name) == nullptr)) {
                    const std::string key = stablePath(*i.value);
                    if (key.empty()) return;
                    Type t = resolve(i.type, *unit);
                    if (t.known()) into[key] = t;
                    return;
                }
                if (i.value->kind != ExprKind::Name) return;
                const std::string& n = static_cast<const NameExpr&>(*i.value).name;
                if (findLocal(n) == nullptr) return;
                Type t = resolve(i.type, *unit);
                if (t.known()) into[n] = t;
                return;
            }
            default:
                return;
        }
    }

    static bool isNullLiteral(const Expr& e) {
        return e.kind == ExprKind::Literal && static_cast<const LiteralExpr&>(e).type == LiteralExpr::Type::Null;
    }

    // Whether a statement never lets control fall through to the one after it.
    static bool alwaysLeaves(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Return:
            case StmtKind::Throw:
            case StmtKind::Break:
            case StmtKind::Continue:
                return true;
            case StmtKind::Block: {
                const auto& b = static_cast<const BlockStmt&>(s);
                return !b.statements.empty() && alwaysLeaves(*b.statements.back());
            }
            case StmtKind::If: {
                const auto& i = static_cast<const IfStmt&>(s);
                return i.otherwise != nullptr && alwaysLeaves(*i.then) && alwaysLeaves(*i.otherwise);
            }
            default:
                return false;
        }
    }

    // ---- Conditions ----

    void condition(const Expr& e, const char* what) {
        Type t = expr(e);
        if (t.known() && !(t.kind == Type::Bool && !t.nullable) && t.kind != Type::Nothing && t.kind != Type::TypeParam)
            report(e, std::string("the condition of ") + what + " must be a bool, not " + describe(t));
    }

    // ---- Destructuring ----

    // The types of the parts of a value taken apart: a data class's header fields in order,
    // or a list's items.
    std::vector<Type> partsOf(const Type& t, size_t count) {
        std::vector<Type> parts(count, unknownType());
        if (t.nullable) return parts;
        if (t.kind == Type::List && !t.args.empty()) {
            for (Type& p : parts) p = t.args[0];
        } else if (t.kind == Type::Class && t.cls != nullptr && t.cls->isData) {
            Env env = envOf(t);
            const ClassStmt& c = *t.cls->decl;
            size_t i = 0;
            for (const HeaderParam& h : c.header) {
                if (!h.isField) continue;
                if (i >= count) break;
                auto f = t.cls->fields.find(h.name);
                if (f != t.cls->fields.end()) parts[i] = subst(fieldType(f->second), env);
                i++;
            }
        }
        return parts;
    }

    // ---- Statements ----

    void block(const std::vector<StmtPtr>& statements) {
        push();
        for (const StmtPtr& s : statements) stmt(*s);
        popKeeping();
    }

    // A block whose statements may not all run (a try's body): what they showed is not kept.
    void blockMaybeCut(const std::vector<StmtPtr>& statements) {
        push();
        for (const StmtPtr& s : statements) stmt(*s);
        pop();
    }

    void stmtInScope(const Stmt& s) {
        if (s.kind == StmtKind::Block) {
            block(static_cast<const BlockStmt&>(s).statements);
        } else {
            push();
            stmt(s);
            popKeeping();
        }
    }

    // Leaves a block whose statements all ran: what they showed of the variables around it
    // (`x = Foo()`, so x is not null) still holds after it.
    void popKeeping() {
        if (narrowed.size() >= 2) {
            auto& outer = narrowed[narrowed.size() - 2];
            const auto& declared = scopes.back();
            for (const auto& [name, t] : narrowed.back())
                if (!declared.count(name.substr(0, name.find('.'))) && name.find('#') == std::string::npos) outer[name] = t;
        }
        pop();
    }

    void stmt(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Expr:
                expr(*static_cast<const ExprStmt&>(s).expr);
                unusedCopy(*static_cast<const ExprStmt&>(s).expr);
                break;
            case StmtKind::Var:
                varStmt(static_cast<const VarStmt&>(s));
                break;
            case StmtKind::Block:
                block(static_cast<const BlockStmt&>(s).statements);
                break;
            case StmtKind::If: {
                const auto& i = static_cast<const IfStmt&>(s);
                condition(*i.condition, "an if");
                std::unordered_map<std::string, Type> yes, no;
                narrowFrom(*i.condition, true, yes);
                narrowFrom(*i.condition, false, no);
                narrowed.push_back(yes);
                scopes.emplace_back();
                stmtInScope(*i.then);
                scopes.pop_back();
                const std::unordered_map<std::string, Type> thenEnd = narrowed.back();
                narrowed.pop_back();
                std::unordered_map<std::string, Type> elseEnd = no;
                if (i.otherwise) {
                    narrowed.push_back(no);
                    scopes.emplace_back();
                    stmtInScope(*i.otherwise);
                    scopes.pop_back();
                    elseEnd = narrowed.back();
                    narrowed.pop_back();
                }
                // `if (x == null) { x = Foo() }`: either way x is a Foo from here on.
                if (!narrowed.empty()) {
                    for (const auto& [name, t] : thenEnd) {
                        auto other = elseEnd.find(name);
                        if (other == elseEnd.end() || findLocal(name) == nullptr) continue;
                        Type both = common(t, other->second);
                        if (both.known()) narrowed.back()[name] = both;
                    }
                }
                // `if (x == null) return`: from here on x is not null.
                if (alwaysLeaves(*i.then) && !narrowed.empty())
                    for (auto& [name, t] : no) narrowed.back()[name] = t;
                if (i.otherwise && alwaysLeaves(*i.otherwise) && !narrowed.empty())
                    for (auto& [name, t] : yes) narrowed.back()[name] = t;
                break;
            }
            case StmtKind::While: {
                const auto& w = static_cast<const WhileStmt&>(s);
                if (w.doWhile) {
                    block(w.body->statements);
                    condition(*w.condition, "a do-while");
                } else {
                    condition(*w.condition, "a while");
                    std::unordered_map<std::string, Type> yes;
                    narrowFrom(*w.condition, true, yes);
                    narrowed.push_back(yes);
                    scopes.emplace_back();
                    block(w.body->statements);
                    scopes.pop_back();
                    narrowed.pop_back();
                }
                break;
            }
            case StmtKind::ForIn: {
                const auto& f = static_cast<const ForInStmt&>(s);
                Type over = expr(*f.iterable);
                if (f.iterable->kind == ExprKind::Range) {
                    // A range with an end counted from the end is a slice's bounds, with no numbers of its own (D146).
                    const auto& r = static_cast<const RangeExpr&>(*f.iterable);
                    auto fromEnd = [](const ExprPtr& e) { return e && e->kind == ExprKind::Unary && static_cast<const UnaryExpr&>(*e).op == TokenType::Caret; };
                    if (fromEnd(r.start) || fromEnd(r.end))
                        report(*f.iterable, "a range with an end counted from the end (^) has no numbers of its own to loop over: loop over items.indices, or make it plain with of(size)");
                }
                Type item = unknownType();
                if (!over.nullable) {
                    if (over.kind == Type::Range) item = Type::of(Type::Int);
                    else if (over.kind == Type::Bytes) item = Type::of(Type::Byte);
                    else if (over.kind == Type::String) item = Type::of(Type::Char);
                    else if (over.kind == Type::List && !over.args.empty()) item = readType(over.args[0]);
                    else if (over.kind == Type::Map) item = mapEntryOf(over);  // a map gives its entries (D64)
                    else if (over.kind == Type::Set && !over.args.empty()) item = readType(over.args[0]);
                    else if (over.kind == Type::Class && over.cls != nullptr) item = iterationItem(over, f);
                } else if (over.known() && over.kind != Type::Null) {
                    report(*f.iterable, "a for cannot go over " + describe(over) + ", which may be null");
                }
                // What the body may change is noted for the loop as well as for the function
                // around (M6.6 part 4): an item is read where it is while the body runs.
                const void* around = effectKeyHere();
                activeLoops.push_back({effectKey(), &f, scopes.size()});
                push();
                if (f.pattern.empty()) {
                    declareLocal(f.variable, item, true);
                    if (over.kind == Type::List && !over.nullable) {
                        scopes.back()[f.variable].loopOf = &f;
                        LoopCandidate& loop = loopCandidates[&f];
                        loop.key = effectKey();
                        loop.item = item;
                    }
                } else {
                    std::vector<Type> parts = partsOf(item, f.pattern.size());
                    for (size_t i = 0; i < f.pattern.size(); i++) declareLocal(f.pattern[i], parts[i], true);
                }
                for (const StmtPtr& st : f.body->statements) stmt(*st);
                pop();
                activeLoops.pop_back();
                if (auto body = effects.find(&f); body != effects.end()) {
                    const Effects inside = body->second;  // copied: effects may grow and move below
                    Effects& outer = effects[around];
                    outer.any = outer.any || inside.any;
                    outer.callsParams = outer.callsParams || inside.callsParams;
                    outer.types.insert(inside.types.begin(), inside.types.end());
                    outer.calls.insert(outer.calls.end(), inside.calls.begin(), inside.calls.end());
                    outer.methodCalls.insert(outer.methodCalls.end(), inside.methodCalls.begin(), inside.methodCalls.end());
                }
                break;
            }
            case StmtKind::Return:
                returnStmt(static_cast<const ReturnStmt&>(s));
                break;
            case StmtKind::Break:
            case StmtKind::Continue:
                break;
            case StmtKind::Throw: {
                // Only a Throwable is thrown (D35).
                const auto& th = static_cast<const ThrowStmt&>(s);
                const Type thrown = expr(*th.value);
                if (const Type root = stdClassType("Throwable"); thrown.known() && root.known() && !fits(thrown, root))
                    report(*th.value, "only a Throwable can be thrown, not " + describe(thrown) +
                                          ": throw IllegalStateException(\"...\") or a class of your own extending Exception (D35)");
                noteThrow(*th.value, thrown);
                break;
            }
            case StmtKind::Try: {
                const auto& t = static_cast<const TryStmt&>(s);
                TryThrows tryThrows(*this, t);
                blockMaybeCut(t.body->statements);
                for (size_t i = 0; i < t.catches.size(); i++) {
                    const CatchClause& c = t.catches[i];
                    push();
                    const Type caught = catchType(c);
                    catchOrder(t, i, caught);
                    tryThrows.clause(i, caught);
                    if (c.hasVariable) tryThrows.declare(c.variable, caught, i);
                    for (const StmtPtr& st : c.body->statements) stmt(*st);
                    pop();
                }
                tryThrows.finally();
                if (t.finalizer) block(t.finalizer->statements);
                break;
            }
            case StmtKind::Import:
                break;
            case StmtKind::Function: {
                const auto& fs = static_cast<const FunctionStmt&>(s);
                if (ctx != nullptr && ctx->topLevel && scopes.size() <= 1) break;  // checked with the file's functions
                // A function declared inside another: a local whose type is a function.
                if (fs.fn->isInline) report(*fs.fn, "a function declared inside another cannot be inline: it is a value of the code around it (D59)");
                FuncInfo* f = newFunc(*fs.fn, unit, ctx ? ctx->cls : nullptr);
                resolveFunc(f);
                effects[effectKeyHere()].calls.push_back(fs.fn.get());  // called through a local, which is not followed
                declareLocal(fs.fn->name, functionValue(f), true);
                bodyInside = true;  // it sees the locals and type parameters around it (G1)
                checkFunctionBody(f, nullptr);
                break;
            }
            case StmtKind::Extend:
                checkExtend(static_cast<const ExtendStmt&>(s));
                break;
            case StmtKind::Class: {
                if (ctx != nullptr && ctx->topLevel && scopes.size() <= 1) break;  // a file's class: already gathered
                const auto& cs = static_cast<const ClassStmt&>(s);
                ClassInfo* k = gatherClass(cs, unit, nullptr);
                k->outerThis = outerThisHere();
                if (localClasses.empty()) push();
                localClasses.back()[cs.name] = k;
                checkClass(k);
                break;
            }
        }
    }

    void varStmt(const VarStmt& v) {
        if (v.isExternal) return;
        // A top-level val or var of the file: already gathered; its type is worked out once.
        const bool global = ctx != nullptr && ctx->topLevel && scopes.size() <= 1 && v.pattern.empty();
        if (v.isConst) {
            if (!global)
                report(v, "const '" + v.name + "' is declared at the top level, in an object or in a static { } block; here, write val " +
                              v.name + " = ...");
            else if (const GlobalSym* g = findGlobal(*unit, v.name); g != nullptr && g->var != nullptr)
                constantOf(g->var);
        }
        Type declared = v.type.present() ? resolve(v.type, *unit) : unknownType();
        if (!v.type.present() && v.init) keptLambda(*v.init);
        // A lazy starting value is worked out by whoever first reads it, which does not wait (D37).
        const NoWait wasWait = ctx != nullptr ? ctx->noWait : CanWait;
        if (v.isLazy && ctx != nullptr) ctx->noWait = LazyValue;
        Type value = v.init ? exprExpected(*v.init, declared) : unknownType();
        if (v.isLazy && ctx != nullptr) ctx->noWait = wasWait;
        if (!v.pattern.empty()) {
            std::vector<Type> parts = partsOf(value, v.pattern.size());
            for (size_t i = 0; i < v.pattern.size(); i++) declareLocal(v.pattern[i], parts[i], v.isVal);
            return;
        }
        if (v.init && declared.known() && !fits(value, declared))
            report(*v.init, "'" + v.name + "' is " + describe(declared) + "; it cannot start as " + describe(value));
        Type type = declared;
        if (!v.type.present()) {
            // A variable takes its type from its first value (D18), so one without either
            // says which.
            if (!v.init && requireTypes)
                report(v, "'" + v.name + "' has no value to take its type from; write one, as in var " + v.name + " : Type");
            if (v.init && value.kind == Type::Null && !v.isConst)
                report(v, "'" + v.name + "' starts as null, so its type must be written (" + v.name + " : Type? = null)");
            type = (value.kind == Type::Null || value.kind == Type::Nothing) ? unknownType() : value;
        }
        if (global) {
            const GlobalSym* g = findGlobal(*unit, v.name);
            if (g != nullptr && g->var != nullptr && g->var->state != 2) {
                g->var->type = type;
                g->var->typeWritten = v.type.present();
                g->var->state = 2;
            }
        }
        declareLocal(v.name, type, v.isVal);
        if (global && v.isConst)
            if (const GlobalSym* g = findGlobal(*unit, v.name); g != nullptr) scopes.back()[v.name].constant = g->var;
        if (v.isVal && !v.isConst && v.init) scopes.back()[v.name].val = &v;
    }

    void returnStmt(const ReturnStmt& r) {
        if (ctx == nullptr) return;
        // A bare return in a lambda copied into an inline function leaves the function around it,
        // whose type it is checked against (D59); in any other lambda the compiler refuses it.
        if (ctx->isLambda && !r.implicit && r.label.empty()) {
            FnCtx* around = ctx;
            while (around != nullptr && around->isLambda) around = around->parent;
            const bool known = around != nullptr && around->returnKnown && !around->topLevel;
            const Type value = r.value ? (known ? exprExpected(*r.value, around->expected) : expr(*r.value)) : Type::of(Type::Unit);
            if (around == nullptr || around->topLevel) return;
            if (around->returns != nullptr) {
                if (r.value) around->returns->push_back(value);
                else around->returnsBare = true;
            }
            if (!known) return;
            const Type& expected = around->expected;
            if (!r.value && expected.known() && expected.kind != Type::Unit && expected.kind != Type::Nothing)
                report(r, "the function this return leaves gives " + describe(expected) + "; return needs a value");
            else if (r.value && expected.kind == Type::Unit && value.known() && value.kind != Type::Unit && value.kind != Type::Nothing)
                report(*r.value, "the function this return leaves gives nothing (unit); it cannot return " + describe(value));
            else if (r.value && expected.kind != Type::Unit && !fits(value, expected))
                report(*r.value, "the function this return leaves gives " + describe(expected) + "; it cannot return " + describe(value));
            return;
        }
        const bool checked = ctx->returnKnown && r.label.empty() && !(ctx->isLambda && !r.implicit);
        Type value = r.value ? (checked ? exprExpected(*r.value, ctx->expected) : expr(*r.value)) : Type::of(Type::Unit);
        // `return cached`: the field is given back at once, and is copied on the way unless the
        // caller keeps it where it is (M6.6 part 5).
        if (r.value && (r.value->kind == ExprKind::Member || r.value->kind == ExprKind::Name)) readAtOnce.insert(r.value.get());
        if (!r.label.empty()) return;  // return@name leaves a lambda
        if (ctx->isLambda && !r.implicit) return;  // a bare return in an inlined lambda leaves the function around it
        if (ctx->returns != nullptr) {
            if (r.value) ctx->returns->push_back(value);
            else ctx->returnsBare = true;
        }
        if (!ctx->returnKnown) return;
        const Type& expected = ctx->expected;
        const char* who = ctx->isLambda ? "this lambda" : "this function";
        if (!r.value) {
            if (expected.known() && expected.kind != Type::Unit && expected.kind != Type::Nothing)
                report(r, std::string(who) + " gives " + describe(expected) + "; return needs a value");
            return;
        }
        if (expected.kind == Type::Unit) {
            if (value.known() && value.kind != Type::Unit && value.kind != Type::Nothing && !r.implicit)
                report(*r.value, std::string(who) + " gives nothing (unit); it cannot return " + describe(value));
            return;
        }
        if (!fits(value, expected))
            report(*r.value, std::string(who) + " gives " + describe(expected) + "; it cannot " +
                                 (r.implicit ? "end with " : "return ") + describe(value));
    }

    // ---- Bodies ----

    void checkFunctionBody(FuncInfo* f, std::vector<Type>* returns) {
        resolveFunc(f);
        if (f->decl == nullptr) return;
        const FunctionDecl& d = *f->decl;
        Unit* savedUnit = unit;
        FnCtx* savedCtx = ctx;
        auto savedScopes = std::move(scopes);
        auto savedNarrowed = std::move(narrowed);
        TParamsSaved savedParams(*this);
        scopes.clear();
        narrowed.clear();
        // A function declared inside another reaches its locals and type parameters, as a
        // lambda does; what a test showed of them is not carried in, as it may be called later.
        if (std::exchange(bodyInside, false)) {
            scopes = savedScopes;
            tparams = savedParams.saved;
        }
        unit = f->unit;
        FnCtx c;
        c.fn = f;
        c.cls = f->owner;
        c.isStatic = f->isStatic;
        c.returns = returns;
        c.returnKnown = f->resultWritten;
        c.expected = f->result;
        if (f->isExtension) {
            c.hasReceiver = true;
            c.receiver = f->receiver;
        }
        c.noWait = noWaitOf(d, f->owner != nullptr);
        if (d.isSuspend && c.noWait != CanWait)
            report(d, "'" + (d.name == "==" ? std::string("==") : d.name) + "' cannot be marked suspend, since " +
                          noWaitReason(c.noWait, d.name));
        ctx = &c;
        pushClassTypeParams(f->owner);
        pushTypeParams(d.typeParams, *f->unit);
        push();
        for (size_t i = 0; i < d.params.size(); i++) {
            if (i < d.paramDefaults.size() && d.paramDefaults[i] != nullptr) {
                Type dv = exprExpected(*d.paramDefaults[i], f->params[i]);
                if (!fits(dv, f->params[i]))
                    report(*d.paramDefaults[i], "'" + d.params[i] + "' is " + describe(f->params[i]) +
                                                    "; its default cannot be " + describe(dv));
            }
            declareLocal(d.params[i], f->params[i], true);
            if (d.isLambda) scopes.back()[d.params[i]].lambdaParam = true;
            scopes.back()[d.params[i]].paramOfDecl = &d;
            noteValueParam(d, static_cast<int>(i), f->params[i]);
            if (f->params[i].kind == Type::Function) {
                Local& l = scopes.back()[d.params[i]];
                l.paramOf = f;
                l.paramIndex = static_cast<int>(i);
            }
            // A lambda's or an anonymous function's parameters, and a setter's value, take
            // their types from where they are used.
            const bool named = !d.isLambda && !d.name.empty() && d.name[0] != '<' && d.accessor != FunctionDecl::Accessor::Set;
            const bool written = static_cast<int>(i) == d.varargIndex ? d.varargType.present()
                                                                  : i < d.paramTypes.size() && d.paramTypes[i].present();
            if (requireTypes && named && !d.generated && d.params[i] != "_" && !written)
                report(i < d.paramPositions.size() ? d.paramPositions[i] : static_cast<const Node&>(d),
                       "'" + d.params[i] + "' has no type; write one, as in (" + d.params[i] + " : Type)");
        }
        for (const StmtPtr& s : d.body) stmt(*s);
        pop();
        unit = savedUnit;
        ctx = savedCtx;
        scopes = std::move(savedScopes);
        narrowed = std::move(savedNarrowed);
    }

    // Lambdas run on an object their type says (D58).
    std::unordered_set<const FunctionDecl*> onObject;

    // `it` (D55): the lambdas without an arrow that have none, and for each lambda being checked
    // that has one, whether it uses it and the uses of inner lambdas' own.
    std::unordered_set<const FunctionDecl*> noItLambdas;
    struct ItUser {
        const FunctionDecl* decl;
        bool used;
        std::vector<const Node*> innerUses;
    };
    std::vector<ItUser*> itUsers;
    std::unordered_set<const Node*> itReported;
    static const std::string& noItName() {
        static const std::string name = "(no it)";
        return name;
    }
    void noteItUse(const Local& l, const NameExpr& n) {
        if (!l.lambdaParam || l.paramOfDecl == nullptr) return;
        size_t at = itUsers.size();
        while (at > 0 && itUsers[at - 1]->decl != l.paramOfDecl) at--;
        if (at == 0) return;
        itUsers[at - 1]->used = true;
        for (size_t i = 0; i + 1 < at; i++) itUsers[i]->innerUses.push_back(&n);
    }

    // A class of the standard library's, by name, as a type: Throwable and the rest (D35).
    Type stdClassType(const std::string& name) {
        for (const auto& u : units) {
            if (!u->prelude) continue;
            if (const GlobalSym* g = findGlobal(*u, name); g != nullptr && g->cls != nullptr) return classType(g->cls);
        }
        return unknownType();
    }

    // A clause whose class an earlier clause of the same try already takes can never run.
    void catchOrder(const TryStmt& t, size_t index, const Type& caught) {
        if (silent != 0 || caught.kind != Type::Class) return;
        for (size_t i = 0; i < index; i++) {
            const Type before = catchType(t.catches[i]);
            if (before.kind == Type::Class && fits(caught, before)) {
                report(t.catches[index].type.present() ? static_cast<const Node&>(t.catches[index].type) : static_cast<const Node&>(*t.catches[index].body),
                       "the catch before this one takes every " + (before.cls != nullptr ? before.cls->name : describe(before)) + ", so this one can never run");
                return;
            }
        }
    }

    // What a catch clause catches: the class it names, a Throwable; with none, every Exception (D151).
    std::unordered_set<const CatchClause*> catchReported;
    Type catchType(const CatchClause& c) {
        if (!c.type.present()) return stdClassType("Exception");
        const Type named = resolve(c.type, *unit);
        const Type root = stdClassType("Throwable");
        if (named.known() && root.known() && !fits(named, root) && catchReported.insert(&c).second)
            report(c.type, "a catch names a class of Throwable, as in catch (e : IOException), not " + describe(named) + " (D35)");
        return named;
    }

    // A lambda kept in a variable or field with no type written runs on no object: its bare names
    // are looked for as anywhere else, where one passed where the checker cannot tell may run on
    // an object.
    std::unordered_set<const FunctionDecl*> keptLambdas;
    void keptLambda(const Expr& e) {
        if (e.kind == ExprKind::Function) keptLambdas.insert(static_cast<const FunctionExpr&>(e).fn.get());
    }

    // A lambda or `fun (x) { ... }` used as a value, with the function type expected where it
    // stands, if one is: its parameters and the object it runs on are taken from it.
    Type lambda(const FunctionDecl& d, const Type* expected) {
        const bool typed = expected != nullptr && expected->kind == Type::Function && expected->knownParams;
        FnCtx c;
        c.parent = ctx;
        c.cls = ctx ? ctx->cls : nullptr;
        c.isStatic = ctx ? ctx->isStatic : false;
        c.isLambda = d.isLambda;
        c.placeDecl = &d;
        c.suspendLambda = typed && expected->suspend;
        if (silent == 0) c.throwScope = newThrowRoot(&d, ThrowRoot::Lambda, d);
        std::vector<Type> returns;
        c.returns = &returns;
        if (!d.isLambda && d.returnType.present()) {
            c.returnKnown = true;
            c.expected = resolve(d.returnType, *unit);
        } else if (d.isLambda && typed && expected->result && expected->result->known() &&
                   expected->result->kind != Type::TypeParam && expected->result->kind != Type::Unit) {
            c.returnKnown = true;
            c.expected = *expected->result;
        }
        if (d.isLambda) {
            if (typed && expected->receiver) {
                c.hasReceiver = true;
                c.receiver = *expected->receiver;
                if (c.receiver.unknown() || c.receiver.kind == Type::TypeParam) c.receiverUnknown = true;
            } else if (!typed) {
                c.receiverUnknown = keptLambdas.count(&d) == 0;
            }
        }
        effects[effectKeyHere()].calls.push_back(&d);  // it may run while the code around it does
        FnCtx* saved = ctx;
        ctx = &c;
        push();
        Type ft = Type::of(Type::Function);
        const bool itLambda = d.isLambda && d.params.size() == 1 && d.params[0] == "it" && !d.paramTypes[0].present() &&
                              d.paramDefaults.size() == 1 && d.paramDefaults[0] != nullptr;
        ft.knownParams = !itLambda || typed;
        // Given a type that takes nothing, it has no `it` of its own (D55).
        const bool noIt = itLambda && typed && expected->args.empty();
        if (silent == 0 && itLambda) {
            if (noIt) noItLambdas.insert(&d);
            else noItLambdas.erase(&d);
        }
        ItUser itUser{&d, false, {}};
        if (itLambda && !noIt) itUsers.push_back(&itUser);
        for (size_t i = 0; i < d.params.size(); i++) {
            Type p = i < d.paramTypes.size() ? resolve(d.paramTypes[i], *unit) : unknownType();
            if (p.unknown() && typed && i < expected->args.size()) p = expected->args[i];
            ft.args.push_back(p);
            const std::string& paramName = noIt ? noItName() : d.params[i];
            declareLocal(paramName, p, true);
            Local& declared = scopes.back()[paramName];
            declared.lambdaParam = true;
            declared.paramOfDecl = &d;
            noteValueParam(d, static_cast<int>(i), p);
        }
        if (itLambda && typed && expected->args.empty()) ft.args.clear();
        if (!itLambda) {
            ft.required = 0;
            for (size_t i = 0; i < d.params.size(); i++)
                if (i >= d.paramDefaults.size() || d.paramDefaults[i] == nullptr) ft.required = static_cast<int>(i) + 1;
        }
        for (size_t i = 0; i < d.paramPatterns.size(); i++) {
            const auto& pattern = d.paramPatterns[i];
            std::vector<Type> parts = partsOf(i < ft.args.size() ? ft.args[i] : unknownType(), pattern.size());
            for (size_t j = 0; j < pattern.size(); j++) declareLocal(pattern[j], parts[j], true);
        }
        if (c.hasReceiver) ft.receiver = std::make_shared<Type>(c.receiver);
        ft.suspend = c.suspendLambda;
        if (silent == 0 && d.isLambda) {
            if (c.hasReceiver && !c.receiverUnknown) onObject.insert(&d);
            else onObject.erase(&d);
        }
        if (silent == 0) {
            LambdaThrows& kept = lambdaThrows[&d];
            kept.unit = unit;
            kept.as = typed ? *expected : Type::of(Type::Function);
        }
        for (const StmtPtr& s : d.body) stmt(*s);
        pop();
        ctx = saved;
        if (itLambda && !noIt) {
            itUsers.pop_back();
            // An `it` of a lambda inside this one, which also uses its own: one hides the other (D55).
            if (itUser.used && silent == 0)
                for (const Node* inner : itUser.innerUses)
                    if (itReported.insert(inner).second)
                        report(*inner, "this 'it' is the inner lambda's, and the lambda around it uses an 'it' of its own: "
                                       "name one of them, as in rows.forEach { row -> row.cells.forEach { print(it) } } (D55)");
        }
        for (const Type& given : returns)
            if (given.kind == Type::Class && !given.nullable && given.cls != nullptr && given.cls->isValue) valueLambdas.insert(&d);
        Type r = (!d.isLambda && c.returnKnown) ? c.expected : unknownType();
        if (d.isLambda && c.returnKnown) {
            r = c.expected;  // what it ends with was checked against this, and reported there
        } else if (d.isLambda && !returns.empty()) {
            r = returns[0];
            for (size_t i = 1; i < returns.size(); i++) r = common(r, returns[i]);
        } else if (d.isLambda) {
            r = Type::of(Type::Unit);
        }
        ft.result = std::make_shared<Type>(r);
        return ft;
    }

    // A value type keeps the values it holds in place, so it cannot hold itself, through others
    // or not, except where the field may be null, which keeps it in a box (M6.6 part 2).
    bool holdsValue(ClassInfo* holder, const ClassInfo* held, std::vector<ClassInfo*>& visited) {
        for (const auto& [name, field] : holder->fields) {
            // A property worked out by its getter keeps nothing.
            if (field->decl != nullptr && field->decl->getter && !field->decl->init) continue;
            const Type t = fieldType(field);
            if (t.kind != Type::Class || t.nullable || t.cls == nullptr || !t.cls->isValue) continue;
            if (t.cls == held) return true;
            if (std::find(visited.begin(), visited.end(), t.cls) != visited.end()) continue;
            visited.push_back(t.cls);
            if (holdsValue(t.cls, held, visited)) return true;
        }
        return false;
    }

    // Kinds of an enum class named bare (D39), for the compiler: the node, the full name.
    std::unordered_map<const Node*, std::string> kindNames;
    std::unordered_map<const Node*, ClassInfo*> kindClasses;

    // The kind of the enum class `e` (or of one of its kinds that is one) called `name`.
    ClassInfo* kindNamed(ClassInfo* e, const std::string& name, int depth = 0) {
        if (depth > 8) return nullptr;
        for (ClassInfo* k : kindsOf(e)) {
            if (k->decl->name == name) return k;
            if (k->isEnumClass)
                if (ClassInfo* inner = kindNamed(k, name, depth + 1)) return inner;
        }
        return nullptr;
    }

    // `is Click` where the value is an Event: the type, with Click found among Event's kinds when
    // the name means no class of its own.
    Type resolveAgainst(const TypeAnnotation& a, const Type& against) {
        const Type plain = withNull(against, false);
        if (plain.kind == Type::Class && plain.cls != nullptr && plain.cls->isEnumClass && !a.typeParam && !a.isFunction() &&
            a.args.empty() && a.name.find('.') == std::string::npos && unit != nullptr &&
            findClass(*unit, a.name, classAround()) == nullptr) {
            if (ClassInfo* kind = kindNamed(plain.cls, a.name)) {
                kindNames[&a] = kind->name;
                kindClasses[&a] = kind;
            }
        }
        return resolve(a, *unit);
    }

    // `Click(1, 2)` or `Close` where an Event is expected: the kind's object or a new one, when
    // the name means nothing else here. Null when it is not that.
    std::unique_ptr<Type> bareKind(const Expr& e, ClassInfo* enumClass) {
        const NameExpr* n = nullptr;
        const CallExpr* call = nullptr;
        if (e.kind == ExprKind::Name) {
            n = static_cast<const NameExpr*>(&e);
        } else if (e.kind == ExprKind::Call && static_cast<const CallExpr&>(e).callee->kind == ExprKind::Name) {
            call = static_cast<const CallExpr*>(&e);
            n = static_cast<const NameExpr*>(call->callee.get());
        }
        if (n == nullptr || unit == nullptr || !meansGlobal(*n) || findGlobal(*unit, n->name) != nullptr ||
            findClass(*unit, n->name, classAround()) != nullptr)
            return nullptr;
        ClassInfo* kind = kindNamed(enumClass, n->name);
        if (kind == nullptr) return nullptr;
        kindNames[n] = kind->name;
        if (call == nullptr) {
            if (!kind->isObject) return nullptr;  // a class's name is not a value
            return std::make_unique<Type>(classType(kind));
        }
        Args args{call->args, call->names, call->trailingLambda, call->hasSpread};
        return std::make_unique<Type>(constructorCall(kind, args, call->typeArgs, *call));
    }

    // The kinds of an enum class or enum interface (D39): the classes and objects that extend it
    // directly, in the order they are written.
    std::vector<ClassInfo*> kindsOf(ClassInfo* e) {
        std::vector<ClassInfo*> kinds;
        for (const auto& [stmt, k] : classOf) {
            resolveSupers(k);
            bool extends = k->superclass == e;
            for (ClassInfo* i : k->interfaces) extends = extends || i == e;
            if (extends) kinds.push_back(k);
        }
        std::sort(kinds.begin(), kinds.end(), [](const ClassInfo* a, const ClassInfo* b) {
            return a->decl->line != b->decl->line ? a->decl->line < b->decl->line : a->decl->col < b->decl->col;
        });
        return kinds;
    }

    // The fields of a data class's header, in order (D65).
    static std::vector<const HeaderParam*> headerFields(const ClassStmt& c) {
        std::vector<const HeaderParam*> fields;
        for (const HeaderParam& p : c.header)
            if (p.isField) fields.push_back(&p);
        return fields;
    }

    size_t inheritedDataFields(ClassInfo* k) {
        resolveSupers(k);
        if (!k->isData || k->superclass == nullptr || !k->superclass->isData || k->superclass->decl == nullptr) return 0;
        return headerFields(*k->superclass->decl).size();
    }

    // A data class is extended only by a data class, whose header starts with its fields and
    // passes them up as they are (D65).
    void checkDataExtension(ClassInfo* k) {
        const ClassStmt& c = *k->decl;
        resolveSupers(k);
        ClassInfo* parent = k->superclass;
        if (parent == nullptr || !parent->isData || parent->decl == nullptr || parent->isValue) return;
        if (!k->isData) {
            report(c, k->name + " cannot extend data " + parent->name + ": a data class is extended only by a data class, whose " +
                          "header starts with " + parent->name + "'s fields (D65)");
            return;
        }
        const std::vector<const HeaderParam*> above = headerFields(*parent->decl);
        std::string wanted;
        for (const HeaderParam* p : above) wanted += (wanted.empty() ? "" : ", ") + p->name;
        const std::vector<const HeaderParam*> own = headerFields(c);
        bool starts = own.size() >= above.size();
        for (size_t i = 0; starts && i < above.size(); i++) {
            starts = own[i]->name == above[i]->name;
            if (starts && own[i]->type.present() && above[i]->type.present())
                starts = text(resolve(own[i]->type, *k->unit)) == text(resolve(above[i]->type, *parent->unit));
        }
        if (!starts) {
            report(c, "data " + k->name + " extends data " + parent->name + ", so its header starts with " + parent->name +
                          "'s fields, of the same types: data " + k->decl->name + "(" + wanted + ", ...) (D65)");
            return;
        }
        bool passed = c.superArgs.size() == above.size();
        for (size_t i = 0; passed && i < above.size(); i++)
            passed = c.superArgs[i]->kind == ExprKind::Name && static_cast<const NameExpr&>(*c.superArgs[i]).name == above[i]->name;
        if (!passed)
            report(c, "data " + k->name + " passes its first fields up to " + parent->name + " as they are: : " + parent->decl->name +
                          "(" + wanted + ") (D65)");
    }

    void checkClass(ClassInfo* k) {
        const ClassStmt& c = *k->decl;
        checkDataExtension(k);
        // The kinds of an enum class are all declared in its file (D39).
        resolveSupers(k);
        {
            std::vector<ClassInfo*> above = k->interfaces;
            if (k->superclass != nullptr) above.push_back(k->superclass);
            for (ClassInfo* e : above)
                if (e != nullptr && e->isEnumClass && e->unit != k->unit)
                    report(c, k->name + " cannot extend " + e->name + ", an enum " + (e->isInterface ? "interface" : "class") +
                                  ": its kinds are all declared in its file, so a when can know them all (D39)");
        }
        // == and hashCode go together, so that equal objects hash alike (D26).
        const FunctionDecl* ownEquals = nullptr;
        const FunctionDecl* ownHash = nullptr;
        for (const auto& m : c.methods) {
            if (m->isStatic) continue;
            if (m->name == "==") ownEquals = m.get();
            if (m->name == "hashCode" && m->params.empty()) ownHash = m.get();
        }
        if (ownEquals != nullptr && ownHash == nullptr)
            report(*ownEquals, k->name + " has its own == and no hashCode: a map or a set finds an object by its hash, so equal objects " +
                                   "must hash alike; add fun hashCode() : int => hash(...) of what == compares (D26)");
        if (ownHash != nullptr && ownEquals == nullptr)
            report(*ownHash, k->name + " has its own hashCode and no ==: the two go together, so that equal objects hash alike; add " +
                                 "op ==(other : any?) : bool comparing what hashCode reads (D26)");
        for (const auto& m : c.methods)
            if (m->isMutating && (!k->isValue || m->isStatic))
                report(*m, "'" + m->name + "' is marked mutating, which only a method of a value type can be: it changes the value " +
                               "it is called on, where an object of a class changes its fields itself");
        if (k->isValue) {
            std::vector<ClassInfo*> visited{k};
            if (holdsValue(k, k, visited))
                report(c, "value " + k->name + " holds itself, and would have no end: make the field that holds it nullable, " +
                              k->name + "?, which keeps it in a box");
        }
        // A method written override takes the place of a form above with its parameters.
        for (const auto& m : c.methods) {
            if (!m->isOverride || m->isStatic || m->accessor != FunctionDecl::Accessor::None || m->name == "init") continue;
            auto known = funcOf.find(m.get());
            if (known == funcOf.end()) continue;
            FuncInfo* f = known->second;
            const std::vector<FuncInfo*> above = formsAbove(k, m->name);
            if (above.empty()) continue;
            FuncInfo* same = nullptr;
            for (FuncInfo* g : above)
                if (same == nullptr && sameForm(f, g)) same = g;
            if (same != nullptr) {
                const bool waits = same->decl != nullptr && same->decl->isSuspend;
                const std::string form = formText(same->owner->name + "." + m->name, same, {});
                if (m->isSuspend && !waits)
                    report(*m, "'" + m->name + "' is marked suspend, but what it overrides (" + form +
                                   ") is not: code that calls it through " + same->owner->name +
                                   " would not know it may wait; take suspend off, or mark the one above suspend too");
                else if (!m->isSuspend && waits)
                    report(*m, "'" + m->name + "' overrides a suspend function (" + form + "), so it is one too: write 'override suspend fun " +
                                   m->name + "'");
                continue;
            }
            std::string forms;
            for (size_t i = 0; i < above.size(); i++)
                forms += (i == 0 ? "" : i + 1 == above.size() ? " and " : ", ") + formText(above[i]->owner->name + "." + m->name, above[i], {});
            report(*m, "'" + m->name + "' is written override, but no form above has its parameters (" + forms +
                           "); give it those, or make it a form of its own without override");
        }
        if (requireTypes)
            for (const HeaderParam& p : c.header)
                if (!p.type.present())
                    report(p, "'" + p.name + "' in the header of " + k->name + " has no type; write one, as in (" + p.name + " : Type)");
        resolveSupers(k);
        TParamsSaved savedParams(*this);
        pushClassTypeParams(k);
        for (const auto& field : c.fields) {
            FieldInfo* f = k->fields[field->name];
            if (field->isConst) constantOf(f);
            Type declared = fieldType(f);
            if (field->isLateinit && declared.kind == Type::Class && declared.cls != nullptr && declared.cls->isValue)
                report(*field, "lateinit var '" + field->name + "' is " + describe(declared) + ", a value type, which is kept whole " +
                                   "where it is: give it a starting value; lateinit is for a field that holds an object (D36)");
            if (requireTypes && !field->type.present() && !field->init && field->getter == nullptr && f->header == nullptr)
                report(*field, "'" + field->name + "' has no value to take its type from; write one, as in " +
                                   (field->isVal ? "val " : "var ") + field->name + " : Type");
            if (field->init) {
                FnCtx fc;
                fc.cls = k;
                fc.noWait = ObjectMade;
                withCtx(fc, [&] {
                    if (!field->type.present()) keptLambda(*field->init);
                    Type value = exprExpected(*field->init, declared);
                    if (field->type.present() && !fits(value, declared))
                        report(*field->init, "'" + field->name + "' is " + describe(declared) + "; it cannot start as " +
                                                 describe(value));
                    if (!field->type.present() && !field->isConst && value.kind == Type::Null && (f->header == nullptr || !f->header->type.present()))
                        report(*field, "'" + field->name + "' starts as null, so its type must be written (" + field->name +
                                           " : Type? = null)");
                });
            }
            for (const auto* acc : {field->getter.get(), field->setter.get()}) {
                if (acc == nullptr) continue;
                FuncInfo* fn = newFunc(*acc, k->unit, k);
                if (acc == field->getter.get()) f->getterFn = fn;
                if (acc == field->getter.get() && !acc->returnType.present()) {
                    fn->resolved = true;
                    fn->resultWritten = declared.known();
                    fn->result = declared;
                    fn->resultState = 2;
                }
                accessorBody(fn, declared);
            }
        }
        for (const auto& field : c.staticFields) {
            FieldInfo* f = k->statics[field->name];
            if (field->isConst) constantOf(f);
            Type declared = fieldType(f);
            if (field->init) {
                FnCtx fc;
                fc.cls = k;
                fc.isStatic = true;
                fc.noWait = StaticValue;
                withCtx(fc, [&] {
                    Type value = exprExpected(*field->init, declared);
                    if (field->type.present() && !fits(value, declared))
                        report(*field->init, "'" + field->name + "' is " + describe(declared) + "; it cannot start as " +
                                                 describe(value));
                });
            }
        }
        for (const HeaderParam& p : c.header) {
            if (p.defaultValue == nullptr) continue;
            FnCtx fc;
            fc.cls = k;
            fc.noWait = ObjectMade;
            withCtx(fc, [&] {
                Type declared = resolve(p.type, *k->unit);
                Type value = exprExpected(*p.defaultValue, declared);
                if (!fits(value, declared))
                    report(*p.defaultValue, "'" + p.name + "' is " + describe(declared) + "; its default cannot be " +
                                                describe(value));
            });
        }
        for (const auto& m : c.methods) {
            auto known = funcOf.find(m.get());
            if (known == funcOf.end() || m->isAbstract) continue;
            checkFunctionBody(known->second, nullptr);
        }
        for (const auto& [count, body] : c.initBlocks) {
            FnCtx fc;
            fc.cls = k;
            fc.noWait = ObjectMade;
            withCtx(fc, [&] { blockMaybeCut(body->statements); });
        }
        if (c.hasSuperArgs && k->superclass != nullptr) {
            FnCtx fc;
            fc.cls = k;
            fc.noWait = ObjectMade;
            withCtx(fc, [&] {
                // The header's parameters are in reach of the superclass's arguments.
                for (const HeaderParam& h : c.header) declareLocal(h.name, resolve(h.type, *k->unit), true);
                Args args{c.superArgs, noNames(), false, false};
                superCall = true;
                Type made = constructorCall(k->superclass, args, {}, c);
                (void)made;
            });
        }
        for (const EnumEntry& e : c.entries) {
            if (e.args.empty() && !k->headerInit && k->inits.empty()) continue;
            FnCtx fc;
            fc.cls = k;
            fc.isStatic = true;
            fc.noWait = StaticValue;
            withCtx(fc, [&] {
                Args args{e.args, noNames(), false, false};
                constructorCall(k, args, {}, e);
            });
        }
        for (const auto& inner : c.nested) checkClass(classOf[inner.get()]);
    }

    // A property's accessor: `field` is the stored value.
    void accessorBody(FuncInfo* fn, const Type& stored) {
        resolveFunc(fn);
        const FunctionDecl& d = *fn->decl;
        FnCtx c;
        c.fn = fn;
        c.cls = fn->owner;
        c.returnKnown = fn->resultWritten;
        c.expected = fn->result;
        c.noWait = d.accessor == FunctionDecl::Accessor::Set ? Setter : Getter;
        if (d.isSuspend) report(d, "'" + d.name + "' cannot be marked suspend, since " + noWaitReason(c.noWait, d.name));
        FnCtx* saved = ctx;
        Unit* savedU = unit;
        auto savedScopes = std::move(scopes);
        auto savedNarrowed = std::move(narrowed);
        scopes.clear();
        narrowed.clear();
        unit = fn->unit;
        ctx = &c;
        push();
        declareLocal("field", stored, false);
        for (size_t i = 0; i < d.params.size(); i++) declareLocal(d.params[i], i == 0 ? stored : unknownType(), true);
        for (const StmtPtr& s : d.body) stmt(*s);
        pop();
        ctx = saved;
        unit = savedU;
        scopes = std::move(savedScopes);
        narrowed = std::move(savedNarrowed);
    }

    template <typename F>
    void withCtx(FnCtx& c, F&& body) {
        FnCtx* saved = ctx;
        auto savedScopes = std::move(scopes);
        auto savedNarrowed = std::move(narrowed);
        scopes.clear();
        narrowed.clear();
        ctx = &c;
        push();
        body();
        pop();
        ctx = saved;
        scopes = std::move(savedScopes);
        narrowed = std::move(savedNarrowed);
    }

    // ---- Expressions ----

    // An expression where a type is expected: a lambda takes its parameters from it, and a
    // list or map literal its item types.
    // The types list and map literals are made as, for the compiler (M6.6 part 1).
    std::unordered_map<const Expr*, Type> literalTypes;

    Type exprExpected(const Expr& e, const Type& expected) {
        // A kind of an enum class by its bare name, where one is expected (D39).
        if (const Type plain = withNull(expected, false); plain.kind == Type::Class && plain.cls != nullptr && plain.cls->isEnumClass)
            if (std::unique_ptr<Type> kind = bareKind(e, plain.cls)) return *kind;
        // A lambda where an interface with one abstract method goes: an object of it (D28).
        if (e.kind == ExprKind::Function)
            if (const Type plain = withNull(expected, false); plain.kind == Type::Class && plain.cls != nullptr && plain.cls->isInterface) {
                std::string method;
                Type fn;
                if (samType(plain, method, fn)) {
                    lambda(*static_cast<const FunctionExpr&>(e).fn, &fn);
                    if (silent == 0) samConversions[&e] = Sam{plain, method, static_cast<int>(fn.args.size())};
                    return plain;
                }
            }
        if (e.kind == ExprKind::List || e.kind == ExprKind::Map) {
            const Type made = exprExpectedLiteral(e, expected);
            literalTypes[&e] = made;
            return made;
        }
        return exprExpectedLiteral(e, expected);
    }

    // D28: an interface with exactly one abstract method, whatever default methods it has
    // besides, takes a lambda, as in Java. Its method's name and type, as a function type.
    struct Sam {
        Type iface;
        std::string method;
        int arity = 0;
    };
    std::unordered_map<const Expr*, Sam> samConversions;
    std::unordered_set<const Node*> samCalls;

    bool samType(const Type& iface, std::string& method, Type& fn) {
        ClassInfo* k = iface.cls;
        if (k == nullptr || !k->isInterface || k->isEnumClass || hasUnknownSuper(k)) return false;
        std::map<std::string, int> abstractForms;
        std::unordered_set<std::string> given;  // a default somewhere, which fills the name
        bool property = false;
        std::unordered_set<ClassInfo*> visited;
        std::function<void(ClassInfo*)> walk = [&](ClassInfo* i) {
            if (i == nullptr || !visited.insert(i).second) return;
            resolveSupers(i);
            for (const auto& [name, field] : i->fields)
                if (field->decl == nullptr || !(field->decl->getter || field->decl->init)) property = true;
            for (const auto& [name, forms] : i->methods)
                for (FuncInfo* f : forms) {
                    if (f->decl == nullptr) continue;
                    if (f->decl->isAbstract) abstractForms[name]++;
                    else given.insert(name);
                }
            for (ClassInfo* above : i->interfaces) walk(above);
        };
        walk(k);
        if (property) return false;
        int count = 0;
        for (const auto& [name, forms] : abstractForms) {
            if (given.count(name) != 0) continue;
            count += forms;
            method = name;
        }
        if (count != 1) return false;
        Member mem = lookupMember(iface, method, true);
        if (mem.methods.empty()) return false;
        FuncInfo* f = mem.methods[0];
        resolveFunc(f);
        const Env& env = mem.methodEnvs.empty() ? mem.env : mem.methodEnvs[0];
        fn = Type::of(Type::Function);
        fn.knownParams = true;
        for (const Type& p : f->params) fn.args.push_back(subst(p, env));
        fn.required = static_cast<int>(f->params.size());
        fn.result = std::make_shared<Type>(subst(resultOf(f), env));
        fn.throws = writtenThrowsTypes(f);  // what the interface's method writes (D53)
        return true;
    }

    // For a call of a C++ function with named arguments or a lambda after it: which argument
    // goes to each parameter in turn, -1 for one not given.
    std::unordered_map<const Node*, std::vector<int>> argumentOrders;

    // Int constants kept as bytes where a byte goes (D19): `val b : byte = 200`.
    std::unordered_map<const Expr*, uint8_t> byteConstants;

    // Whether `e` is made of int literals alone, as `200` and `0x80 | 1` are: a literal that
    // fits is a byte where a byte goes, and a name, even a constant's, is the int it is (D19).
    static bool literalsOnly(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Literal: return static_cast<const LiteralExpr&>(e).type == LiteralExpr::Type::Int;
            case ExprKind::Unary: return literalsOnly(*static_cast<const UnaryExpr&>(e).operand);
            case ExprKind::Binary: {
                const auto& b = static_cast<const BinaryExpr&>(e);
                return literalsOnly(*b.left) && literalsOnly(*b.right);
            }
            default: return false;
        }
    }

    // What a list or map literal makes, given the type expected where it goes: never frozen,
    // which only frozen() makes (D26), and changeable by whoever made it (D32).
    static Type made(const Type& expected) {
        Type t = withNull(expected, false);
        t.frozen = false;
        t.open = true;
        return t;
    }

    Type exprExpectedLiteral(const Expr& e, const Type& expected) {
        if (expected.kind == Type::Byte && e.kind != ExprKind::List && e.kind != ExprKind::Map && e.kind != ExprKind::Function) {
            // A constant int that fits needs no conversion; one that does not is caught here.
            Type t = expr(e);
            if (t.kind != Type::Int || t.nullable || !literalsOnly(e)) return t;
            const ConstValue known = foldQuietly(e);
            if (known.kind != ConstValue::Int) return t;
            if (known.i < 0 || known.i > 255) {
                report(e, std::to_string(known.i) + " does not fit in a byte, which holds 0 to 255");
                return unknownType();
            }
            byteConstants[&e] = static_cast<uint8_t>(known.i);
            return Type::of(Type::Byte);
        }
        if (expected.known()) {
            if (e.kind == ExprKind::Function) {
                Type plain = withNull(expected, false);
                return lambda(*static_cast<const FunctionExpr&>(e).fn, &plain);
            }
            // A list or map literal takes its item types from where it goes, unless what is
            // expected still names a type parameter being worked out from it.
            if (e.kind == ExprKind::List && expected.kind == Type::List && !expected.args.empty() && expected.args[0].known() &&
                !mentionsTypeParam(expected)) {
                const auto& l = static_cast<const ListExpr&>(e);
                bool all = true;
                Type item;
                for (size_t i = 0; i < l.items.size(); i++) {
                    Type t = exprExpected(*l.items[i], expected.args[0]);
                    if (!fits(t, expected.args[0])) all = false;
                    item = i == 0 ? t : common(item, t);
                }
                if (all) return made(expected);
                return openListOf(item.kind == Type::Null ? unknownType() : item);
            }
            if (e.kind == ExprKind::Map && expected.kind == Type::Map && expected.args.size() == 2 && !mentionsTypeParam(expected)) {
                const auto& m = static_cast<const MapExpr&>(e);
                bool all = true;
                Type keys, values;
                for (size_t i = 0; i < m.entries.size(); i++) {
                    Type k = exprExpected(*m.entries[i].first, expected.args[0]);
                    Type v = exprExpected(*m.entries[i].second, expected.args[1]);
                    noteKey(*m.entries[i].first, k);
                    if (!fits(k, expected.args[0]) || !fits(v, expected.args[1])) all = false;
                    keys = i == 0 ? k : common(keys, k);
                    values = i == 0 ? v : common(values, v);
                }
                if (all) return made(expected);
                Type t = Type::of(Type::Map);
                t.args = {keys, values};
                t.open = true;
                return t;
            }
        }
        return expr(e);
    }

    Type expr(const Expr& e) { return readType(exprWithStorage(e)); }

    Type exprWithStorage(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Literal: {
                const auto& l = static_cast<const LiteralExpr&>(e);
                switch (l.type) {
                    case LiteralExpr::Type::Null: return Type::of(Type::Null);
                    case LiteralExpr::Type::Bool: return Type::of(Type::Bool);
                    case LiteralExpr::Type::Int: return Type::of(Type::Int);
                    case LiteralExpr::Type::Float: return Type::of(Type::Float);
                    case LiteralExpr::Type::String: return Type::of(Type::String);
                    case LiteralExpr::Type::Char: return Type::of(Type::Char);
                }
                return unknownType();
            }
            case ExprKind::Interp:
                for (const ExprPtr& p : static_cast<const InterpExpr&>(e).parts) expr(*p);
                return Type::of(Type::String);
            case ExprKind::Name:
                return name(static_cast<const NameExpr&>(e));
            case ExprKind::This:
                if (!static_cast<const ThisExpr&>(e).label.empty()) return labelledThis(static_cast<const ThisExpr&>(e));
                return thisType();
            case ExprKind::Super:
                return unknownType();
            case ExprKind::Unary:
                return unary(static_cast<const UnaryExpr&>(e));
            case ExprKind::Binary:
                return binary(static_cast<const BinaryExpr&>(e));
            case ExprKind::Logical: {
                const auto& l = static_cast<const LogicalExpr&>(e);
                const char* word = l.isAnd ? "and" : "or";
                Type a = expr(*l.left);
                std::unordered_map<std::string, Type> narrow;
                narrowFrom(*l.left, l.isAnd, narrow);
                narrowed.push_back(narrow);
                Type b = expr(*l.right);
                narrowed.pop_back();
                const std::pair<const Type*, const Expr*> sides[] = {{&a, l.left.get()}, {&b, l.right.get()}};
                for (const auto& [t, at] : sides)
                    if (t->known() && !(t->kind == Type::Bool && !t->nullable) && t->kind != Type::Nothing && t->kind != Type::TypeParam)
                        report(*at, std::string("'") + word + "' takes bool values; this is " + describe(*t));
                return Type::of(Type::Bool);
            }
            case ExprKind::Ternary: {
                const auto& t = static_cast<const TernaryExpr&>(e);
                condition(*t.condition, "'? :'");
                std::unordered_map<std::string, Type> yes, no;
                narrowFrom(*t.condition, true, yes);
                narrowFrom(*t.condition, false, no);
                narrowed.push_back(yes);
                Type a = expr(*t.then);
                narrowed.pop_back();
                narrowed.push_back(no);
                Type b = expr(*t.otherwise);
                narrowed.pop_back();
                return common(a, b);
            }
            case ExprKind::Assign:
                return assign(static_cast<const AssignExpr&>(e));
            case ExprKind::IncDec: {
                const auto& i = static_cast<const IncDecExpr&>(e);
                checkValueFieldWrite(*i.target);
                Type t = expr(*i.target);
                // a?.count++ does nothing when a is null, so the null that ?. adds is not the
                // field's own
                if (t.nullable && safeChain(*i.target)) t = withNull(t, false);
                const std::string op = i.increment ? "++" : "--";
                if (nullablePlace(*i.target, t, op, i.nullSafe, e)) return Type::of(Type::Unit);
                t = withNull(t, false);
                if (t.kind == Type::Class && !t.nullable)
                    operatorResult(t, i.increment ? "+" : "-", e, {Type::of(Type::Int)});
                else if (t.known() && t.kind != Type::TypeParam && !isNumeric(t))
                    report(e, std::string(i.increment ? "++" : "--") + " needs a number; this is " + describe(t));
                return Type::of(Type::Unit);
            }
            case ExprKind::Call: {
                const Type made = call(static_cast<const CallExpr&>(e));
                // A value given back by a function may be one kept in a list or an object, read
                // where it is (M6.6 part 5): kept so where the code around may.
                if (made.kind == Type::Class && !made.nullable && made.cls != nullptr && made.cls->isValue)
                    fieldCandidates[&e] = IndexCandidate{effectKey(), made.cls};
                return made;
            }
            case ExprKind::Member:
                return member(static_cast<const MemberExpr&>(e));
            case ExprKind::Index:
                return index(static_cast<const IndexExpr&>(e));
            case ExprKind::List: {
                const auto& l = static_cast<const ListExpr&>(e);
                if (l.items.empty()) return literalTypes[&e] = openListOf(unknownType());
                Type item = expr(*l.items[0]);
                for (size_t i = 1; i < l.items.size(); i++) item = common(item, expr(*l.items[i]));
                if (item.kind == Type::Null) item = unknownType();
                return literalTypes[&e] = openListOf(item);
            }
            case ExprKind::Map: {
                const auto& m = static_cast<const MapExpr&>(e);
                Type t = Type::of(Type::Map);
                Type k = unknownType(), v = unknownType();
                for (size_t i = 0; i < m.entries.size(); i++) {
                    Type ki = expr(*m.entries[i].first), vi = expr(*m.entries[i].second);
                    noteKey(*m.entries[i].first, ki);
                    k = i == 0 ? ki : common(k, ki);
                    v = i == 0 ? vi : common(v, vi);
                }
                if (k.kind == Type::Null) k = unknownType();
                if (v.kind == Type::Null) v = unknownType();
                t.args = {k, v};
                t.open = true;
                return literalTypes[&e] = t;
            }
            case ExprKind::Range: {
                const auto& r = static_cast<const RangeExpr&>(e);
                for (const Expr* part : {r.start.get(), r.end.get(), r.step.get()}) {
                    if (part == nullptr) continue;
                    Type t = expr(*part);
                    const bool position = t.kind == Type::Index && part != r.step.get();  // `2..^1`, a slice's bounds (D147)
                    if (t.known() && !(t.kind == Type::Int && !t.nullable) && !position && t.kind != Type::Nothing && t.kind != Type::TypeParam)
                        report(*part, "a range is made of ints; this is " + describe(t));
                }
                return Type::of(Type::Range);
            }
            case ExprKind::Is: {
                const auto& i = static_cast<const IsExpr&>(e);
                const Type value = expr(*i.value);
                if (resolveAgainst(i.type, value).kind == Type::Union) report(i.type, unionTested());  // `x is Click` for an Event (D39)
                if (i.type.isOpen) notMadeOpen(value, resolve(i.type, *unit), i, "is");
                return Type::of(Type::Bool);
            }
            case ExprKind::ClassOf: return classOfExpr(static_cast<const ClassOfExpr&>(e));
            // The target of `.=`, as it is (D156).
            case ExprKind::Current: return currentTargets.empty() ? unknownType() : currentTargets.back();
            case ExprKind::Cast: {
                const auto& c = static_cast<const CastExpr&>(e);
                const Type value = expr(*c.value);
                Type t = resolve(c.type, *unit);
                if (t.kind == Type::Union) report(c.type, unionTested());
                if (c.type.isOpen) notMadeOpen(value, t, c, "as");
                return c.safe ? withNull(t) : t;
            }
            case ExprKind::In: {
                const auto& i = static_cast<const InExpr&>(e);
                const Type value = expr(*i.value);
                const Type container = expr(*i.container);
                if (container.kind == Type::Map && !container.nullable && !wholeEntry(value))
                    report(*i.value, "'in' a map asks for a whole entry, (key to value); for a key write " + placeText(*i.value) +
                                         " in map.keys, and for a value " + placeText(*i.value) + " in map.values (D140)");
                return Type::of(Type::Bool);
            }
            case ExprKind::Elvis: {
                const auto& el = static_cast<const ElvisExpr&>(e);
                Type a = expr(*el.left);
                Type b = exprExpected(*el.right, withNull(a, false));
                if (a.unknown()) return unknownType();
                if (a.kind == Type::Null) return b;
                Type plain = withNull(a, false);
                if (b.kind == Type::Nothing) return plain;
                return common(plain, b);
            }
            case ExprKind::Function:
                return lambda(*static_cast<const FunctionExpr&>(e).fn, nullptr);
            case ExprKind::Jump:
                stmt(*static_cast<const JumpExpr&>(e).jump);
                return Type::of(Type::Nothing);
            case ExprKind::Statement:
                return statementValue(*static_cast<const StatementExpr&>(e).statement);
            case ExprKind::When:
                return when(static_cast<const WhenExpr&>(e));
            case ExprKind::Object: {
                const auto& o = static_cast<const ObjectExpr&>(e);
                ClassInfo* k = gatherClass(*o.cls, unit, ctx ? ctx->cls : nullptr);
                k->outerThis = outerThisHere();
                checkClass(k);
                resolveSupers(k);
                // Its own class: what it adds, and what every type it is made from has.
                if (k->unknownSuper) return unknownType();
                return classType(k);
            }
        }
        return unknownType();
    }

    Type selfTypeArgs(ClassInfo* k, const std::vector<Type>& args) {
        Type t = classType(k);
        for (size_t i = 0; i < k->typeParams.size(); i++) t.args.push_back(i < args.size() ? args[i] : unknownType());
        return t;
    }

    // `this`: the object the innermost lambda runs on, an extension's receiver, or the object
    // of the method.
    Type thisType() {
        Type t = unthisType();
        if (!t.known()) return t;
        const std::string key = thisKey();
        for (auto it = narrowed.rbegin(); it != narrowed.rend(); ++it) {
            auto f = it->find(key);
            if (f != it->end()) return f->second;
        }
        return t;
    }

    // `this@Name` (D57): the object of the class Name around the code, or of the block given to
    // the function name (or labelled name@), innermost first.
    Type labelledThis(const ThisExpr& t) {
        for (FnCtx* c = ctx; c != nullptr; c = c->parent) {
            if (c->isLambda) {
                const FunctionDecl* d = c->placeDecl;
                if (d != nullptr && c->hasReceiver && (d->label == t.label || d->calleeLabel == t.label))
                    return c->receiverUnknown ? unknownType() : c->receiver;
                continue;
            }
            if (c->cls != nullptr && !c->isStatic && c->cls->name == t.label) return selfType(c->cls);
            if (c->fn != nullptr && c->fn->isExtension && c->fn->name == t.label) return c->receiver;
            if (c->cls != nullptr && !c->isStatic)
                for (const auto& [label, type] : c->cls->outerThis)
                    if (label == t.label) return type;
            break;
        }
        report(t, "there is no class or block called '" + t.label + "' around this: this@Name names the object of a class " +
                      "the code is written in, or of the block given to the function name (D57)");
        return unknownType();
    }

    // What `this@Name` may name where a class is declared inside a function.
    std::vector<std::pair<std::string, Type>> outerThisHere() {
        std::vector<std::pair<std::string, Type>> found;
        for (FnCtx* c = ctx; c != nullptr; c = c->parent) {
            if (c->isLambda) {
                const FunctionDecl* d = c->placeDecl;
                if (d != nullptr && c->hasReceiver) {
                    const Type t = c->receiverUnknown ? unknownType() : c->receiver;
                    if (!d->label.empty()) found.emplace_back(d->label, t);
                    if (!d->calleeLabel.empty()) found.emplace_back(d->calleeLabel, t);
                }
                continue;
            }
            if (c->cls != nullptr && !c->isStatic) {
                found.emplace_back(c->cls->name, selfType(c->cls));
                for (const auto& p : c->cls->outerThis) found.push_back(p);
            }
            if (c->fn != nullptr && c->fn->isExtension) found.emplace_back(c->fn->name, c->receiver);
            break;
        }
        return found;
    }

    // What `this` is, before any test narrows it.
    Type unthisType() {
        for (FnCtx* c = ctx; c != nullptr; c = c->isLambda ? c->parent : nullptr) {
            if (c->isLambda) {
                if (c->receiverUnknown) return unknownType();
                if (c->hasReceiver) return c->receiver;
                continue;
            }
            if (c->hasReceiver) return c->receiver;
            if (c->cls != nullptr && !c->isStatic) return selfType(c->cls);
            return unknownType();
        }
        return unknownType();
    }

    // The name a test on `this` is noted under: one no local can have, and one for each body
    // `this` belongs to, so a lambda's receiver is not narrowed by a test on the function's.
    std::string thisKey() {
        FnCtx* owner = nullptr;
        for (FnCtx* c = ctx; c != nullptr; c = c->isLambda ? c->parent : nullptr) {
            owner = c;
            if (!c->isLambda || c->hasReceiver || c->receiverUnknown) break;
        }
        return "this#" + std::to_string(reinterpret_cast<std::uintptr_t>(owner));
    }

    // The value of an `if` or `try` used as an expression: that of its blocks' last lines.
    Type statementValue(const Stmt& s) {
        if (s.kind == StmtKind::If) {
            const auto& i = static_cast<const IfStmt&>(s);
            condition(*i.condition, "an if");
            std::unordered_map<std::string, Type> yes, no;
            narrowFrom(*i.condition, true, yes);
            narrowFrom(*i.condition, false, no);
            narrowed.push_back(yes);
            Type a = statementValueOf(*i.then);
            narrowed.pop_back();
            if (!i.otherwise) return unknownType();
            narrowed.push_back(no);
            Type b = statementValueOf(*i.otherwise);
            narrowed.pop_back();
            return common(a, b);
        }
        if (s.kind == StmtKind::Try) {
            const auto& t = static_cast<const TryStmt&>(s);
            TryThrows tryThrows(*this, t);
            Type value = statementValueOf(*t.body);
            for (size_t i = 0; i < t.catches.size(); i++) {
                const CatchClause& c = t.catches[i];
                push();
                const Type caught = catchType(c);
                catchOrder(t, i, caught);
                tryThrows.clause(i, caught);
                if (c.hasVariable) tryThrows.declare(c.variable, caught, i);
                value = common(value, statementValueOf(*c.body));
                pop();
            }
            tryThrows.finally();
            if (t.finalizer) block(t.finalizer->statements);
            return value;
        }
        stmt(s);
        return unknownType();
    }

    Type statementValueOf(const Stmt& body) {
        if (body.kind == StmtKind::Expr) return expr(*static_cast<const ExprStmt&>(body).expr);
        if (body.kind == StmtKind::Block) {
            const auto& b = static_cast<const BlockStmt&>(body);
            push();
            Type t = unknownType();
            for (size_t i = 0; i < b.statements.size(); i++) {
                const Stmt& st = *b.statements[i];
                if (i + 1 == b.statements.size()) t = statementValueOf(st);
                else stmt(st);
            }
            pop();
            return t;
        }
        if (body.kind == StmtKind::If || body.kind == StmtKind::Try) return statementValue(body);
        stmt(body);
        return alwaysLeaves(body) ? Type::of(Type::Nothing) : unknownType();
    }

    Type when(const WhenExpr& w) {
        Type subject = unknownType();
        push();
        if (w.subject) {
            subject = expr(*w.subject);
            if (!w.subjectName.empty()) declareLocal(w.subjectName, subject, true);
        }
        Type result;
        bool first = true;
        bool hasElse = false;
        std::unordered_set<std::string> covered;  // enum entries, true/false, null
        std::vector<ClassInfo*> coveredKinds;      // `is Kind` over an enum class (D39)
        std::vector<Type> coveredTypes;            // `is T`, for a union (D40)
        for (const WhenBranch& b : w.branches) {
            if (b.isElse) hasElse = true;
            std::unordered_map<std::string, Type> narrow;
            for (const WhenCondition& c : b.conditions) {
                if (c.kind == WhenCondition::Kind::Is) {
                    if (w.subject) resolveAgainst(c.type, subject);  // `is Click` over an Event (D39)
                    if (w.subject && !c.negated) {
                        const Type kind = resolve(c.type, *unit);
                        if (kind.kind == Type::Class && kind.cls != nullptr && !kind.nullable) coveredKinds.push_back(kind.cls);
                        if (kind.known()) coveredTypes.push_back(withNull(kind, false));
                        if (kind.kind == Type::Union) report(c.type, unionTested());
                        if (kind.kind == Type::Null || (kind.nullable && kind.kind == Type::Null)) covered.insert("null");
                    }
                    if (w.subject && w.subject->kind == ExprKind::Name && !c.negated && b.conditions.size() == 1) {
                        Type t = resolve(c.type, *unit);
                        if (t.known()) narrow[static_cast<const NameExpr&>(*w.subject).name] = t;
                    }
                } else if (c.value) {
                    if (w.subject && c.kind == WhenCondition::Kind::Value) noteCovered(*c.value, subject, covered);
                    Type t = w.subject && c.kind == WhenCondition::Kind::Value && subject.kind == Type::Class && subject.cls &&
                                     subject.cls->isEnum && c.value->kind == ExprKind::Name
                                 ? enumEntry(subject, static_cast<const NameExpr&>(*c.value).name, *c.value)
                             : w.subject && c.kind == WhenCondition::Kind::Value && subject.kind == Type::Class && subject.cls &&
                                     subject.cls->isEnumClass
                                 ? exprExpected(*c.value, subject)  // `Close ->` over an Event (D39)
                                 : expr(*c.value);
                    if (!w.subject && c.kind == WhenCondition::Kind::Value && t.known() &&
                        !(t.kind == Type::Bool && !t.nullable) && t.kind != Type::Nothing)
                        report(*c.value, "a condition of a when must be a bool, not " + describe(t));
                    if (!w.subject && c.kind == WhenCondition::Kind::Value && b.conditions.size() == 1)
                        narrowFrom(*c.value, true, narrow);
                }
            }
            narrowed.push_back(narrow);
            Type t = statementValueOf(*b.body);
            narrowed.pop_back();
            result = first ? t : common(result, t);
            first = false;
        }
        pop();
        // Every when over an enum or a bool covers every kind or has an else (L21).
        if (w.subject && !hasElse) {
            std::vector<std::string> missing;
            if (subject.kind == Type::Class && subject.cls != nullptr && subject.cls->isEnum) {
                for (const EnumEntry& entry : subject.cls->decl->entries)
                    if (!covered.count(entry.name)) missing.push_back(subject.cls->name + "." + entry.name);
            } else if (subject.kind == Type::Class && subject.cls != nullptr && subject.cls->isEnumClass) {
                // Each kind: by `is` it or a class above it, an object kind also by its name (D39).
                std::function<bool(ClassInfo*)> isCovered = [&](ClassInfo* kind) {
                    for (ClassInfo* by : coveredKinds)
                        if (isSubclass(kind, by)) return true;
                    if (kind->isObject && covered.count(kind->decl->name)) return true;
                    if (!kind->isEnumClass) return false;
                    const std::vector<ClassInfo*> inner = kindsOf(kind);
                    if (inner.empty()) return false;
                    for (ClassInfo* k : inner)
                        if (!isCovered(k)) return false;
                    return true;
                };
                for (ClassInfo* kind : kindsOf(subject.cls))
                    if (!isCovered(kind)) missing.push_back(kind->isObject ? kind->name : "is " + kind->name);
            } else if (subject.kind == Type::Bool) {
                for (const char* v : {"true", "false"})
                    if (!covered.count(v)) missing.push_back(v);
            } else if (subject.kind == Type::Union) {
                // Each of its types (D40).
                for (const Type& m : subject.args) {
                    bool done = false;
                    for (const Type& by : coveredTypes) done = done || fits(m, by);
                    if (!done) missing.push_back("is " + text(m));
                }
            }
            if (!missing.empty() || (subject.kind == Type::Class && subject.cls && (subject.cls->isEnum || subject.cls->isEnumClass)) ||
                subject.kind == Type::Bool || subject.kind == Type::Union) {
                if (subject.nullable && !covered.count("null")) missing.push_back("null");
                if (!missing.empty()) {
                    std::string list;
                    for (size_t i = 0; i < missing.size(); i++)
                        list += (i == 0 ? "" : i + 1 == missing.size() ? " and " : ", ") + missing[i];
                    report(w, "this when does not cover " + list + "; add " + (missing.size() == 1 ? "it" : "them") +
                                  ", or an else branch");
                }
            }
        }
        return first ? unknownType() : result;
    }

    // A value a when's branch compares its subject with, noted for covering every kind.
    void noteCovered(const Expr& v, const Type& subject, std::unordered_set<std::string>& covered) {
        if (isNullLiteral(v)) {
            covered.insert("null");
        } else if (v.kind == ExprKind::Literal && static_cast<const LiteralExpr&>(v).type == LiteralExpr::Type::Bool) {
            covered.insert(static_cast<const LiteralExpr&>(v).boolValue ? "true" : "false");
        } else if (subject.kind == Type::Class && subject.cls != nullptr && (subject.cls->isEnum || subject.cls->isEnumClass)) {
            if (v.kind == ExprKind::Name) covered.insert(static_cast<const NameExpr&>(v).name);
            else if (v.kind == ExprKind::Member) covered.insert(static_cast<const MemberExpr&>(v).name);
        }
    }

    // A bare enum entry in a when over that enum (`when (way) { up -> ... }`).
    Type enumEntry(const Type& subject, const std::string& entryName, const Expr& at) {
        if (findLocal(entryName) == nullptr && subject.cls->statics.count(entryName)) return withNull(subject, false);
        return expr(at);
    }

    // ---- Names ----

    // A bare name: a local, a member of an object a lambda or extension runs on, a member of
    // the class around the code, or a top-level name.
    // Whether a bare name, written where `n` is, means a global: it is not a variable, nor a
    // class declared in the function, nor a member of an object the code runs on or of the
    // class around it, which all come first (see name).
    bool meansGlobal(const NameExpr& n) {
        if (findLocal(n.name) != nullptr || localClass(n.name) != nullptr) return false;
        bool escape = false;
        if (onReceivers(n.name, escape) != nullptr || escape) return false;
        if (ctx != nullptr && ctx->cls != nullptr) {
            if (!ctx->isStatic && lookupMember(selfType(ctx->cls), n.name).found()) return false;
            for (ClassInfo* c = ctx->cls; c != nullptr; c = c->outer)
                if (c->statics.count(n.name) != 0 || c->nested.count(n.name) != 0) return false;
        }
        return true;
    }

    Type name(const NameExpr& n) {
        if (Local* l = findLocal(n.name)) {
            if (n.name == "it") noteItUse(*l, n);
            if (l->loopOf != nullptr) {
                LoopCandidate& loop = loopCandidates[l->loopOf];
                loop.uses.insert(&n);
                if (&n == passedUse || &n == valueCallArg) loop.argUses.insert(&n);
            }
            if (&n != passedUse) noteUse(*l, ParamUse::Other);
            noteConst(n, l->constant);
            if (l->val != nullptr && silent == 0) valUses[&n] = l->val;
            return localType(n.name, *l);
        }
        if (pathsNarrowed()) {
            const std::string key = stablePath(n);
            if (!key.empty())
                for (auto it = narrowed.rbegin(); it != narrowed.rend(); ++it) {
                    auto f = it->find(key);
                    if (f != it->end()) return f->second;
                }
        }
        if (ClassInfo* k = localClass(n.name)) return k->isObject ? classType(k) : unknownType();
        bool escape = false;
        if (const Type* t = onReceivers(n.name, escape)) return *t;
        if (escape) return unknownType();
        if (ctx != nullptr && ctx->cls != nullptr) {
            if (!ctx->isStatic) {
                Member m = lookupMember(selfType(ctx->cls), n.name);
                noteKeyMember(m, n.name);
                if (m.field) {
                    if (ClassInfo* t = insideObject(ctx->cls, fieldType(m.field))) fieldCandidates[&n] = IndexCandidate{effectKey(), t};
                    return subst(fieldType(noteConst(n, m.field)), m.env);
                }
                if (m.getter) {
                    noteCallThrows(m.getter, nullptr, noArgs(), nullptr, n);
                    return subst(resultOf(m.getter), m.env);
                }
            }
            for (ClassInfo* c = ctx->cls; c != nullptr; c = c->outer) {
                auto s = c->statics.find(n.name);
                if (s != c->statics.end()) return fieldType(noteConst(n, s->second));
                auto nested = c->nested.find(n.name);
                if (nested != c->nested.end()) return nested->second->isObject ? classType(nested->second) : unknownType();
            }
        }
        if (const GlobalSym* g = findGlobal(*unit, n.name)) {
            if (g->var != nullptr) return fieldType(noteConst(n, g->var));
            if (g->cls != nullptr) return g->cls->isObject ? classType(g->cls) : unknownType();
            if (g->funcs.size() == 1) {
                noteFunctionValue(g->funcs[0], n);
                return functionValue(g->funcs[0]);
            }
        } else if (std::string advice = removedName(n.name); !advice.empty()) {
            report(n, advice);
        }
        return unknownType();
    }

    // A name looked for on the objects the code runs on, innermost first. Sets `escape` when
    // one of them is not known well enough to say the name is not its.
    std::unique_ptr<Type> receiverHit;
    const Type* onReceivers(const std::string& name, bool& escape) {
        for (FnCtx* c = ctx; c != nullptr; c = c->isLambda ? c->parent : nullptr) {
            if (c->isLambda && c->receiverUnknown) {
                escape = true;
                return nullptr;
            }
            if (!c->hasReceiver) {
                if (!c->isLambda) break;
                continue;
            }
            const Type& r = c->receiver;
            if (r.kind == Type::Class && !r.nullable) {
                Member m = lookupMember(r, name);
                if (m.found() && hiddenHere(m)) m = Member{};
                if (m.field) {
                    receiverHit = std::make_unique<Type>(subst(fieldType(m.field), m.env));
                    return receiverHit.get();
                }
                if (m.getter) {
                    receiverHit = std::make_unique<Type>(subst(resultOf(m.getter), m.env));
                    return receiverHit.get();
                }
                if (!m.methods.empty() || m.setter) {
                    escape = true;
                    return nullptr;
                }
                if (!membersKnown(r)) {
                    escape = true;
                    return nullptr;
                }
            } else {
                // A built-in type, an unknown one, or a type parameter: its members are not all known.
                escape = true;
                return nullptr;
            }
            if (!c->isLambda) break;
        }
        return nullptr;
    }

    Type functionValue(FuncInfo* f) {
        resolveFunc(f);
        Type t = Type::of(Type::Function);
        t.knownParams = f->vararg < 0 && f->typeParams.empty();
        t.args = f->params;
        t.required = 0;
        for (size_t i = 0; i < f->params.size(); i++)
            if (i >= f->hasDefault.size() || !f->hasDefault[i]) t.required = static_cast<int>(i) + 1;
        t.result = std::make_shared<Type>(f->typeParams.empty() ? resultOf(f) : unknownType());
        t.fnRef = f;
        t.throws = writtenThrowsTypes(f);
        t.suspend = f->decl != nullptr && f->decl->isSuspend;
        return t;
    }

    Type unary(const UnaryExpr& u) {
        Type t = expr(*u.operand);
        if (t.unknown() || t.kind == Type::TypeParam) return u.op == TokenType::Bang ? Type::of(Type::Bool) : (u.op == TokenType::BangBang ? withNull(t, false) : unknownType());
        switch (u.op) {
            case TokenType::Bang:
                if (!(t.kind == Type::Bool && !t.nullable) && t.kind != Type::Nothing)
                    report(u, "'!' takes a bool; this is " + describe(t));
                return Type::of(Type::Bool);
            case TokenType::Minus:
                if (t.kind == Type::Class) return operatorResult(t, "unary-", u, {});
                if (t.kind == Type::Byte && !t.nullable) return Type::of(Type::Int);  // a byte negated is an int (D19)
                if (!isNumeric(t)) report(u, "'-' needs a number; this is " + describe(t));
                return isNumeric(t) ? t : unknownType();
            case TokenType::Tilde:
                if (t.kind == Type::Class) return operatorResult(t, "unary~", u, {});
                if (!((t.kind == Type::Int || t.kind == Type::Byte) && !t.nullable)) report(u, "'~' needs an int; this is " + describe(t));
                return Type::of(Type::Int);
            case TokenType::BangBang:
                return withNull(t, false);
            case TokenType::Caret:
                // `^k`: k from the end (D146).
                if (!(t.kind == Type::Int && !t.nullable)) report(u, "'^' counts from the end by an int; this is " + describe(t));
                if (const ConstValue k = foldQuietly(*u.operand); k.kind == ConstValue::Int && k.i < 1)
                    report(u, "^" + std::to_string(k.i) + " is no position: ^1 is the last, ^2 the one before it");
                return Type::of(Type::Index);
            default:
                return unknownType();
        }
    }

    // The result of an operator a class declares (`op +(other)`); reports one it lacks.
    // Whether a member or index chain goes through ?. somewhere.
    static bool safeChain(const Expr& e) {
        if (e.kind == ExprKind::Member) {
            const auto& m = static_cast<const MemberExpr&>(e);
            return m.safe || safeChain(*m.object);
        }
        return false;
    }

    Type operatorResult(const Type& left, const std::string& op, const Node& at, std::vector<Type> args) {
        if (left.cls == nullptr) return unknownType();
        Member m = lookupMember(withNull(left, false), op);
        std::vector<FuncInfo*> found = m.methods;
        if (found.empty()) found = findExtensions(left, op);
        if (found.empty()) {
            if (reportMissing(left.cls, op)) {
                std::string shown = op.rfind("unary", 0) == 0 ? op.substr(5) : op;
                report(at, left.cls->name + " has no operator " + shown + " (op " + shown + "(...) in the class)");
            }
            return unknownType();
        }
        FuncInfo* f = found.size() == 1 ? found[0] : operatorForm(found, m.env, args);
        if (f == nullptr) {
            bool known = true, anyFits = false;
            for (const Type& a : args) known = known && !a.unknown();
            for (FuncInfo* g : found) {
                bool ok = g->params.size() == args.size();
                for (size_t i = 0; i < args.size() && ok; i++) ok = fits(args[i], subst(g->params[i], m.env));
                anyFits = anyFits || ok;
            }
            if (known && !anyFits) {
                std::string given;
                for (const Type& a : args) given += (given.empty() ? "" : ", ") + describe(a);
                report(at, "no form of " + left.cls->name + "'s operator " + op + " takes " + given);
            }
            return unknownType();
        }
        resolveFunc(f);
        for (size_t i = 0; i < args.size() && i < f->params.size(); i++) {
            Type want = subst(f->params[i], m.env);
            if (!fits(args[i], want))
                report(at, left.cls->name + "'s operator " + op + " takes " + describe(want) + ", not " + describe(args[i]));
        }
        return subst(resultOf(f), m.env);
    }

    static std::string opText(TokenType op) {
        switch (op) {
            case TokenType::Plus: return "+";
            case TokenType::Minus: return "-";
            case TokenType::Star: return "*";
            case TokenType::Slash: return "/";
            case TokenType::Percent: return "%";
            case TokenType::Lt: return "<";
            case TokenType::LtEq: return "<=";
            case TokenType::Gt: return ">";
            case TokenType::GtEq: return ">=";
            case TokenType::Amp: return "&";
            case TokenType::Pipe: return "|";
            case TokenType::Caret: return "^";
            case TokenType::Shl: return "<<";
            case TokenType::Shr: return ">>";
            case TokenType::Ushr: return ">>>";
            case TokenType::AmpPlus: return "&+";
            case TokenType::AmpMinus: return "&-";
            case TokenType::AmpStar: return "&*";
            default: return "?";
        }
    }

    Type binary(const BinaryExpr& b) {
        Type l = expr(*b.left);
        Type r = expr(*b.right);
        const TokenType op = b.op;
        // Two bundles added: the names of both, the right one's winning (D143).
        if (op == TokenType::Plus && withNull(l, false).kind == Type::Bundle && withNull(r, false).kind == Type::Bundle) {
            Type made = withNull(l, false);
            for (size_t i = 0; i < r.fields.size(); i++) {
                auto at = std::find(made.fields.begin(), made.fields.end(), r.fields[i]);
                if (at == made.fields.end()) {
                    made.fields.push_back(r.fields[i]);
                    made.args.push_back(r.args[i]);
                } else {
                    made.args[static_cast<size_t>(at - made.fields.begin())] = r.args[i];
                }
            }
            if (silent == 0) bundleAdds.insert(&b);
            return made;
        }
        switch (op) {
            case TokenType::EqEq:
            case TokenType::BangEq:
            case TokenType::EqEqEq:
            case TokenType::BangEqEq:
                for (const Type* side : {&l, &r})
                    if ((op == TokenType::EqEqEq || op == TokenType::BangEqEq) && side->kind == Type::Class && side->cls != nullptr && side->cls->isValue) {
                        report(b, "a value has no identity, so " + std::string(op == TokenType::EqEqEq ? "===" : "!==") +
                                      " cannot tell two values of " + side->cls->name + " apart; == compares them by their fields");
                        break;
                    }
                // A char equals a string of that one character (D25); a string known when
                // compiling to be of another length never does.
                if ((l.kind == Type::Char && r.kind == Type::String) || (l.kind == Type::String && r.kind == Type::Char)) {
                    const Expr& text = l.kind == Type::String ? *b.left : *b.right;
                    const ConstValue known = foldQuietly(text);
                    if (known.kind == ConstValue::String && countCharacters(known.s) != 1)
                        report(text, "a char is one character, so it is never equal to \"" + known.s + "\", which holds " +
                                         std::to_string(countCharacters(known.s)));
                }
                return Type::of(Type::Bool);
            default:
                break;
        }
        const bool comparison = op == TokenType::Lt || op == TokenType::LtEq || op == TokenType::Gt || op == TokenType::GtEq;
        if (l.kind == Type::Nothing || r.kind == Type::Nothing) return Type::of(Type::Nothing);
        if (op == TokenType::Plus && ((l.kind == Type::String && !l.nullable) || (r.kind == Type::String && !r.nullable)))
            return Type::of(Type::String);
        if (l.unknown() || r.unknown() || l.kind == Type::TypeParam || r.kind == Type::TypeParam)
            return comparison ? Type::of(Type::Bool) : unknownType();
        if (l.kind == Type::Class && !l.nullable) {
            Type result = operatorResult(l, opText(op), b, {r});
            return comparison ? Type::of(Type::Bool) : result;
        }
        if (l.nullable || r.nullable) {
            report(b, "'" + opText(op) + "' cannot take " + describe(l.nullable ? l : r) +
                          ", which may be null; check it first, or give a value for null with ?:");
            return comparison ? Type::of(Type::Bool) : unknownType();
        }
        // A byte in arithmetic is the int it holds (D19); a char adds as D25 has it.
        if (l.kind == Type::Byte) l = Type::of(Type::Int);
        if (r.kind == Type::Byte) r = Type::of(Type::Int);
        if (l.kind == Type::Char || r.kind == Type::Char) {
            if (comparison) {
                if (l.kind != Type::Char || r.kind != Type::Char)
                    report(b, "'" + opText(op) + "' cannot compare " + describe(l) + " with " + describe(r));
                return Type::of(Type::Bool);
            }
            const Type result = charArithmetic(op, l, r);
            if (result.unknown()) report(b, "'" + opText(op) + "' cannot be used on " + describe(l) + " and " + describe(r));
            return result;
        }
        switch (op) {
            case TokenType::Plus:
            case TokenType::Minus:
            case TokenType::Star:
            case TokenType::Slash:
            case TokenType::Percent:
                if (isNumeric(l) && isNumeric(r)) return arithmetic(l, r);
                // A new list or set, its maker's to change (D32).
                if (op == TokenType::Plus && l.kind == Type::List && r.kind == Type::List) return made(common(withNull(l, false), withNull(r, false)));
                // set + x and set - x: a new set, with or without x, or what a collection holds.
                if ((op == TokenType::Plus || op == TokenType::Minus) && l.kind == Type::Set) return made(l);
                if (op == TokenType::Plus && (l.kind == Type::List || l.kind == Type::Map || l.kind == Type::Bytes))
                    return unknownType();
                if (op == TokenType::Star && l.kind == Type::String && r.kind == Type::Int) return Type::of(Type::String);
                report(b, "'" + opText(op) + "' cannot be used on " + describe(l) + " and " + describe(r));
                return unknownType();
            case TokenType::Lt:
            case TokenType::LtEq:
            case TokenType::Gt:
            case TokenType::GtEq:
                if (!((isNumeric(l) && isNumeric(r)) || (l.kind == Type::String && r.kind == Type::String)))
                    report(b, "'" + opText(op) + "' cannot compare " + describe(l) + " with " + describe(r));
                return Type::of(Type::Bool);
            case TokenType::Amp:
            case TokenType::Pipe:
            case TokenType::Caret:
            case TokenType::Shl:
            case TokenType::Shr:
            case TokenType::Ushr:
            case TokenType::AmpPlus:
            case TokenType::AmpMinus:
            case TokenType::AmpStar:
                if (l.kind != Type::Int || r.kind != Type::Int) {
                    report(b, "'" + opText(op) + "' works on ints; these are " + describe(l) + " and " + describe(r));
                    return unknownType();
                }
                return Type::of(Type::Int);
            default:
                return unknownType();
        }
    }

    // `char + char` is a string, `char +- int` a char, `char - char` an int, `int +- char` an
    // int (D25); Unknown for anything else.
    static Type charArithmetic(TokenType op, const Type& l, const Type& r) {
        const bool lc = l.kind == Type::Char, rc = r.kind == Type::Char;
        const bool li = l.kind == Type::Int, ri = r.kind == Type::Int;
        if (op == TokenType::Plus && lc && rc) return Type::of(Type::String);
        if ((op == TokenType::Plus || op == TokenType::Minus) && lc && ri) return Type::of(Type::Char);
        if (op == TokenType::Minus && lc && rc) return Type::of(Type::Int);
        if ((op == TokenType::Plus || op == TokenType::Minus) && li && rc) return Type::of(Type::Int);
        return unknownType();
    }

    static Type arithmetic(const Type& l, const Type& r) {
        if (l.kind == Type::Int && r.kind == Type::Int) return Type::of(Type::Int);
        if (l.kind == Type::Float || r.kind == Type::Float) return Type::of(Type::Float);
        if (l.kind == Type::Number || r.kind == Type::Number) return Type::of(Type::Number);
        return Type::of(Type::Float);
    }

    // ---- Assignment ----

    // ---- Changing a value through its place (M6.6 part 4, value-lists.html section 8) ----

    // The expressions of a value type that are places a change is made through, for the
    // compiler: it reaches the value where it is kept rather than reading a copy.
    std::unordered_set<const Expr*> valuePlaces;

    // The value type the code being checked belongs to and may change `this` of: a mutating
    // method's, or one whose value is being made.
    ClassInfo* changesThis() {
        for (FnCtx* c = ctx; c != nullptr; c = c->parent) {
            if (c->cls != nullptr && c->cls->isValue &&
                (c->noWait == ObjectMade || (c->fn != nullptr && (c->fn->name == "init" || (c->fn->decl != nullptr && c->fn->decl->isMutating)))))
                return c->cls;
            if (!c->isLambda) break;
        }
        return nullptr;
    }

    static FieldInfo* fieldNamed(ClassInfo* k, const std::string& name) {
        for (; k != nullptr; k = k->superclass) {
            auto found = k->fields.find(name);
            if (found != k->fields.end()) return found->second;
        }
        return nullptr;
    }

    Type typeQuietly(const Expr& e) {
        silent++;
        Type t = withNull(expr(e), false);
        silent--;
        return t;
    }

    // Whether an expression of a value type is a place that keeps a change made through it: a
    // var local, a lambda's parameter, a var global, a var field of a place or of an object, an
    // item of a list, or `this` in a mutating method. Noted for the compiler where it is.
    bool isPlace(const Expr& e) {
        bool place = false;
        switch (e.kind) {
            case ExprKind::Name: {
                const auto& n = static_cast<const NameExpr&>(e);
                if (Local* l = findLocal(n.name)) {
                    place = !l->isVal || l->lambdaParam;
                } else if (const GlobalSym* g = meansGlobal(n) ? findGlobal(*unit, n.name) : nullptr; g != nullptr && g->var != nullptr) {
                    place = !g->var->isVal;
                } else if (ctx != nullptr && ctx->cls != nullptr) {
                    // A field of the object or value the method runs on.
                    FieldInfo* field = fieldNamed(ctx->cls, n.name);
                    place = field != nullptr && !field->isVal && (!ctx->cls->isValue || changesThis() == ctx->cls);
                }
                break;
            }
            case ExprKind::This:
                place = changesThis() != nullptr;
                break;
            case ExprKind::Index:
                place = typeQuietly(*static_cast<const IndexExpr&>(e).object).kind == Type::List;
                break;
            case ExprKind::Member: {
                const auto& m = static_cast<const MemberExpr&>(e);
                if (m.safe) break;
                const Type object = typeQuietly(*m.object);
                if (object.kind != Type::Class || object.cls == nullptr) break;
                FieldInfo* field = fieldNamed(object.cls, m.name);
                if (field == nullptr || field->isVal) break;
                place = !object.cls->isValue || isPlace(*m.object);
                break;
            }
            default:
                break;
        }
        if (place && silent == 0) valuePlaces.insert(&e);
        return place;
    }

    // `p.x = 4` on a value: only a var field, only through a place (M6.6 part 4); a value's
    // fields are given their values freely while it is being made.
    void checkValueFieldWrite(const Expr& target) {
        ClassInfo* owner = nullptr;
        std::string name;
        const Expr* object = nullptr;
        if (target.kind == ExprKind::Member) {
            const auto& m = static_cast<const MemberExpr&>(target);
            const Type type = typeQuietly(*m.object);
            if (type.kind == Type::Class && type.cls != nullptr) owner = type.cls;
            name = m.name;
            object = m.object.get();
        } else if (target.kind == ExprKind::Name) {
            const auto& n = static_cast<const NameExpr&>(target);
            bool escape = false;
            if (findLocal(n.name) != nullptr || onReceivers(n.name, escape) != nullptr || escape) return;
            if (ctx != nullptr) owner = ctx->cls;
            name = n.name;
        }
        if (owner == nullptr || !owner->isValue) return;
        FieldInfo* field = fieldNamed(owner, name);
        if (field == nullptr) return;
        for (FnCtx* c = ctx; c != nullptr; c = c->parent) {
            if (c->cls == owner && (c->noWait == ObjectMade || (c->fn != nullptr && c->fn->name == "init"))) return;
            if (!c->isLambda) break;
        }
        if (field->isVal) {
            report(target, "'" + name + "' of value " + owner->name + " is a val: it is given its value when the value is made; " +
                               "declare it var to change it through the place that holds the value");
            return;
        }
        if (object == nullptr) {
            if (changesThis() != owner)
                report(target, "'" + name + "' of value " + owner->name + " can be changed only by a method marked mutating, " +
                                   "which is called on the place that holds the value: mutating fun ...");
            return;
        }
        if (!isPlace(*object))
            report(target, "'" + name + "' cannot be given a new value here: this value of " + owner->name + " is kept nowhere that " +
                               "could take the change (a var, a var field or an item of a list); make a changed copy, copy(" + name +
                               " = ...)");
        else
            notePlaceChange(*object, owner);
    }

    const CallExpr* methodCall = nullptr;  // the call callFunctions is checking

    // ---- Which lists a function may change (M6.6 part 4, value-lists.html section 6) ----
    //
    // A value may be read where it is, in an item of a list, only where nothing changes the
    // list while it is in use: no item taken out, put in before it, replaced, changed, or moved
    // by a sort. (Adding at the end moves none.) What each function does to lists of each value
    // type is noted as it is checked, with what it calls and the lambdas written in it, which
    // may run while it does; what calls reach is joined at the end.

    struct Effects {
        bool any = false;          // may change a list of any value type, or calls what is not known
        bool callsParams = false;  // calls a function it was given, which its callers account for
        std::unordered_set<ClassInfo*> types;
        std::vector<const void*> calls;      // the functions it calls and the lambdas in it, by declaration
        std::vector<FuncInfo*> methodCalls;  // methods it calls, whose overrides are added at the end
    };
    std::unordered_map<const void*, Effects> effects;
    bool effectsJoined = false;

    // The function the code being checked belongs to, as effects knows it: a lambda's or a
    // function's declaration, or the file for its top level.
    const void* effectKey() const {
        for (const FnCtx* c = ctx; c != nullptr; c = c->parent) {
            if (c->placeDecl != nullptr) return c->placeDecl;
            if (c->fn != nullptr) return c->fn->decl != nullptr ? static_cast<const void*>(c->fn->decl) : static_cast<const void*>(c->fn);
            if (c->topLevel) break;
        }
        return unit;
    }

    // The for loops being checked, the innermost last: the function each is in, and how many
    // scopes there were around it. What the innermost loop of the code being checked may change
    // is noted for it, and for the loops and the function around when it ends.
    struct ActiveLoop {
        const void* fn;
        const ForInStmt* loop;
        size_t depth;
    };
    std::vector<ActiveLoop> activeLoops;
    const void* effectKeyHere() const {
        const void* fn = effectKey();
        if (!activeLoops.empty() && activeLoops.back().fn == fn) return activeLoops.back().loop;
        return fn;
    }

    // `kept = p` in a loop, to a variable from outside it that may hold a value: an item read
    // where it is might be kept past the loop, so the loops it is in are judged by the whole
    // function instead.
    void noteKeptPastLoops(const std::string& name, const Type& type) {
        if (activeLoops.empty()) return;
        if (type.known() && type.kind != Type::Any && type.kind != Type::TypeParam &&
            !(type.kind == Type::Class && type.cls != nullptr && type.cls->isValue))
            return;
        size_t at = scopes.size();
        while (at > 0 && scopes[at - 1].count(name) == 0) at--;
        if (at == 0) return;
        const void* fn = effectKey();
        for (const ActiveLoop& l : activeLoops)
            if (l.fn == fn && at - 1 < l.depth) loopCandidates[l.loop].keptPast = true;
    }

    // A change to a list of `list`'s type: of its value type's lists, or, for a list that may be
    // seen as one of any (List<any>, List<T>), of every one.
    void noteListChange(const Type& list) {
        const Type l = withNull(list, false);
        if (l.kind != Type::List) return;
        Effects& here = effects[effectKeyHere()];
        if (l.args.size() != 1 || l.args[0].unknown() || l.args[0].kind == Type::Any || l.args[0].kind == Type::TypeParam) {
            here.any = true;
            return;
        }
        if (l.args[0].kind == Type::Class && l.args[0].cls != nullptr && l.args[0].cls->isValue) here.types.insert(l.args[0].cls);
    }

    // A change made through a place: one that is an item of a list, or may be (a parameter, or
    // `this` in a mutating method), changes a list of its type.
    void notePlaceChange(const Expr& place, ClassInfo* type) {
        const Expr* root = &place;
        while (root->kind == ExprKind::Member) {
            // Through a field of an object, the change is to a value kept inside it (M6.6 part 5).
            const auto& m = static_cast<const MemberExpr&>(*root);
            const Type owner = typeQuietly(*m.object);
            if (owner.kind == Type::Class && owner.cls != nullptr && !owner.cls->isValue) {
                if (FieldInfo* f = fieldNamed(owner.cls, m.name))
                    if (ClassInfo* t = insideObject(owner.cls, fieldType(f))) effects[effectKeyHere()].types.insert(t);
                return;
            }
            root = m.object.get();
        }
        if (root->kind == ExprKind::Name && findLocal(static_cast<const NameExpr&>(*root).name) == nullptr && ctx != nullptr &&
            ctx->cls != nullptr && !ctx->cls->isValue) {
            if (FieldInfo* f = fieldNamed(ctx->cls, static_cast<const NameExpr&>(*root).name)) {
                if (ClassInfo* t = insideObject(ctx->cls, fieldType(f))) effects[effectKeyHere()].types.insert(t);
                return;
            }
        }
        if (root->kind == ExprKind::Index) {
            noteListChange(typeQuietly(*static_cast<const IndexExpr&>(*root).object));
            return;
        }
        if (root->kind == ExprKind::Name) {
            const Local* l = findLocal(static_cast<const NameExpr&>(*root).name);
            if (l == nullptr || l->paramOfDecl == nullptr) return;  // a variable of its own
            placeParams.insert({l->paramOfDecl, static_cast<const NameExpr&>(*root).name});
        }
        const Type rootType = typeQuietly(*root);
        if (rootType.kind == Type::Class && rootType.cls != nullptr && rootType.cls->isValue) effects[effectKeyHere()].types.insert(rootType.cls);
        else effects[effectKeyHere()].types.insert(type);
    }

    static bool listChangingName(const std::string& name, size_t args) {
        static const std::unordered_set<std::string> changing = {
            "insert", "remove", "removeAt", "removeFirst", "removeLast", "removeFirstOrNull", "removeLastOrNull", "clear", "sort",
            "sortBy", "sortByDescending", "sortWith", "sortDescending", "reverse", "fill", "set", "shuffle", "removeAll",
            "retainAll", "removeIf", "replaceAll", "pop"};
        return changing.count(name) != 0 || ((name == "add" || name == "addAll") && args == 2);
    }

    void noteCallEffects(FuncInfo* f, const Type* receiver, const Args& args) {
        Effects& here = effects[effectKeyHere()];
        if (f->isExternal) {
            if (f->isExtension && receiver != nullptr && listChangingName(f->name, args.exprs.size())) noteListChange(*receiver);
            return;
        }
        if (f->decl != nullptr) here.calls.push_back(f->decl);
        if (f->owner != nullptr && !f->isStatic) here.methodCalls.push_back(f);
    }

    // The effects of what each function calls, joined into it, until nothing more is added.
    void joinEffects() {
        if (effectsJoined) return;
        effectsJoined = true;
        for (auto& [key, e] : effects)
            for (FuncInfo* f : e.methodCalls)
                for (FuncInfo* g : overridersOf(f))
                    if (g->decl != nullptr) e.calls.push_back(g->decl);
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& [key, e] : effects) {
                for (const void* callee : e.calls) {
                    auto found = effects.find(callee);
                    if (found == effects.end() || found->first == key) continue;
                    const Effects& c = found->second;
                    if (c.any && !e.any) {
                        e.any = true;
                        changed = true;
                    }
                    for (ClassInfo* t : c.types)
                        if (e.types.insert(t).second) changed = true;
                }
            }
        }
    }

    // Whether code of `key` may change a list of `type` (null: of a type it cannot tell) while
    // it runs, a function it was given counting as one that may.
    bool mayChangeLists(const void* key, ClassInfo* type) {
        joinEffects();
        auto found = effects.find(key);
        if (found == effects.end()) return false;
        const Effects& e = found->second;
        return e.any || e.callsParams || type == nullptr || e.types.count(type) != 0;
    }

    struct IndexCandidate {
        const void* key = nullptr;
        ClassInfo* type = nullptr;
    };
    std::unordered_map<const IndexExpr*, IndexCandidate> indexCandidates;

    struct LoopCandidate {
        const void* key = nullptr;  // the function the loop is in
        Type item;
        std::unordered_set<const NameExpr*> uses, argUses;
        bool keptPast = false;  // noteKeptPastLoops
    };
    std::unordered_map<const ForInStmt*, LoopCandidate> loopCandidates;

    struct ParamCandidate {
        int index;
        std::string name;
        ClassInfo* type;
    };
    std::unordered_map<const FunctionDecl*, std::vector<ParamCandidate>> paramCandidates;
    std::unordered_set<const FunctionDecl*> valueLambdas;      // lambdas that give a value of a value type
    std::unordered_set<const AssignExpr*> builtInPlace;        // `pts[i] = f(...)`, f giving the list's item type
    std::set<std::pair<const FunctionDecl*, std::string>> placeParams;  // parameters changed through as places

    void noteValueParam(const FunctionDecl& d, int index, const Type& t) {
        if (t.kind != Type::Class || t.cls == nullptr || !t.cls->isValue) return;
        auto& list = paramCandidates[&d];
        for (const ParamCandidate& p : list)
            if (p.index == index) return;
        list.push_back(ParamCandidate{index, d.params[static_cast<size_t>(index)], t.cls});
    }

    // An item of a list of values read where it is (GetIndexRef): where nothing the code does
    // may change a list of its type.
    // A field of a value type kept inside its object (M6.6 part 5): one of an object of a class,
    // whose declared type is a value type, not null. Its type, or null for any other field.
    static ClassInfo* insideObject(const ClassInfo* owner, const Type& field) {
        if (owner == nullptr || owner->isValue) return nullptr;
        if (field.kind != Type::Class || field.nullable || field.cls == nullptr || !field.cls->isValue) return nullptr;
        return field.cls;
    }
    std::unordered_map<const Node*, IndexCandidate> fieldCandidates;
    std::unordered_set<const Node*> readAtOnce;  // calls whose value is read at once, by a field of it
    bool readsFieldInPlace(const Node& at) {
        if (readAtOnce.count(&at) != 0) return true;
        auto found = fieldCandidates.find(&at);
        return found != fieldCandidates.end() && !mayChangeLists(found->second.key, found->second.type);
    }

    bool readsInPlace(const IndexExpr& i) {
        auto found = indexCandidates.find(&i);
        return found != indexCandidates.end() && !mayChangeLists(found->second.key, found->second.type);
    }

    // A for loop's variable the item itself, where the list keeps values: where nothing the code
    // does may change a list of its type, or where the variable's one use is to be passed on
    // (`for (item in this) action(item)`), the function it is passed to seeing to itself.
    bool loopsInPlace(const ForInStmt& s) {
        auto found = loopCandidates.find(&s);
        if (found == loopCandidates.end()) return false;
        const LoopCandidate& loop = found->second;
        ClassInfo* type = nullptr;
        if (loop.item.kind == Type::Class && loop.item.cls != nullptr && loop.item.cls->isValue) type = loop.item.cls;
        else if (loop.item.kind != Type::TypeParam && loop.item.known()) return false;  // never a value
        if (loop.uses.size() == 1 && loop.argUses.size() == 1) return true;
        return type != nullptr && !mayChangeLists(loop.keptPast ? loop.key : static_cast<const void*>(&s), type);
    }

    // The parameters a function takes a copy of as it starts, should it have been given an item
    // of a list where it is: those of a value type whose lists it may change, but for one it
    // changes through as a place, where the change has to go to the item.
    std::vector<int> paramsOwned(const FunctionDecl& d) {
        std::vector<int> owned;
        auto found = paramCandidates.find(&d);
        if (found == paramCandidates.end()) return owned;
        for (const ParamCandidate& p : found->second)
            if (mayChangeLists(&d, p.type) && placeParams.count({&d, p.name}) == 0) owned.push_back(p.index);
        return owned;
    }

    // A mutating method called on a value: what it is called on must be a place, which keeps
    // the change; called by its bare name, the method around must change `this` too.
    void checkMutatingCall(const FuncInfo* f, const Node& at) {
        if (silent > 0 || f->decl == nullptr || !f->decl->isMutating || f->owner == nullptr) return;
        const CallExpr* call = methodCall;
        if (call == nullptr || static_cast<const Node*>(call) != &at) return;
        if (call->callee->kind == ExprKind::Member) {
            const auto& m = static_cast<const MemberExpr&>(*call->callee);
            if (isPlace(*m.object)) {
                notePlaceChange(*m.object, f->owner);
                return;
            }
            report(at, "'" + f->name + "' is mutating: it changes the value of " + f->owner->name + " it is called on, and this one " +
                           "is kept nowhere that could take the change (a var, a var field or an item of a list), so the change " +
                           "would be lost");
        } else if (changesThis() != f->owner) {
            report(at, "'" + f->name + "' is mutating: it can be called by its bare name only from a method that is mutating too");
        }
    }

    // A grid's insertRows and the like give a new grid and leave the one they are called on as
    // it was (D155): one whose result is left unused looks like a change and is not, so it is an
    // error saying how to keep it.
    void unusedCopy(const Expr& e) {
        if (silent > 0 || e.kind != ExprKind::Call) return;
        const auto& call = static_cast<const CallExpr&>(e);
        if (call.callee->kind != ExprKind::Member) return;
        const auto& m = static_cast<const MemberExpr&>(*call.callee);
        static const std::unordered_set<std::string> copies = {"insertRows", "insertColumns", "prependRows", "appendRows",
                                                               "prependColumns", "appendColumns", "removeRows", "removeColumns"};
        if (copies.count(m.name) == 0) return;
        const Type on = withNull(typeQuietly(*m.object), false);
        if (on.kind != Type::Class || on.cls == nullptr || on.cls->name != "Grid" || on.cls->unit == nullptr || !on.cls->unit->prelude) return;
        std::string target = "grid";
        if (m.object->kind == ExprKind::Name) target = static_cast<const NameExpr&>(*m.object).name;
        else if (m.object->kind == ExprKind::Member) target = static_cast<const MemberExpr&>(*m.object).name;
        report(call, "'" + m.name + "' gives a new grid and leaves '" + target + "' as it was: write " + target + " .= " + m.name + "(...) (D155)");
    }

    // The targets of the `.=` being checked, innermost last: what CurrentExpr stands for.
    std::vector<Type> currentTargets;

    Type assign(const AssignExpr& a) {
        atomicAssign(a);
        checkValueFieldWrite(*a.target);
        if (a.target->kind == ExprKind::Index) {
            const Type list = typeQuietly(*static_cast<const IndexExpr&>(*a.target).object);
            noteListChange(list);
            // `pts[i] = pts[i].moved(1)`: the result may be built in the item (M6.6 part 4).
            if (a.op == TokenType::Eq && a.value->kind == ExprKind::Call && list.kind == Type::List && list.args.size() == 1 &&
                list.args[0].kind == Type::Class && !list.args[0].nullable && list.args[0].cls != nullptr && list.args[0].cls->isValue &&
                silent == 0)
                builtInPlace.insert(&a);
        }
        Type target = unknownType();
        std::string targetName;
        Local* local = nullptr;
        if (a.target->kind == ExprKind::Name) {
            const auto& n = static_cast<const NameExpr&>(*a.target);
            targetName = n.name;
            local = findLocal(n.name);
            target = local != nullptr ? local->type : name(n);
            if (local != nullptr) noteKeptPastLoops(n.name, local->type);
            // The name of a class, an object or a function is never given a value; the compiler
            // says so, which tells more than what the value's type is.
            if (local == nullptr) {
                const bool global = meansGlobal(n);
                const GlobalSym* g = global ? findGlobal(*unit, n.name) : nullptr;
                if (localClass(n.name) != nullptr || (g != nullptr && g->var == nullptr)) target = unknownType();
                if (g != nullptr && g->var == nullptr && !g->funcs.empty())
                    report(*a.target, "'" + n.name + "' is a function, and the name of a function is never given another value; " +
                                          "to change what is called, keep it in a var of a function type");
            }
        } else if (a.target->kind == ExprKind::Member) {
            const auto& m = static_cast<const MemberExpr&>(*a.target);
            targetName = m.name;
            target = assignedMember(m);
        } else if (a.target->kind == ExprKind::Index) {
            const auto& ix = static_cast<const IndexExpr&>(*a.target);
            const Type holder = typeQuietly(*ix.object);
            if (holder.frozen) report(*a.target, frozenChange(holder));
            else if (readOnly(holder)) report(*a.target, readOnlyChange(holder, *ix.object));
            else if (holder.kind == Type::Map && a.op == TokenType::Eq) noteKey(*ix.index, typeQuietly(*ix.index));
            target = index(ix);
        } else {
            expr(*a.target);
        }
        // A value kept inside an object given another (M6.6 part 5): a change to values of its type;
        // and `comp.bounds = Rect(...)` may build the new one there.
        ClassInfo* inside = nullptr;
        if (a.target->kind == ExprKind::Member && !static_cast<const MemberExpr&>(*a.target).safe) {
            const auto& m = static_cast<const MemberExpr&>(*a.target);
            const Type owner = typeQuietly(*m.object);
            if (owner.kind == Type::Class && owner.cls != nullptr)
                if (FieldInfo* f = fieldNamed(owner.cls, m.name)) inside = insideObject(owner.cls, fieldType(f));
        } else if (a.target->kind == ExprKind::Name && local == nullptr && ctx != nullptr && ctx->cls != nullptr && !ctx->isStatic) {
            if (FieldInfo* f = fieldNamed(ctx->cls, targetName)) inside = insideObject(ctx->cls, fieldType(f));
        }
        if (inside != nullptr) {
            effects[effectKeyHere()].types.insert(inside);
            if (a.op == TokenType::Eq && a.value->kind == ExprKind::Call && silent == 0) builtInPlace.insert(&a);
        }
        Type value;
        if (a.op == TokenType::DotEq) {
            // `target .= member`: the member asked of the target as it is (D156).
            currentTargets.push_back(target);
            value = expr(*a.value);
            currentTargets.pop_back();
        } else {
            value = exprExpected(*a.value, a.op == TokenType::Eq ? target : unknownType());
        }
        if (local != nullptr) {
            forget(targetName);
            // `next = Dog("Fido")`: from here on it is that value, not null.
            if (a.op == TokenType::Eq && target.nullable && value.known() && !value.nullable && value.kind != Type::Null &&
                value.kind != Type::Nothing && fits(value, target) && !narrowed.empty())
                narrowed.back()[targetName] = withNull(target, false);
        }
        if (target.unknown()) return Type::of(Type::Unit);
        Type result = value;
        if (a.op != TokenType::Eq && a.op != TokenType::DotEq) {
            TokenType op;
            switch (a.op) {
                case TokenType::PlusEq: op = TokenType::Plus; break;
                case TokenType::MinusEq: op = TokenType::Minus; break;
                case TokenType::StarEq: op = TokenType::Star; break;
                case TokenType::SlashEq: op = TokenType::Slash; break;
                case TokenType::PercentEq: op = TokenType::Percent; break;
                case TokenType::UshrEq: op = TokenType::Ushr; break;
                default: return Type::of(Type::Unit);
            }
            if (target.unknown() || value.unknown()) return Type::of(Type::Unit);
            // a?.count += 1 does nothing when a is null, as with ++
            if (target.nullable && safeChain(*a.target)) target = withNull(target, false);
            if (!nullablePlace(*a.target, target, opText(op) + "=", a.nullSafe, a)) target = withNull(target, false);
            else return Type::of(Type::Unit);
            const Type left = target.kind == Type::Byte ? Type::of(Type::Int) : readType(target);
            const Type right = value.kind == Type::Byte ? Type::of(Type::Int) : value;
            if (op == TokenType::Plus && (target.kind == Type::String || value.kind == Type::String)) result = Type::of(Type::String);
            else if (left.kind == Type::Char || right.kind == Type::Char) result = charArithmetic(op, left, right);
            else if (isNumeric(left) && isNumeric(right)) result = arithmetic(left, right);
            else return Type::of(Type::Unit);
            if (result.unknown()) {
                report(a, "'" + opText(op) + "=' cannot be used on " + describe(target) + " and " + describe(value));
                return Type::of(Type::Unit);
            }
        }
        if (!fits(result, target)) {
            std::string who = targetName.empty() ? "this place" : "'" + targetName + "'";
            if (a.op == TokenType::Eq)
                report(*a.value, who + " is " + describe(target) + "; it cannot be given " + describe(result));
            else if (a.op == TokenType::DotEq)
                report(*a.value, who + " is " + describe(target) + "; '.=' would give it " + describe(result));
            else
                report(a, who + " is " + describe(target) + "; this makes it " + describe(result));
        }
        return Type::of(Type::Unit);
    }

    // ---- Members and indexes ----

    // `obj.name`, not called.
    // `++`, `+=` and the rest on a place whose value may be null (L118): the plain form is an
    // error, saying what to write instead, and the `?` form is for such places only. Gives
    // whether the place's type is still to be taken as it is (true) or without its null.
    bool nullablePlace(const Expr& place, const Type& t, const std::string& op, bool nullSafe, const Node& at) {
        if (!t.known() || t.kind == Type::TypeParam) return false;
        const std::string written = place.kind == ExprKind::Name ? "'" + placeText(place) + "'" : placeText(place);
        if (nullSafe) {
            if (!t.nullable)
                report(at, written + " is " + describe(t) + ", never null; use " + op);
            return false;
        }
        if (!t.nullable) return false;
        const bool entry = place.kind == ExprKind::Index && !static_cast<const IndexExpr&>(place).defaultValue;
        if (entry) {
            const auto& ix = static_cast<const IndexExpr&>(place);
            report(at, written + " may be missing, so " + op + " cannot change it; use " + op + "? to skip it when missing, " +
                           placeText(*ix.object) + "[" + placeText(*ix.index) + ", default = 0] to start it from a value, or ?:");
        } else {
            report(at, written + " may be null, so " + op + " cannot change it; use " + op + "? to skip it when null, or ?:");
        }
        return true;
    }

    // How a place reads in a message: `stock["apples"]`, `box.count`, `count`.
    std::string placeText(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Name: return static_cast<const NameExpr&>(e).name;
            case ExprKind::This: return "this";
            case ExprKind::Member: {
                const auto& m = static_cast<const MemberExpr&>(e);
                return placeText(*m.object) + (m.safe ? "?." : ".") + m.name;
            }
            case ExprKind::Index: {
                const auto& ix = static_cast<const IndexExpr&>(e);
                return placeText(*ix.object) + "[" + placeText(*ix.index) + "]";
            }
            case ExprKind::Literal: {
                const auto& l = static_cast<const LiteralExpr&>(e);
                if (l.type == LiteralExpr::Type::String) return "\"" + l.stringValue + "\"";
                if (l.type == LiteralExpr::Type::Int) return std::to_string(l.intValue);
                return "...";
            }
            default:
                return "...";
        }
    }

    // The member an assignment is to. Every field is declared (R24), so assigning one a class does
    // not have is said in those words.
    Type assignedMember(const MemberExpr& m) {
        if (m.object->kind == ExprKind::This || m.object->kind == ExprKind::Name) {
            silent++;
            Type obj = expr(*m.object);
            silent--;
            Type plain = withNull(obj, false);
            if (plain.kind == Type::Class && plain.cls != nullptr && !lookupMember(plain, m.name).found() &&
                findExtensions(plain, m.name).empty() && reportMissing(plain.cls, m.name)) {
                expr(*m.object);
                const std::string io = preludeAdvice(plain.cls, m.name);
                report(m, !io.empty() ? io : plain.cls->name + " has no field '" + m.name + "'; fields must be declared in the class with val or var");
                return unknownType();
            }
        }
        return member(m);
    }

    // The name a test on a value that cannot change is noted under: a val local or parameter,
    // `this`, or a val field of one of those without a getter (`event.letter`), as Kotlin's
    // smart casts allow. Empty for anything else.
    std::string stablePath(const Expr& e) {
        if (e.kind == ExprKind::Name) {
            const auto& n = static_cast<const NameExpr&>(e);
            if (Local* l = findLocal(n.name)) return l->isVal ? n.name : std::string();
            // A val field of the object of the method, named bare: this.name.
            bool escape = false;
            if (onReceivers(n.name, escape) != nullptr || escape) return {};
            if (ctx == nullptr || ctx->cls == nullptr || ctx->isStatic) return {};
            Member mem = lookupMember(selfType(ctx->cls), n.name);
            if (mem.field == nullptr || !mem.field->isVal || mem.getter != nullptr) return {};
            return thisKey() + "." + n.name;
        }
        if (e.kind == ExprKind::This) return thisKey();
        if (e.kind != ExprKind::Member) return {};
        const auto& m = static_cast<const MemberExpr&>(e);
        if (m.safe) return {};
        const std::string base = stablePath(*m.object);
        if (base.empty()) return {};
        silent++;
        Type obj = expr(*m.object);
        silent--;
        Member mem = lookupMember(withNull(obj, false), m.name);
        if (mem.field == nullptr || !mem.field->isVal || mem.getter != nullptr) return {};
        return base + "." + m.name;
    }

    // Whether any test in reach has noted something under a path (see stablePath).
    bool pathsNarrowed() const {
        for (const auto& n : narrowed)
            for (const auto& [name, t] : n)
                if (name.find('.') != std::string::npos) return true;
        return false;
    }

    Type member(const MemberExpr& m) {
        if (pathsNarrowed()) {
            const std::string key = stablePath(m);
            if (!key.empty())
                for (auto it = narrowed.rbegin(); it != narrowed.rend(); ++it) {
                    auto f = it->find(key);
                    if (f != it->end()) return f->second;
                }
        }
        // `ui.Button` after `import jade as ui`, or `Outer.Inner`, or `Color.red`.
        if (m.object->kind == ExprKind::Name) {
            const std::string& base = static_cast<const NameExpr&>(*m.object).name;
            if (findLocal(base) == nullptr && !nameIsMember(base) && classNamed(base) == nullptr) {
                // `int.maxValue` and the other limits of Astra's numbers (D76): constants.
                if (base == "int" || base == "float") {
                    ConstValue limit;
                    if (numberLimit(base, m.name, limit)) {
                        if (silent == 0) builtinConstants[&m] = limit;
                        return Type::of(limit.kind == ConstValue::Int ? Type::Int : Type::Float);
                    }
                    report(m, base + " has no limit called '" + m.name + "'; it has " +
                                  (base == "int" ? "maxValue and minValue" : "maxValue, minValue, minPositive, nan, positiveInfinity and negativeInfinity"));
                    return unknownType();
                }
                // Kotlin's holders, whose numbers are not Astra's (D76).
                if ((base == "Int" || base == "Long" || base == "Double" || base == "Float") && findGlobal(*unit, base) == nullptr) {
                    report(m, oldLimitAdvice(base, m.name));
                    return unknownType();
                }
            }
            if (findLocal(base) == nullptr && !nameIsMember(base)) {
                if (Namespace* ns = findAlias(*unit, base)) {
                    if (const GlobalSym* g = findIn(ns, m.name)) {
                        if (g->var) return fieldType(noteConst(m, g->var));
                        if (g->cls) return g->cls->isObject ? classType(g->cls) : unknownType();
                        if (g->funcs.size() == 1) return functionValue(g->funcs[0]);
                    }
                    return unknownType();
                }
                if (ClassInfo* k = classNamed(base)) {
                    if ((m.name == "className" || m.name == "class") && k->statics.count(m.name) == 0)
                        report(m, base + "." + m.name + " is written " + base + "#" + (m.name == "className" ? ".name" : "") +
                                      ": `#` gives a class (D48)");
                    return staticMember(k, m);
                }
            }
        }
        Type obj = expr(*m.object);
        Type result = memberOf(obj, m.name, m, m.safe);
        if (obj.kind == Type::Class && obj.cls != nullptr) {
            if (FieldInfo* f = fieldNamed(obj.cls, m.name)) {
                if (ClassInfo* t = insideObject(obj.cls, fieldType(f))) fieldCandidates[&m] = IndexCandidate{effectKey(), t};
                // `node.sizes().pref`: what the call gives is read at once, with nothing run in
                // between, so it may be where the function found it (M6.6 part 5).
                if (obj.cls->isValue && m.object->kind == ExprKind::Call) readAtOnce.insert(m.object.get());
            }
        }
        return result;
    }

    // The limits of Astra's numbers (D76): `int.maxValue` and the rest, at their real values.
    bool numberLimit(const std::string& type, const std::string& name, ConstValue& limit) {
        limit = ConstValue{};
        if (type == "int") {
            if (name == "maxValue") limit = constInt(std::numeric_limits<int64_t>::max());
            else if (name == "minValue") limit = constInt(std::numeric_limits<int64_t>::min());
        } else {
            if (name == "maxValue") limit = constFloat(std::numeric_limits<double>::max());
            else if (name == "minValue") limit = constFloat(-std::numeric_limits<double>::max());
            else if (name == "minPositive") limit = constFloat(std::numeric_limits<double>::denorm_min());
            else if (name == "nan") limit = constFloat(std::numeric_limits<double>::quiet_NaN());
            else if (name == "positiveInfinity") limit = constFloat(std::numeric_limits<double>::infinity());
            else if (name == "negativeInfinity") limit = constFloat(-std::numeric_limits<double>::infinity());
        }
        return limit.kind != ConstValue::None;
    }
    // What `Int.MAX_VALUE`, `Double.MIN_VALUE` and the like are in Astra.
    static std::string oldLimitAdvice(const std::string& holder, const std::string& name) {
        // MAX_VALUE as Kotlin writes it, or maxValue as Astra's holders had it.
        const bool screaming = name.find('_') != std::string::npos || name == "NaN";
        std::string camel = screaming ? "" : name;
        bool up = false;
        for (char c : screaming ? name : std::string()) {
            if (c == '_') {
                up = true;
            } else {
                camel += up ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                up = false;
            }
        }
        const bool whole = holder == "Int" || holder == "Long";
        std::string to = (whole ? "int." : "float.") + camel;
        if (!whole && camel == "minValue") to = "float.minPositive (Kotlin's " + holder + ".MIN_VALUE is the smallest above 0; float.minValue is the most negative)";
        std::string text = holder + " is Kotlin's; Astra's numbers are int and float: write " + to;
        if (holder == "Int" && (camel == "maxValue" || camel == "minValue"))
            text += ", which is " + std::string(camel == "maxValue" ? "9223372036854775807" : "-9223372036854775808") + ", an int being 64 bits";
        return text;
    }
    std::unordered_map<const Node*, ConstValue> builtinConstants;  // `int.maxValue` and the like

    // `Image#`, `ui.Button#`, `Box<int>#`, `int#`: the class or built-in type a `#` names (D48).
    std::unordered_map<const Expr*, Type> namedClasses;

    // The type `value#` names, when `value` is a class's or a built-in type's name, not a value.
    bool typeNamedBy(const ClassOfExpr& c, Type& found) {
        std::string dotted;
        const Expr* at = c.value.get();
        while (at->kind == ExprKind::Member) {
            const auto& m = static_cast<const MemberExpr&>(*at);
            if (m.safe) return false;
            dotted = "." + m.name + dotted;
            at = m.object.get();
        }
        if (at->kind != ExprKind::Name) return false;
        const std::string& first = static_cast<const NameExpr&>(*at).name;
        if (findLocal(first) != nullptr || nameIsMember(first)) return false;
        if (const GlobalSym* g = findGlobal(*unit, first); g != nullptr && (g->var != nullptr || !g->funcs.empty())) return false;
        TypeAnnotation written;
        written.line = c.line;
        written.col = c.col;
        written.name = first + dotted;
        written.args = c.typeArgs;
        silent++;
        Type t = resolve(written, *unit);
        silent--;
        if (t.kind == Type::Class && t.cls != nullptr && t.cls->isObject) return false;  // the name of the object
        if (!t.known() || t.kind == Type::Unit || t.kind == Type::Nothing) return false;
        found = resolve(written, *unit);  // reported this time
        return true;
    }

    Type classOfExpr(const ClassOfExpr& c) {
        Type named;
        if (typeNamedBy(c, named)) {
            if (named.kind == Type::Class && !c.typeArgs.empty() && c.typeArgs.size() != named.cls->typeParams.size())
                report(c, named.cls->name + " takes " + std::to_string(named.cls->typeParams.size()) + " type argument" +
                              (named.cls->typeParams.size() == 1 ? "" : "s") + ", not " + std::to_string(c.typeArgs.size()));
            if (named.kind == Type::Class && c.typeArgs.empty()) named.args.clear();  // `Box#`: what it holds not said
            if (silent == 0) namedClasses[&c] = named;
        } else {
            if (!c.typeArgs.empty()) report(c, "type arguments go with the name of a class, as in Box<int>#; a value's class has its own");
            const Type value = expr(*c.value);
            if (value.nullable || value.kind == Type::Null)
                report(c, placeText(*c.value) + " may be null, which is of no class: write " + placeText(*c.value) +
                              "!!#, or test it first (D48)");
        }
        return stdClassType("Class");
    }

    // Whether a bare name is a member of something the code runs on, rather than a class.
    bool nameIsMember(const std::string& base) {
        bool escape = false;
        if (onReceivers(base, escape) != nullptr || escape) return true;
        if (ctx != nullptr && ctx->cls != nullptr && !ctx->isStatic) {
            Member m = lookupMember(selfType(ctx->cls), base);
            if (m.field || m.getter) return true;
        }
        return false;
    }

    // A class named by a bare name in the code (not an object, whose name is its instance).
    ClassInfo* classNamed(const std::string& base) {
        if (ClassInfo* k = localClass(base)) return k->isObject ? nullptr : k;
        for (ClassInfo* c = classAround(); c != nullptr; c = c->outer) {
            if (c->decl->name == base && !c->isObject) return c;
            auto n = c->nested.find(base);
            if (n != c->nested.end()) return n->second->isObject ? nullptr : n->second;
        }
        const GlobalSym* g = findGlobal(*unit, base);
        if (g != nullptr && g->cls != nullptr && !g->cls->isObject && g->var == nullptr && g->funcs.empty()) return g->cls;
        return nullptr;
    }

    Type staticMember(ClassInfo* k, const MemberExpr& m) {
        auto s = k->statics.find(m.name);
        if (s != k->statics.end()) return fieldType(noteConst(m, s->second));
        auto n = k->nested.find(m.name);
        if (n != k->nested.end()) return n->second->isObject ? classType(n->second) : unknownType();
        auto sm = k->staticMethods.find(m.name);
        if (sm != k->staticMethods.end() && sm->second.size() == 1) return functionValue(sm->second[0]);
        // An old name of the standard library's, Font.bold and the like, is said; other names the
        // checker cannot yet rule out (static properties with getters are not kept here).
        if (sm == k->staticMethods.end() && !hasStatic(k, m.name))
            if (const std::string advice = preludeAdvice(k, m.name); !advice.empty()) report(m, advice);
        return unknownType();
    }

    // Whether a class or one above it has a static member, a nested class or an entry of the name.
    static bool hasStatic(const ClassInfo* k, const std::string& name) {
        for (const ClassInfo* c = k; c != nullptr; c = c->superclass) {
            if (c->statics.count(name) != 0 || c->nested.count(name) != 0 || c->staticMethods.count(name) != 0) return true;
            if (c->isEnum && (name == "entries" || name == "valueOf" || name == "values")) return true;
        }
        return false;
    }

    // Reports a value that may be null used as if it could not; true if it was.
    bool nullUse(const Type& obj, const std::string& name, const Node& at, bool safe, bool called) {
        if (!obj.nullable || safe || obj.unknown()) return false;
        if (called)
            report(at, describe(obj) + " may be null, so ." + name + "() cannot be called on it; use ?." + name +
                           "(), or check it first");
        else
            report(at, describe(obj) + " may be null, so ." + name + " cannot be used on it; use ?." + name + ", or check it first");
        return true;
    }

    // A union's members are those every type of it has (D40): the union as the nearest type
    // above all of them, with an error where there is none.
    bool unionMember(Type& obj, const std::string& name, const Node& at) {
        if (obj.kind != Type::Union) return true;
        Type all = unionCommon(withNull(obj, false));
        if (all.unknown() || all.kind == Type::Union) {
            if (builtinMember(name)) {
                obj = withNull(Type::of(Type::Any), obj.nullable);
                return true;
            }
            report(at, "'" + name + "' is not something every type of " + text(withNull(obj, false)) + " has: test which it is first, " +
                           "with is or a when (D40)");
            return false;
        }
        obj = withNull(all, obj.nullable);
        return true;
    }

    // ---- Atomic fields (D92) ----
    // An `atomic var` field is changed in one step, by any core of a parallel section, through
    // `+=`, `-=`, `= max(field, v)`, `= min(field, v)` and `field.compareAndSet(expected, v)`;
    // each is noted for the compiler, which makes it the one operation. Plain assignment to one
    // inside a section is an error.
    struct AtomicOp {
        uint8_t kind = 0;               // 0 add, 1 subtract, 2 max, 3 min, 4 compare and set
        const Expr* object = nullptr;   // null: the object the code runs on
        std::string field;
        const Expr* operand = nullptr;
        const Expr* expected = nullptr;  // compare and set
    };
    std::unordered_map<const Expr*, AtomicOp> atomicOps;

    // The atomic field a place is, if it is one: `obj.hits`, or `hits` in the class's own code.
    FieldInfo* atomicFieldAt(const Expr& place, const Expr*& object) {
        object = nullptr;
        FieldInfo* f = nullptr;
        if (place.kind == ExprKind::Member) {
            const auto& m = static_cast<const MemberExpr&>(place);
            silent++;
            const Type t = expr(*m.object);
            silent--;
            if (t.kind == Type::Class && t.cls != nullptr && !m.safe) f = fieldNamed(t.cls, m.name);
            object = m.object.get();
        } else if (place.kind == ExprKind::Name && ctx != nullptr && ctx->cls != nullptr && !ctx->isStatic &&
                   findLocal(static_cast<const NameExpr&>(place).name) == nullptr) {
            Member m = lookupMember(selfType(ctx->cls), static_cast<const NameExpr&>(place).name);
            f = m.field;
        }
        return f != nullptr && f->decl != nullptr && f->decl->isAtomic ? f : nullptr;
    }

    // Whether the code being checked runs in a parallel section: a lambda given to one, or one
    // inside such a lambda.
    bool inParallelSection() const {
        for (const FnCtx* c = ctx; c != nullptr && c->isLambda; c = c->parent) {
            auto site = c->placeDecl != nullptr ? lambdaSites.find(c->placeDecl) : lambdaSites.end();
            if (site != lambdaSites.end() && site->second.noWait == ParallelSection) return true;
        }
        return false;
    }

    // `x.hits += 1`, `best = max(best, v)`: noted as the one operation it is, where it is one.
    void atomicAssign(const AssignExpr& a) {
        const Expr* object = nullptr;
        FieldInfo* f = atomicFieldAt(*a.target, object);
        if (f == nullptr) return;
        AtomicOp op;
        op.object = object;
        op.field = f->name;
        op.operand = a.value.get();
        if (a.op == TokenType::PlusEq || a.op == TokenType::MinusEq) {
            op.kind = a.op == TokenType::PlusEq ? 0 : 1;
        } else if (a.op == TokenType::Eq && a.value->kind == ExprKind::Call) {
            const auto& c = static_cast<const CallExpr&>(*a.value);
            std::string callee;
            if (c.callee->kind == ExprKind::Name) callee = static_cast<const NameExpr&>(*c.callee).name;
            else if (c.callee->kind == ExprKind::Member) callee = placeText(*c.callee);
            const bool max = callee == "maxOf" || callee == "max" || callee == "Math.max";
            const bool min = callee == "minOf" || callee == "min" || callee == "Math.min";
            if (!(max || min) || c.args.size() != 2 || placeText(*c.args[0]) != placeText(*a.target)) {
                if (inParallelSection())
                    report(a, "'" + f->name + "' is an atomic var: in a parallel section it changes through +=, -=, = max(" + f->name + ", v), = min(" + f->name +
                                  ", v) or " + f->name + ".compareAndSet(expected, v) (D92)");
                return;
            }
            op.kind = max ? 2 : 3;
            op.operand = c.args[1].get();
        } else {
            if (inParallelSection())
                report(a, "'" + f->name + "' is an atomic var: in a parallel section it changes through +=, -=, = max(" + f->name + ", v), = min(" + f->name +
                              ", v) or " + f->name + ".compareAndSet(expected, v) (D92)");
            return;
        }
        if (silent == 0) atomicOps[&a] = op;
    }

    // `x.hits.compareAndSet(expected, v)`: true where it set the field. Unknown where the call is
    // not one.
    bool atomicCompareAndSet(const CallExpr& c, Type& result) {
        if (c.callee->kind != ExprKind::Member || static_cast<const MemberExpr&>(*c.callee).name != "compareAndSet" || c.args.size() != 2) return false;
        const Expr* object = nullptr;
        FieldInfo* f = atomicFieldAt(*static_cast<const MemberExpr&>(*c.callee).object, object);
        if (f == nullptr) return false;
        const Type held = fieldType(f);
        for (const ExprPtr& arg : c.args) {
            const Type given = expr(*arg);
            if (held.known() && given.known() && !fits(given, held)) report(*arg, "'" + f->name + "' holds " + describe(held) + ", not " + describe(given));
        }
        AtomicOp op;
        op.kind = 4;
        op.object = object;
        op.field = f->name;
        op.expected = c.args[0].get();
        op.operand = c.args[1].get();
        if (silent == 0) atomicOps[&c] = op;
        result = Type::of(Type::Bool);
        return true;
    }

    // `5.seconds`: an extension property (`val int.seconds : Duration get() => ...`), read where
    // the type has no member of the name. Noted for the compiler, which calls its getter.
    std::unordered_map<const Expr*, FuncInfo*> extensionGetters;
    bool extensionProperty(const Type& plain, const std::string& name, const MemberExpr& at, Type& result) {
        for (FuncInfo* f : findExtensions(plain, name)) {
            if (f->stmt == nullptr || !f->stmt->isProperty) continue;
            result = resultOf(f);
            if (silent == 0) extensionGetters[&at] = f;
            noteCallThrows(f, nullptr, noArgs(), nullptr, at);
            return true;
        }
        return false;
    }

    Type memberOf(const Type& objIn, const std::string& name, const MemberExpr& at, bool safe) {
        Type obj = objIn;
        if (!unionMember(obj, name, at)) return unknownType();
        if (obj.unknown()) return unknownType();
        if (obj.kind == Type::Nothing) return Type::of(Type::Nothing);
        if (obj.kind == Type::Null) return safe ? Type::of(Type::Null) : unknownType();
        if (nullUse(obj, name, at, safe, false)) return unknownType();
        if (obj.kind == Type::TypeParam) {
            if (obj.bound == nullptr) return unknownType();
            obj = *obj.bound;
        }
        if (obj.kind == Type::Any) {
            report(at, "'" + name + "' cannot be used on a value of type any; cast it first (x as Type) or test it (if (x is Type))");
            return unknownType();
        }
        Type result = unknownType();
        Type plain = withNull(obj, false);
        if (obj.kind == Type::Class && obj.cls != nullptr) {
            ClassInfo* k = obj.cls;
            Member m = lookupMember(plain, name);
            noteKeyMember(m, name);
            // An object's constant is folded where the object is named, so nothing is skipped.
            const bool named = at.object->kind == ExprKind::Name || at.object->kind == ExprKind::This;
            if (m.field) {
                if (named) noteConst(at, m.field);
                result = subst(fieldType(m.field), m.env);
            } else if (m.getter) {
                noteCallThrows(m.getter, nullptr, noArgs(), nullptr, at);
                result = subst(resultOf(m.getter), m.env);
            } else if (m.setter) {
                resolveFunc(m.setter);
                result = m.setter->params.empty() ? unknownType() : subst(m.setter->params[0], m.env);
            } else if (m.methods.size() == 1) {
                result = subst(functionValue(m.methods[0]), m.env);
            } else if (m.methods.empty()) {
                auto st = k->statics.find(name);
                if (st != k->statics.end()) result = fieldType(named ? noteConst(at, st->second) : st->second);
                else if (k->isEnum && name == "ordinal") result = Type::of(Type::Int);
                else if (k->isEnum && name == "name") result = Type::of(Type::String);
                else if (extensionProperty(plain, name, at, result)) {}
                else if (findExtensions(plain, name).empty() && reportMissing(k, name)) {
                    const std::string io = preludeAdvice(k, name);
                    report(at, !io.empty() ? io
                               : name == "length" && (k->unit != nullptr && k->unit->prelude) ? goneAdvice(name)
                               : name == "class" ? k->name + " has no member 'class'; the class of an object is written x# (D48)"
                                                 : k->name + " has no member '" + name + "'");
                }
            }
        } else if (!builtinProperty(obj, name, result) && !extensionProperty(plain, name, at, result)) {
            if (name == "length" && builtinDeclared(plain))
                report(at, goneAdvice(name));
            else if ((obj.kind == Type::List || obj.kind == Type::String || obj.kind == Type::Map || obj.kind == Type::Bytes) &&
                     (name == "size" || name == "lastIndex"))
                result = Type::of(Type::Int);
        }
        return safe ? withNull(result) : result;
    }

    Type index(const IndexExpr& i) {
        Type obj = expr(*i.object);
        Type idx = expr(*i.index);
        std::vector<Type> rest;
        for (const ExprPtr& m : i.more) rest.push_back(expr(*m));
        if (!i.more.empty() && obj.known() && !obj.nullable && obj.kind != Type::Class && obj.kind != Type::TypeParam) {
            report(*i.more[0], describe(obj) + " takes one index; several, as in [x, y], are for a class with op [](x, y)");
            return unknownType();
        }
        if (!i.defaultValue && obj.kind == Type::List && !obj.nullable && obj.args.size() == 1 && obj.args[0].kind == Type::Class &&
            obj.args[0].cls != nullptr && obj.args[0].cls->isValue)
            indexCandidates[&i] = IndexCandidate{effectKey(), obj.args[0].cls};
        if (i.defaultValue) {
            // `counts[word, default = 0]`: the entry, or the start value; never null (L118).
            const bool map = obj.kind == Type::Map && !obj.nullable;
            const Type value = map && obj.args.size() == 2 ? obj.args[1] : unknownType();
            Type start = exprExpected(*i.defaultValue, value);
            if (obj.known() && !map && obj.kind != Type::TypeParam)
                report(i, "only a map's index takes a default; this is " + describe(obj));
            else if (map && obj.args.size() == 2 && !fits(idx, obj.args[0]))
                report(*i.index, "the keys of this map are " + text(obj.args[0]) + "s; this is " + describe(idx));
            else if (value.known() && !fits(start, value))
                report(*i.defaultValue, "the values of this map are " + describe(value) + "; the default cannot be " + describe(start));
            return value.known() ? withNull(value, false) : unknownType();
        }
        if (obj.unknown() || obj.kind == Type::TypeParam) return unknownType();
        if (obj.nullable) {
            report(i, describe(obj) + " may be null, so it cannot be indexed; check it first");
            return unknownType();
        }
        auto needInt = [&] {
            if (idx.known() && !((idx.kind == Type::Int || idx.kind == Type::Index || idx.kind == Type::Byte) && !idx.nullable) && idx.kind != Type::Nothing &&
                idx.kind != Type::Range && idx.kind != Type::TypeParam)
                report(*i.index, "an index must be an int, not " + describe(idx));
            // A negative number never counts from the end (D146).
            if (idx.kind == Type::Int) {
                const ConstValue at = foldQuietly(*i.index);
                if (at.kind == ConstValue::Int && at.i < 0)
                    report(*i.index, "[" + std::to_string(at.i) + "] is before the start: counting from the end is written [^" +
                                         std::to_string(-at.i) + "]" + (at.i == -1 ? ", the last" : ""));
            }
        };
        switch (obj.kind) {
            case Type::List:
                needInt();
                if (idx.kind == Type::Range) {
                    // A slice is a new list, its maker's to change (D32).
                    Type slice = withNull(obj, false);
                    slice.frozen = false;
                    slice.open = true;
                    return slice;
                }
                return obj.args.empty() ? unknownType() : obj.args[0];
            case Type::String:
                needInt();
                if (idx.kind == Type::Range) return Type::of(Type::String);
                return Type::of(Type::Char);  // s[i] is a char (D24)
            case Type::Bytes:
                needInt();
                if (idx.kind == Type::Range) return Type::of(Type::Bytes);
                return Type::of(Type::Byte);  // an item of Bytes is a byte (D19)
            case Type::Map: {
                // A list that can change finds a frozen one by what it holds (D26).
                Type key = obj.args.size() == 2 ? obj.args[0] : unknownType();
                key.frozen = false;
                if (obj.args.size() == 2 && !fits(idx, key))
                    report(*i.index, "the keys of this map are " + text(obj.args[0]) + "s; this is " + describe(idx));
                return obj.args.size() == 2 ? withNull(obj.args[1]) : unknownType();
            }
            case Type::Set:
                report(i, "a set's items have no positions: first(), last(), or toList()[i] for a list of them");
                return obj.args.empty() ? unknownType() : obj.args[0];
            case Type::Class: {
                std::vector<Type> args{idx};
                args.insert(args.end(), rest.begin(), rest.end());
                return operatorResult(obj, "[]", i, args);
            }
            default:
                return unknownType();
        }
    }

    // ---- Calls ----

    // Where each argument goes: a parameter's index, or -1 for one that fits none.
    struct Binding {
        std::vector<int> paramOf;    // per argument
        std::vector<bool> given;     // per parameter
        bool failed = false;         // an argument fit no parameter: missing ones are not reported
        bool missing = false;        // a parameter without a default was given no value
        // `...style`: the parameter each of the bundle's names goes to, its type, and where (D143).
        std::vector<std::tuple<int, Type, const Expr*, std::string>> bundled;
    };

    Binding bind(FuncInfo* f, const Args& args, const Node& at, const std::string& what) {
        resolveFunc(f);
        Binding b;
        const size_t n = f->params.size();
        b.given.assign(n, false);
        b.paramOf.assign(args.exprs.size(), -1);
        size_t positional = 0;
        const size_t count = args.trailing && !args.exprs.empty() ? args.exprs.size() - 1 : args.exprs.size();
        for (size_t i = 0; i < count; i++) {
            const std::string argName = i < args.names.size() ? args.names[i] : std::string();
            if (argName == "...") {
                // A bundle's names, each going to the parameter of that name, unless the call
                // names it itself, which wins (D143).
                const Type t = withNull(typeQuietly(*args.exprs[i]), false);
                if (t.kind != Type::Bundle) {
                    if (t.known()) report(*args.exprs[i], "'...' passes the arguments of a bundle made with params(...), not " + describe(t));
                    b.failed = !t.known() ? b.failed : true;
                    continue;
                }
                std::vector<std::string> expanded;
                for (size_t k = 0; k < t.fields.size(); k++) {
                    const std::string& name = t.fields[k];
                    if (std::find(args.names.begin(), args.names.end(), name) != args.names.end()) continue;
                    auto found = std::find(f->paramNames.begin(), f->paramNames.end(), name);
                    if (found == f->paramNames.end()) {
                        report(*args.exprs[i], what + " has no parameter '" + name + "', which the bundle gives");
                        b.failed = true;
                        continue;
                    }
                    const size_t p = static_cast<size_t>(found - f->paramNames.begin());
                    if (b.given[p]) {
                        report(*args.exprs[i], "'" + name + "' is given twice: by the bundle as well");
                        b.failed = true;
                    }
                    b.given[p] = true;
                    b.bundled.emplace_back(static_cast<int>(p), t.args[k], args.exprs[i].get(), name);
                    expanded.push_back(name);
                }
                if (silent == 0) bundleExpansions[args.exprs[i].get()] = std::move(expanded);
                continue;
            }
            if (argName.empty()) {
                if (f->vararg >= 0 && positional >= static_cast<size_t>(f->vararg)) {
                    b.paramOf[i] = f->vararg;
                    b.given[static_cast<size_t>(f->vararg)] = true;
                    continue;
                }
                if (positional >= n) {
                    report(*args.exprs[i], what + " takes " + std::to_string(n) + " argument" + (n == 1 ? "" : "s") + ", not more");
                    b.failed = true;
                    break;
                }
                b.paramOf[i] = static_cast<int>(positional);
                b.given[positional] = true;
                positional++;
            } else {
                auto found = std::find(f->paramNames.begin(), f->paramNames.end(), argName);
                if (found == f->paramNames.end()) {
                    report(*args.exprs[i], what + " has no parameter '" + argName + "'");
                    b.failed = true;
                    continue;
                }
                size_t p = static_cast<size_t>(found - f->paramNames.begin());
                if (b.given[p]) {
                    report(*args.exprs[i], "'" + argName + "' is given twice");
                    b.failed = true;
                }
                b.paramOf[i] = static_cast<int>(p);
                b.given[p] = true;
            }
        }
        if (args.trailing && !args.exprs.empty()) {
            // The lambda goes to the last parameter, as in Kotlin (D56).
            const Expr& lambdaArg = *args.exprs.back();
            if (n == 0) {
                report(lambdaArg, what + " takes no arguments, so it cannot take a lambda after its ')'");
                b.failed = true;
            } else {
                const size_t p = n - 1;
                if (b.given[p] && static_cast<int>(p) != f->vararg) {
                    report(lambdaArg, "'" + f->paramNames[p] + "' is given twice");
                    b.failed = true;
                }
                b.paramOf[args.exprs.size() - 1] = static_cast<int>(p);
                b.given[p] = true;
            }
        }
        if (!b.failed && !f->isLambda()) {
            for (size_t p = 0; p < n; p++) {
                if (b.given[p] || f->hasDefault[p] || static_cast<int>(p) == f->vararg) continue;
                b.missing = true;
                if (f->owner != nullptr && f->owner->name == "Channel" && f->owner->unit != nullptr && f->owner->unit->prelude)
                    report(at, "a channel says how much it holds: Channel<T>(capacity = n), with 0 for each send to wait for a receive, or "
                               "Channel<T>.unbounded() (D87)");
                else
                    report(at, what + " needs a value for '" + f->paramNames[p] + "'");
            }
        }
        return b;
    }

    // A call to a declared function with arguments: their types are checked, the function's
    // own type parameters worked out from them (and the receiver, for an extension), and the
    // result given with them filled in. `env` holds what a generic class's parameters stand for.
    Type callWith(FuncInfo* f, const Args& args, const std::vector<TypeAnnotation>& typeArgs, Env env,
                  const Type* receiver, const Node& at, const std::string& what, Env* envOut = nullptr) {
        resolveFunc(f);
        std::vector<std::string> vars = f->typeParams;
        if (f->owner != nullptr && f->name == "init")
            for (const std::string& p : f->owner->typeParams) vars.push_back(p);
        for (size_t i = 0; i < typeArgs.size() && i < f->typeParams.size(); i++) env[f->typeParams[i]] = resolve(typeArgs[i], *unit);
        // What is written at the call (`first<string>(...)`, `Box<string>(...)`) is not worked out again.
        vars.erase(std::remove_if(vars.begin(), vars.end(), [&](const std::string& v) { return env.count(v) > 0; }), vars.end());
        if (receiver != nullptr && f->isExtension) {
            unify(f->receiver, *receiver, env, vars);
            fixByReceiver(f->receiver, env, vars);
        }
        if (args.spread) {
            for (const ExprPtr& a : args.exprs) expr(*a);
            if (envOut != nullptr) *envOut = env;
            noteTypeArguments(f, env, at);
            return finishCall(f, env, vars, at);
        }
        Binding b = bind(f, args, at, what);
        if (f->decl != nullptr && f->decl->isSuspend) noteWait(at, f->name, givesToSequence(f));
        if (f->decl != nullptr && f->decl->isInline && silent == 0) noteInlineCall(f, b, at);
        noteCallThrows(f, nullptr, args, &b, at);
        // Named arguments and a lambda after the call, given to a C++ function, are put where the
        // form chosen has them; a parameter between them that is not given gets a null, which
        // the C++ takes as not given (`names.join(", ") { }`, `items.add(item = x)`).
        const bool named = std::any_of(args.names.begin(), args.names.end(), [](const std::string& n) { return !n.empty(); });
        if (f->isExternal && (args.trailing || named) && !b.failed) {
            int last = -1;
            for (int p : b.paramOf) last = std::max(last, p);
            std::vector<int> order(static_cast<size_t>(last + 1), -1);
            bool clash = false;
            for (size_t i = 0; i < b.paramOf.size(); i++) {
                const int p = b.paramOf[i];
                if (p < 0 || p == f->vararg || order[static_cast<size_t>(p)] >= 0) clash = true;
                else order[static_cast<size_t>(p)] = static_cast<int>(i);
            }
            if (!clash) argumentOrders[&at] = std::move(order);
        }
        std::vector<Type> types(args.exprs.size());
        auto wanted = [&](size_t i) -> Type {
            const int p = b.paramOf[i];
            if (p < 0) return unknownType();
            if (p == f->vararg) return i < args.names.size() && !args.names[i].empty() ? listOf(f->varargItem) : f->varargItem;
            return f->params[static_cast<size_t>(p)];
        };
        // Values first, then lambdas, which take their parameters from what is known by then.
        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 0; i < args.exprs.size(); i++) {
                const bool isLambda = args.exprs[i]->kind == ExprKind::Function;
                if ((pass == 0) == isLambda) continue;
                Type want = wanted(i);
                Type expected = subst(want, env);
                if (isLambda) noteLambdaSite(*args.exprs[i], f, b.paramOf[i]);
                if (args.exprs[i]->kind == ExprKind::Name) {
                    if (Local* l = findLocal(static_cast<const NameExpr&>(*args.exprs[i]).name)) {
                        noteUse(*l, ParamUse::PassedOn, f, b.paramOf[i]);
                        passedUse = args.exprs[i].get();
                    }
                }
                // A function value given to a function that may call it: what it does, done.
                if (want.kind == Type::Function && args.exprs[i]->kind != ExprKind::Function && silent == 0) {
                    const Local* given = args.exprs[i]->kind == ExprKind::Name ? findLocal(static_cast<const NameExpr&>(*args.exprs[i]).name) : nullptr;
                    if (given != nullptr && given->paramOf != nullptr) effects[effectKeyHere()].callsParams = true;
                    else effects[effectKeyHere()].any = true;
                }
                types[i] = exprExpected(*args.exprs[i], expected);
                passedUse = nullptr;
                unify(want, types[i], env, vars);
            }
        }
        // A bundle's arguments, each against the parameter it goes to (D143).
        for (const auto& [p, given, from, name] : b.bundled) {
            unify(f->params[static_cast<size_t>(p)], given, env, vars);
            const Type want = subst(f->params[static_cast<size_t>(p)], env);
            if (!fits(given, want)) report(*from, what + " takes " + describe(want) + " for '" + name + "', and the bundle gives " + describe(given));
        }
        for (size_t i = 0; i < args.exprs.size(); i++) {
            if (b.paramOf[i] < 0) continue;
            Type want = subst(wanted(i), env);
            if (!fits(types[i], want)) {
                const std::string& param = f->paramNames[static_cast<size_t>(b.paramOf[i])];
                const Type plainWant = withNull(want, false);
                if (plainWant.kind == Type::Class && plainWant.cls != nullptr && plainWant.cls->name == "Duration" && plainWant.cls->unit != nullptr &&
                    plainWant.cls->unit->prelude && (types[i].kind == Type::Int || types[i].kind == Type::Float || types[i].kind == Type::Number))
                    report(*args.exprs[i], what + " takes a Duration for '" + param + "': a time says its unit, as 16.milliseconds or 2.seconds (D90)");
                else
                    report(*args.exprs[i], what + " takes " + describe(want) + " for '" + param + "', not " + describe(types[i]));
            }
        }
        if (envOut != nullptr) *envOut = env;
        noteTypeArguments(f, env, at);
        noteNewValueAdded(f, receiver, args, at);
        checkMutatingCall(f, at);
        noteCallEffects(f, receiver, args);
        return finishCall(f, env, vars, at);
    }

    // `pts.add(Point(1, 2))` on a list of a value type: the value can be made where it goes,
    // in the list, with no box of its own (M6.6 part 3, value-lists.html section 7 rule b).
    std::unordered_map<const Node*, ClassInfo*> constructedValues;
    std::unordered_set<const Node*> newValueAdds;

    void noteNewValueAdded(const FuncInfo* f, const Type* receiver, const Args& args, const Node& at) {
        if (!f->isExternal || f->name != "add" || receiver == nullptr || args.exprs.size() != 1 || args.spread) return;
        const Type list = withNull(*receiver, false);
        if (list.kind != Type::List || list.args.size() != 1) return;
        const Type& item = list.args[0];
        if (item.kind != Type::Class || item.nullable || item.cls == nullptr || !item.cls->isValue) return;
        auto made = constructedValues.find(static_cast<const Node*>(args.exprs[0].get()));
        if (made != constructedValues.end() && made->second == item.cls) newValueAdds.insert(&at);
    }

    // A call's result, with what is known of the type parameters filled in; for a function of
    // the C++'s, kept for the compiler (M6.6 part 1), with the function's name as a program
    // writes it.
    struct ExternalCall {
        Type result;
        std::string name;
    };
    std::unordered_map<const Node*, ExternalCall> externalCalls;

    Type finishCall(FuncInfo* f, const Env& env, const std::vector<std::string>& vars, const Node& at) {
        Type result = finish(resultOf(f), env, vars);
        if (f->isExternal) {
            std::string name = f->name;
            if (f->isExtension) name = argText(f->receiver) + "." + name;
            else if (f->owner != nullptr) name = f->owner->name + "." + name;
            externalCalls[&at] = ExternalCall{result, std::move(name)};
        }
        return result;
    }

    // What a call of a generic function or class gives its type parameters, in their order,
    // for the compiler to pass on (M6.6 part 1); a part not worked out is unknown.
    std::unordered_map<const Node*, std::vector<Type>> typeArgumentsAt;

    void noteTypeArguments(const std::vector<std::string>& params, const Env& env, const Node& at) {
        std::vector<Type>& given = typeArgumentsAt[&at];
        given.clear();
        for (const std::string& p : params) {
            auto found = env.find(p);
            given.push_back(found != env.end() ? subst(found->second, env) : unknownType());
        }
    }

    // Only a function written in Astra takes them: the C++'s have none.
    void noteTypeArguments(const FuncInfo* f, const Env& env, const Node& at) {
        if (!f->typeParams.empty() && f->decl != nullptr && !f->isExternal) noteTypeArguments(f->typeParams, env, at);
    }

    // What the object a function is called on says of its type parameters is so: `words.add(1)` on
    // a List<string> does not make T something both fit, as an argument would. Where it says
    // nothing of one (a Map written with no type arguments), that one is unknown: the arguments
    // cannot tell what the object holds, and `data.get(key, "missing")` is not a string? for it
    // (M6.6 part 1, found by ASTRA_CHECK_NATIVE_RESULTS).
    static void fixByReceiver(const Type& declared, Env& env, std::vector<std::string>& vars) {
        std::vector<std::string> named;
        typeParamsIn(declared, named);
        vars.erase(std::remove_if(vars.begin(), vars.end(),
                                  [&](const std::string& v) {
                                      auto found = env.find(v);
                                      if (found != env.end() && found->second.known()) return true;
                                      if (std::find(named.begin(), named.end(), v) == named.end()) return false;
                                      env[v] = unknownType();
                                      return true;
                                  }),
                   vars.end());
    }

    // The names of the type parameters a type is written with, `K` and `V` in `Map<K, List<V>>`.
    static void typeParamsIn(const Type& t, std::vector<std::string>& out) {
        if (t.kind == Type::TypeParam) out.push_back(t.name);
        for (const Type& arg : t.args) typeParamsIn(arg, out);
        if (t.result) typeParamsIn(*t.result, out);
        if (t.receiver) typeParamsIn(*t.receiver, out);
    }

    // A result with what is known of the type parameters filled in, and the rest unknown.
    Type finish(const Type& result, Env env, const std::vector<std::string>& vars) {
        for (const std::string& v : vars)
            if (!env.count(v)) env[v] = unknownType();
        return subst(result, env);
    }

    bool superCall = false;  // the next constructorCall is a subclass's of its superclass

    Type constructorCall(ClassInfo* k, const Args& args, const std::vector<TypeAnnotation>& typeArgs, const Node& at) {
        const std::string what = k->name + "(...)";
        // Java's Font(name, style, size), its style a number: the style is said by name (L80).
        if (k->name == "Font" && k->unit != nullptr && k->unit->prelude && args.exprs.size() == 3 && !args.spread &&
            std::all_of(args.names.begin(), args.names.end(), [](const std::string& n) { return n.empty(); }) &&
            withNull(typeQuietly(*args.exprs[1]), false).kind == Type::Int) {
            for (const ExprPtr& a : args.exprs) expr(*a);
            report(at, "Java's Font(name, style, size) is Font(name, size, bold = true, italic = true) in Astra: a style is said by name (L80)");
            return classType(k);
        }
        // `Action { ... }`: the lambda, as the interface (D28).
        if (k->isInterface && args.exprs.size() == 1 && args.exprs[0]->kind == ExprKind::Function && !args.spread) {
            Type iface = classType(k);
            for (size_t i = 0; i < k->typeParams.size(); i++)
                iface.args.push_back(i < typeArgs.size() ? resolve(typeArgs[i], *unit) : unknownType());
            std::string method;
            Type fn;
            if (samType(iface, method, fn)) {
                const Type made = exprExpected(*args.exprs[0], iface);
                if (silent == 0) samCalls.insert(&at);
                return made;
            }
        }
        if (k->isEnumClass && silent == 0 && !std::exchange(superCall, false))
            report(at, k->name + " is an enum class: it is made only as one of its kinds (D39)");
        superCall = false;
        Env env;
        for (size_t i = 0; i < typeArgs.size() && i < k->typeParams.size(); i++) env[k->typeParams[i]] = resolve(typeArgs[i], *unit);
        FuncInfo* init = k->inits.size() == 1 ? k->inits[0] : (k->headerInit ? headerFunc(k) : nullptr);
        // Secondary constructors: the class's static functions made of them, chosen among with
        // the class's own forms by D56's rules.
        std::vector<FuncInfo*> secondaries;
        for (const auto& [name, fs] : k->staticMethods)
            if (name.rfind("<constructor ", 0) == 0) secondaries.insert(secondaries.end(), fs.begin(), fs.end());
        if (!secondaries.empty() && !args.spread) {
            std::vector<FuncInfo*> forms;
            if (k->headerInit) forms.push_back(headerFunc(k));
            else forms.insert(forms.end(), k->inits.begin(), k->inits.end());
            forms.insert(forms.end(), secondaries.begin(), secondaries.end());
            size_t index = 0;
            const Env none;
            auto envOf = [&](size_t i) -> const Env& { return forms[i]->isStatic ? none : env; };
            FuncInfo* f = chooseForm(forms, envOf, args, typeArgs, nullptr, at, k->name, index);
            if (f == nullptr) {
                for (const ExprPtr& a : args.exprs) expr(*a);
                return classType(k);
            }
            if (f->isStatic) {
                if (silent == 0) secondaryCalls[&at] = f;
                return callWith(f, args, typeArgs, none, nullptr, at, what);
            }
            init = f;
        }
        if (init != nullptr) {
            callWith(init, args, {}, env, nullptr, at, what, &env);
        } else {
            for (const ExprPtr& a : args.exprs) expr(*a);
            if (k->inits.empty() && !k->decl->hasHeader && k->decl->supertypes.empty() && !args.exprs.empty())
                report(at, what + " takes no arguments");
        }
        noteCallThrows(nullptr, k, args, nullptr, at);
        Type t = classType(k);
        for (const std::string& p : k->typeParams) t.args.push_back(env.count(p) ? env[p] : unknownType());
        if (!k->typeParams.empty() && k->decl != nullptr && !k->isInterface) noteTypeArguments(k->typeParams, env, at);
        if (k->isValue && k->typeParams.empty()) constructedValues[&at] = k;
        return t;
    }

    // Calls of a class's name that chose one of its secondary constructors: the static function
    // made of it, which the compiler calls in place of making the object.
    std::unordered_map<const Node*, FuncInfo*> secondaryCalls;

    // A call of a name that may have several forms (overloads, D56): the form it means is
    // chosen, and the call checked against it. `envs`, when given, holds each form's own `env`.
    Type callFunctions(const std::vector<FuncInfo*>& fs, const CallExpr& c, const std::string& what, const Env& env,
                       const Type* receiver, const std::vector<Env>* envs = nullptr) {
        struct Noted {
            const CallExpr*& at;
            const CallExpr* saved;
            ~Noted() { at = saved; }
        } noted{methodCall, methodCall};
        methodCall = &c;
        Args args{c.args, c.names, c.trailingLambda, c.hasSpread};
        auto envOfForm = [&](size_t i) -> const Env& { return envs != nullptr && i < envs->size() ? (*envs)[i] : env; };
        if (fs.size() == 1) checkInfixCall(fs[0], c);
        if (fs.size() == 1) noteSelfCall(fs[0], c);
        if (fs.size() == 1) return callWith(fs[0], args, c.typeArgs, envOfForm(0), receiver, c, what + "(...)");
        // Forms with the same receiver and parameters are one form declared in several places:
        // the nearest is the one meant, as for any name.
        std::vector<FuncInfo*> forms;
        std::vector<Env> formEnvs;
        std::unordered_map<std::string, size_t> bySignature;
        for (size_t i = 0; i < fs.size(); i++) {
            FuncInfo* f = fs[i];
            const std::string key = (f->isExtension ? argText(f->receiver) : std::string()) + "|" + signatureOf(f, envOfForm(i));
            auto [at, added] = bySignature.emplace(key, forms.size());
            if (added) {
                forms.push_back(f);
                formEnvs.push_back(envOfForm(i));
            } else if (nearness(f) < nearness(forms[at->second])) {
                forms[at->second] = f;
                formEnvs[at->second] = envOfForm(i);
            }
        }
        if (forms.size() == 1) checkInfixCall(forms[0], c);
        if (forms.size() == 1) noteSelfCall(forms[0], c);
        if (forms.size() == 1) return callWith(forms[0], args, c.typeArgs, formEnvs[0], receiver, c, what + "(...)");
        size_t index = 0;
        auto envOfChosen = [&](size_t i) -> const Env& { return formEnvs[i]; };
        FuncInfo* f = args.spread ? nullptr : chooseForm(forms, envOfChosen, args, c.typeArgs, receiver, c, what, index);
        if (f == nullptr) {
            for (const ExprPtr& a : c.args) expr(*a);
            return unknownType();
        }
        chosenForms[&c] = f;
        checkInfixCall(f, c);
        noteSelfCall(f, c);
        return callWith(f, args, c.typeArgs, formEnvs[index], receiver, c, what + "(...)");
    }

    // A call of the function being checked, on the same object: a bare name or `this.name`
    // (D29). Whether a subclass overrides it is seen once the whole program is.
    std::unordered_map<const CallExpr*, FuncInfo*> selfCalls;
    void noteSelfCall(FuncInfo* f, const CallExpr& c) {
        if (silent != 0 || ctx == nullptr || ctx->isLambda || ctx->fn != f || f->decl == nullptr || f->decl->isLambda) return;
        if (c.callee->kind == ExprKind::Member) {
            const auto& m = static_cast<const MemberExpr&>(*c.callee);
            if (m.safe || m.object->kind != ExprKind::This || !static_cast<const ThisExpr&>(*m.object).label.empty()) return;
        } else if (c.callee->kind != ExprKind::Name) {
            return;
        }
        selfCalls[&c] = f;
    }
    bool overriddenBelow(const FuncInfo* f) const {
        if (f->owner == nullptr || f->isStatic) return false;
        for (const auto& k : classStore) {
            if (k.get() == f->owner || k->methods.count(f->name) == 0) continue;
            for (const ClassInfo* above = k->superclass; above != nullptr; above = above->superclass)
                if (above == f->owner) return true;
            if (f->owner->isInterface) return true;  // a class may implement it with one of its own
        }
        return false;
    }

    // ---- Checked exceptions (D35, D51 to D54) ----
    //
    // While bodies are checked, each function, lambda, class's construction and other body
    // gets a scope noting what is thrown in it (`throw` of a checked exception), what it calls
    // (with, for each parameter of a function type, what is given: a lambda, a parameter of the
    // caller, or a function value of a type that says what it throws), and its try statements,
    // each with scopes of its own. After the whole program, what may leave each is worked out
    // until nothing changes; a parameter only called stands for whatever is given for it, as
    // Swift's rethrows. Then the places that cannot pass something on are told.
    struct ThrowScope;
    struct TryRecord {
        ThrowScope* body = nullptr;
        std::vector<std::pair<std::vector<ClassInfo*>, ThrowScope*>> clauses;  // what each catches, its body
        ThrowScope* finallyScope = nullptr;
    };
    struct ArgSource {
        const FunctionDecl* lambda = nullptr;           // a lambda written there
        std::pair<FuncInfo*, int> param{nullptr, -1};   // a parameter of the caller, passed on
        std::vector<ClassInfo*> classes;                // a value of a type that says what it throws
    };
    struct CallRecord {
        FuncInfo* callee = nullptr;
        ClassInfo* constructs = nullptr;
        std::vector<ArgSource> params;  // one per parameter of the callee
        const Node* at = nullptr;
    };
    struct ThrowScope {
        std::vector<ClassInfo*> direct;
        std::vector<std::pair<FuncInfo*, int>> paramCalls;
        std::vector<CallRecord> calls;
        std::vector<TryRecord*> tries;
        std::vector<std::pair<TryRecord*, size_t>> rethrows;
        const Node* firstAt = nullptr;
    };
    struct ThrowRoot {
        enum Kind { Function, Lambda, Build, Sink, Script } kind = Function;
        ThrowScope* scope = nullptr;
        Unit* unit = nullptr;
        const Node* at = nullptr;
        FuncInfo* fn = nullptr;
    };
    struct ThrowSet {
        std::vector<ClassInfo*> classes;
        std::vector<std::pair<FuncInfo*, int>> params;
        bool operator==(const ThrowSet& o) const { return classes == o.classes && params == o.params; }
    };
    struct LambdaThrows {
        Unit* unit = nullptr;
        Type as;  // the function type it was checked as, where it is kept
    };
    std::deque<ThrowScope> throwScopes;
    std::deque<TryRecord> tryRecords;
    std::unordered_map<const void*, ThrowRoot> throwRoots;  // by a FunctionDecl, a ClassInfo, or a FnCtx's sink
    std::unordered_map<const FunctionDecl*, LambdaThrows> lambdaThrows;
    std::vector<std::tuple<FuncInfo*, const Node*, Unit*>> functionValues;  // and the file using it
    std::unordered_map<const void*, ThrowSet> thrownBy;
    std::unordered_map<const TryRecord*, ThrowSet> tryBodies;
    std::deque<std::unique_ptr<int>> sinkKeys;

    static const std::vector<ExprPtr>& noExprs() {
        static const std::vector<ExprPtr> none;
        return none;
    }
    static Args noArgs() { return Args{noExprs(), noNames(), false, false}; }

    ThrowScope* newScope() {
        throwScopes.emplace_back();
        return &throwScopes.back();
    }
    ThrowScope* newThrowRoot(const void* key, ThrowRoot::Kind kind, const Node& at, FuncInfo* fn = nullptr) {
        ThrowRoot& r = throwRoots[key];
        r.kind = kind;
        r.scope = newScope();
        r.unit = unit;
        r.at = &at;
        r.fn = fn;
        return r.scope;
    }

    // Where what is thrown or called in the code being checked is noted; none while checking
    // silently. Made when first needed: a function's, a class's construction (its fields'
    // values and init blocks), a static or lazy starting value's, or the top level's.
    ThrowScope* throwHere(const Node& at) {
        if (ctx == nullptr || silent != 0) return nullptr;
        if (ctx->throwScope != nullptr) return static_cast<ThrowScope*>(ctx->throwScope);
        if (ctx->isLambda) return nullptr;
        ThrowScope* scope = nullptr;
        if (ctx->topLevel) {
            scope = newThrowRoot(unit, ThrowRoot::Script, at);
        } else if (ctx->fn != nullptr && ctx->fn->decl != nullptr) {
            scope = newThrowRoot(ctx->fn->decl, ThrowRoot::Function, *ctx->fn->decl, ctx->fn);
        } else if (ctx->cls != nullptr && !ctx->isStatic && ctx->noWait == ObjectMade) {
            auto found = throwRoots.find(ctx->cls);
            scope = found != throwRoots.end() ? found->second.scope : newThrowRoot(ctx->cls, ThrowRoot::Build, at);
        } else {
            sinkKeys.push_back(std::make_unique<int>(0));
            scope = newThrowRoot(sinkKeys.back().get(), ThrowRoot::Sink, at);
        }
        ctx->throwScope = scope;
        if (scope->firstAt == nullptr) scope->firstAt = &at;
        return scope;
    }

    ClassInfo* stdClass(const char* name) {
        const Type t = stdClassType(name);
        return t.kind == Type::Class ? t.cls : nullptr;
    }
    ClassInfo* errorClass = nullptr;
    ClassInfo* runtimeClass = nullptr;
    ClassInfo* throwableClass = nullptr;
    // Checked: an Exception that is not a RuntimeException, or a Throwable of a class of its own.
    bool isChecked(ClassInfo* k) {
        if (throwableClass == nullptr) {
            errorClass = stdClass("Error");
            runtimeClass = stdClass("RuntimeException");
            throwableClass = stdClass("Throwable");
        }
        ClassInfo* const error = errorClass;
        ClassInfo* const runtime = runtimeClass;
        ClassInfo* const throwable = throwableClass;
        if (k == nullptr || throwable == nullptr || !isSubclass(k, throwable)) return false;
        return !(error != nullptr && isSubclass(k, error)) && !(runtime != nullptr && isSubclass(k, runtime));
    }

    void noteThrow(const Expr& value, const Type& thrown) {
        ThrowScope* scope = throwHere(value);
        if (scope == nullptr) return;
        if (value.kind == ExprKind::Name) {
            // `throw e` of a catch clause's variable: what the try's body could throw, of that class.
            if (Local* l = findLocal(static_cast<const NameExpr&>(value).name); l != nullptr && l->catchOf != nullptr) {
                scope->rethrows.emplace_back(static_cast<TryRecord*>(l->catchOf), static_cast<size_t>(l->catchClause));
                return;
            }
        }
        if (thrown.kind == Type::Class && isChecked(thrown.cls)) scope->direct.push_back(thrown.cls);
    }

    void noteParamCall(FuncInfo* f, int index, const Node& at) {
        ThrowScope* scope = throwHere(at);
        if (scope != nullptr && index >= 0) scope->paramCalls.emplace_back(f, index);
    }

    void noteValueCallThrows(const Type& t, const CallExpr& c) {
        ThrowScope* scope = throwHere(c);
        if (scope == nullptr || t.kind != Type::Function) return;
        if (t.fnRef != nullptr) {
            // A function by its name, kept in a variable: a call of that function.
            CallRecord r;
            r.callee = t.fnRef;
            r.at = &c;
            scope->calls.push_back(std::move(r));
            return;
        }
        for (const Type& x : t.throws)
            if (x.kind == Type::Class && isChecked(x.cls)) scope->direct.push_back(x.cls);
    }

    void noteCallThrows(FuncInfo* f, ClassInfo* constructs, const Args& args, const Binding* b, const Node& at) {
        ThrowScope* scope = throwHere(at);
        if (scope == nullptr) return;
        CallRecord r;
        r.callee = f;
        r.constructs = constructs;
        r.at = &at;
        if (f != nullptr && b != nullptr) {
            r.params.resize(f->params.size());
            for (size_t i = 0; i < args.exprs.size() && i < b->paramOf.size(); i++) {
                const int p = b->paramOf[i];
                if (p < 0 || static_cast<size_t>(p) >= f->params.size() || f->params[static_cast<size_t>(p)].kind != Type::Function) continue;
                const Expr& arg = *args.exprs[i];
                ArgSource& src = r.params[static_cast<size_t>(p)];
                if (arg.kind == ExprKind::Function) {
                    src.lambda = static_cast<const FunctionExpr&>(arg).fn.get();
                    continue;
                }
                if (arg.kind == ExprKind::Name)
                    if (Local* l = findLocal(static_cast<const NameExpr&>(arg).name); l != nullptr && l->paramOf != nullptr) {
                        src.param = {l->paramOf, l->paramIndex};
                        continue;
                    }
                silent++;
                const Type given = expr(arg);
                silent--;
                if (given.kind == Type::Function) {
                    if (given.fnRef != nullptr) {
                        // A function by its name: what it may throw, worked out with the rest.
                        CallRecord inner;
                        inner.callee = given.fnRef;
                        inner.at = &arg;
                        scope->calls.push_back(std::move(inner));
                    }
                    for (const Type& x : given.throws)
                        if (x.kind == Type::Class && isChecked(x.cls)) src.classes.push_back(x.cls);
                }
            }
        }
        scope->calls.push_back(std::move(r));
    }

    void noteFunctionValue(FuncInfo* f, const Node& at) {
        if (silent == 0) functionValues.emplace_back(f, &at, unit);
    }

    // A try's scopes, set in turn as its body, each clause and its finally block are checked.
    struct TryThrows {
        Impl& c;
        FnCtx* where;
        void* outer;
        TryRecord* record = nullptr;
        TryThrows(Impl& checker, const TryStmt& t) : c(checker), where(checker.ctx), outer(nullptr) {
            ThrowScope* around = c.throwHere(t);
            if (around == nullptr) return;
            outer = around;
            c.tryRecords.emplace_back();
            record = &c.tryRecords.back();
            record->body = c.newScope();
            around->tries.push_back(record);
            where->throwScope = record->body;
        }
        void clause(size_t, const Type& caught) {
            if (record == nullptr) return;
            std::vector<ClassInfo*> classes;
            if (caught.kind == Type::Class && caught.cls != nullptr) classes.push_back(caught.cls);
            record->clauses.emplace_back(classes, c.newScope());
            where->throwScope = record->clauses.back().second;
        }
        void declare(const std::string& name, const Type& caught, size_t index) {
            c.declareLocal(name, caught, true);
            if (record == nullptr) return;
            Local& l = c.scopes.back()[name];
            l.catchOf = record;
            l.catchClause = static_cast<int>(index);
        }
        void finally() {
            if (record == nullptr) return;
            record->finallyScope = c.newScope();
            where->throwScope = record->finallyScope;
        }
        ~TryThrows() {
            if (record != nullptr) where->throwScope = outer;
        }
    };

    // What a function writes after `throws`.
    std::vector<Type> writtenThrowsTypes(FuncInfo* f) {
        std::vector<Type> types;
        if (f == nullptr || f->decl == nullptr || f->unit == nullptr) return types;
        for (const TypeAnnotation& a : f->decl->throwsTypes) types.push_back(resolve(a, *f->unit));
        return types;
    }
    std::vector<ClassInfo*> writtenThrows(FuncInfo* f) {
        std::vector<ClassInfo*> list;
        for (const Type& t : writtenThrowsTypes(f))
            if (t.kind == Type::Class && t.cls != nullptr) list.push_back(t.cls);
        return list;
    }

    static void addClass(ThrowSet& s, ClassInfo* k) {
        if (std::find(s.classes.begin(), s.classes.end(), k) == s.classes.end()) s.classes.push_back(k);
    }
    static void addParam(ThrowSet& s, std::pair<FuncInfo*, int> p) {
        if (std::find(s.params.begin(), s.params.end(), p) == s.params.end()) s.params.push_back(p);
    }
    static void addAll(ThrowSet& into, const ThrowSet& from) {
        for (ClassInfo* k : from.classes) addClass(into, k);
        for (const auto& p : from.params) addParam(into, p);
    }

    // What callers of a function handle: what it writes, if it writes anything or has no body;
    // otherwise what its body throws. Either way, what is given for a parameter it only calls.
    ThrowSet allowance(FuncInfo* f) {
        ThrowSet s;
        if (f == nullptr || f->decl == nullptr) return s;
        const bool written = !f->decl->throwsTypes.empty() || f->isExternal || f->decl->isAbstract || f->unit == nullptr ||
                             f->unit->skipBodies;
        if (written) {
            for (ClassInfo* k : writtenThrows(f)) addClass(s, k);
            // A kept file's function writes nothing: what its facts say it throws (D145).
            if (f->decl->throwsTypes.empty() && f->unit != nullptr && f->unit->skipBodies)
                for (ClassInfo* k : keptThrown(f)) addClass(s, k);
            resolveFunc(f);
            for (size_t i = 0; i < f->params.size(); i++)
                if (f->params[i].kind == Type::Function && inPlace(f, static_cast<int>(i))) addParam(s, {f, static_cast<int>(i)});
            return s;
        }
        auto found = thrownBy.find(f->decl);
        if (found != thrownBy.end()) s = found->second;
        return s;
    }

    ThrowSet constructionThrows(ClassInfo* k, int depth = 0) {
        ThrowSet s;
        if (k == nullptr || depth > 64) return s;
        if (auto found = thrownBy.find(k); found != thrownBy.end()) addAll(s, found->second);
        return s;
    }

    ThrowSet evalCall(const CallRecord& r) {
        ThrowSet set;
        if (r.constructs != nullptr) addAll(set, constructionThrows(r.constructs));
        if (r.callee == nullptr) return set;
        const ThrowSet a = allowance(r.callee);
        for (ClassInfo* k : a.classes) addClass(set, k);
        for (const auto& mark : a.params) {
            if (mark.first != r.callee) {
                addParam(set, mark);
                continue;
            }
            if (mark.second < 0 || static_cast<size_t>(mark.second) >= r.params.size()) continue;
            const ArgSource& src = r.params[static_cast<size_t>(mark.second)];
            if (src.lambda != nullptr) {
                if (auto found = thrownBy.find(src.lambda); found != thrownBy.end()) addAll(set, found->second);
            } else if (src.param.first != nullptr) {
                addParam(set, src.param);
            }
            for (ClassInfo* k : src.classes) addClass(set, k);
        }
        return set;
    }

    bool catches(const std::vector<ClassInfo*>& caught, ClassInfo* k) {
        for (ClassInfo* c : caught)
            if (isSubclass(k, c)) return true;
        return false;
    }
    // A clause that catches every Exception catches whatever a lambda given may throw.
    bool catchesAllChecked(const TryRecord& t) {
        ClassInfo* exception = stdClass("Exception");
        for (const auto& [caught, scope] : t.clauses)
            for (ClassInfo* c : caught)
                if (exception != nullptr && isSubclass(exception, c)) return true;
        return false;
    }

    ThrowSet evalScope(const ThrowScope* s, int depth = 0) {
        ThrowSet set;
        if (s == nullptr || depth > 200) return set;
        for (ClassInfo* k : s->direct) addClass(set, k);
        for (const auto& p : s->paramCalls) addParam(set, p);
        for (const CallRecord& r : s->calls) addAll(set, evalCall(r));
        for (const TryRecord* t : s->tries) {
            const ThrowSet body = evalScope(t->body, depth + 1);
            tryBodies[t] = body;
            for (ClassInfo* k : body.classes) {
                bool caught = false;
                for (const auto& [classes, scope] : t->clauses) caught = caught || catches(classes, k);
                if (!caught) addClass(set, k);
            }
            if (!catchesAllChecked(*t))
                for (const auto& p : body.params) addParam(set, p);
            for (const auto& [classes, scope] : t->clauses) addAll(set, evalScope(scope, depth + 1));
            addAll(set, evalScope(t->finallyScope, depth + 1));
        }
        for (const auto& [t, index] : s->rethrows) {
            // `throw e`: of what the try's body may throw, what this clause took (D35).
            const ThrowSet& body = tryBodies[t];
            const std::vector<ClassInfo*>& took = t->clauses[index].first;
            for (ClassInfo* k : body.classes) {
                if (catches(took, k)) addClass(set, k);
                for (ClassInfo* c : took)
                    if (c != k && isSubclass(c, k) && isChecked(c)) addClass(set, c);
            }
            bool all = false;
            ClassInfo* exception = stdClass("Exception");
            for (ClassInfo* c : took) all = all || (exception != nullptr && isSubclass(exception, c));
            if (all)
                for (const auto& p : body.params) addParam(set, p);
        }
        return set;
    }

    static std::string classList(const std::vector<ClassInfo*>& classes) {
        std::string s;
        for (size_t i = 0; i < classes.size(); i++) s += (i ? " | " : "") + classes[i]->name;
        return s;
    }
    std::vector<ClassInfo*> beyond(const std::vector<ClassInfo*>& thrown, const std::vector<ClassInfo*>& allowed) {
        std::vector<ClassInfo*> list;
        for (ClassInfo* k : thrown)
            if (!catches(allowed, k)) list.push_back(k);
        return list;
    }

    std::unordered_set<Unit*> judging;  // the files checked this time, whose places are told

    // After the whole program: what may leave each body, then each place that cannot pass it on.
    void judgeThrows() {
        if (stdClass("Exception") == nullptr) return;
        for (int round = 0; round < 64; round++) {
            bool changed = false;
            for (const auto& [key, root] : throwRoots) {
                ThrowSet now = evalScope(root.scope);
                ThrowSet& was = thrownBy[key];
                if (!(now == was)) {
                    ThrowSet merged = was;
                    addAll(merged, now);
                    if (!(merged == was)) changed = true;
                    was = merged;
                }
            }
            if (!changed) break;
        }
        Unit* saved = unit;
        // A lambda given where it is only called passes on what it throws; any other is kept,
        // to be called where nothing catches what it throws but what its type says (D30, D52).
        std::unordered_set<const FunctionDecl*> passedOn;
        for (const auto& [key, root] : throwRoots) {
            std::vector<const ThrowScope*> stack{root.scope};
            while (!stack.empty()) {
                const ThrowScope* s = stack.back();
                stack.pop_back();
                if (s == nullptr) continue;
                for (const CallRecord& r : s->calls)
                    for (size_t p = 0; p < r.params.size(); p++)
                        if (r.params[p].lambda != nullptr && r.callee != nullptr && inPlace(r.callee, static_cast<int>(p)))
                            passedOn.insert(r.params[p].lambda);
                for (const TryRecord* t : s->tries) {
                    stack.push_back(t->body);
                    for (const auto& [classes, scope] : t->clauses) stack.push_back(scope);
                    stack.push_back(t->finallyScope);
                }
            }
        }
        for (const auto& [lambda, kept] : lambdaThrows) {
            if (passedOn.count(lambda) != 0 || judging.count(kept.unit) == 0) continue;
            auto found = thrownBy.find(lambda);
            if (found == thrownBy.end() || found->second.classes.empty()) continue;
            std::vector<ClassInfo*> allowed;
            for (const Type& t : kept.as.throws)
                if (t.kind == Type::Class && t.cls != nullptr) allowed.push_back(t.cls);
            const std::vector<ClassInfo*> extra = beyond(found->second.classes, allowed);
            if (extra.empty()) continue;
            unit = kept.unit;
            report(*lambda, "this lambda may throw " + classList(extra) + ", which nothing can catch where it is called: it is kept " +
                                "as " + describe(kept.as) + ", which throws " + (allowed.empty() ? std::string("nothing checked") : classList(allowed)) +
                                "; catch it inside the lambda (D35)");
        }
        for (const auto& [key, root] : throwRoots) {
            const ThrowSet& set = thrownBy[key];
            unit = root.unit;
            if (judging.count(root.unit) == 0) continue;
            if (root.kind == ThrowRoot::Sink && !set.classes.empty())
                report(*root.scope->firstAt, "this starting value may throw " + classList(set.classes) +
                                                 ", which nothing can catch where it is worked out: catch it here (D35)");
            if (root.kind != ThrowRoot::Function || root.fn == nullptr || root.fn->decl == nullptr) continue;
            FuncInfo* f = root.fn;
            const FunctionDecl& d = *f->decl;
            if (!d.throwsTypes.empty()) {
                // throws written: a promise the body keeps (D54).
                const std::vector<ClassInfo*> extra = beyond(set.classes, writtenThrows(f));
                if (!extra.empty())
                    report(d, "'" + d.name + "' may throw " + classList(extra) + ", which its throws does not name: catch it, or add it (D54)");
            } else if (!set.classes.empty() && libraryPublic(f)) {
                report(d, "'" + d.name + "' may throw " + classList(set.classes) + ": a library's public function says so, " +
                              "throws " + classList(set.classes) + " (D35)");
            }
        }
        // An override throws no more than what it takes the place of may (D53).
        for (const auto& [decl, f] : funcOf) {
            if (f->owner == nullptr || f->isStatic || f->decl == nullptr) continue;
            const std::vector<FuncInfo*>& takingPlace = overridersOf(f);
            if (takingPlace.empty()) continue;
            const ThrowSet above = allowance(f);
            for (FuncInfo* g : takingPlace) {
                if (g->decl == nullptr || g->unit == nullptr || judging.count(g->unit) == 0) continue;
                const std::vector<ClassInfo*> extra = beyond(allowance(g).classes, above.classes);
                if (extra.empty()) continue;
                unit = g->unit;
                report(*g->decl, "'" + g->name + "' may throw " + classList(extra) + ", which " + f->owner->name + "." + f->name +
                                     " it takes the place of cannot: callers through " + f->owner->name + " would not catch it; " +
                                     "catch it here, or write throws " + classList(extra) + " on " + f->owner->name + "." + f->name + " (D53)");
            }
        }
        // A function as a value throws what it writes (D52).
        for (const auto& [f, at, user] : functionValues) {
            if (judging.count(user) == 0) continue;
            const std::vector<ClassInfo*> extra = beyond(allowance(f).classes, writtenThrows(f));
            if (extra.empty()) continue;
            unit = user;
            report(*at, "'" + f->name + "' may throw " + classList(extra) + ", and as a value a function throws only what it writes: " +
                            "write throws " + classList(extra) + " on it, or call it in a lambda (D52)");
        }
        unit = saved;
    }

    // A library's public function: what a program using the library sees (D35).
    bool libraryPublic(FuncInfo* f) {
        if (f->unit == nullptr || f->unit->ns == nullptr || f->decl == nullptr) return false;
        if (!f->unit->prelude && f->unit->ns->library == "<program>") return false;
        if (f->decl->access == kAccessWordPrivate || f->decl->access == kAccessWordInternal) return false;
        if (f->decl->isLambda || f->decl->name.empty() || f->decl->name[0] == '_') return false;
        if (f->stmt != nullptr && f->stmt->visibility != 0) return false;
        if (f->owner != nullptr && f->owner->decl != nullptr && f->owner->decl->visibility != 0) return false;
        return topLevelPublic.count(f->decl) != 0 || f->owner != nullptr || f->isExtension;
    }
    std::unordered_set<const FunctionDecl*> topLevelPublic;

    // ---- Inline functions (D59) ----

    std::unordered_set<const CallExpr*> onObjectCalls, memberLocalCalls;
    struct InlineCallFact {
        FuncInfo* fn = nullptr;
        std::vector<int> paramOf;
    };
    std::unordered_map<const Node*, InlineCallFact> inlineCalls;
    // The inline functions each inline function's body calls, to find one that calls itself.
    std::map<FuncInfo*, std::vector<std::pair<FuncInfo*, const Node*>>> inlineCallsFrom;
    std::map<std::pair<FuncInfo*, int>, int> copiedKnown;  // 1 being worked out, 2 yes, 3 no

    void noteInlineCall(FuncInfo* f, const Binding& b, const Node& at) {
        inlineCalls[&at] = InlineCallFact{f, b.paramOf};
        FnCtx* c = ctx;
        while (c != nullptr && c->isLambda) c = c->parent;
        if (c != nullptr && c->fn != nullptr && c->fn->decl != nullptr && c->fn->decl->isInline)
            inlineCallsFrom[c->fn].emplace_back(f, &at);
    }

    bool copiedIn(FuncInfo* f, int param) {
        if (f == nullptr || f->decl == nullptr || !f->decl->isInline || param < 0 || param == f->vararg) return false;
        resolveFunc(f);
        if (static_cast<size_t>(param) >= f->params.size() || f->params[static_cast<size_t>(param)].kind != Type::Function) return false;
        const auto key = std::make_pair(f, param);
        auto known = copiedKnown.find(key);
        if (known != copiedKnown.end()) return known->second != 3;
        copiedKnown[key] = 1;
        bool yes = true;
        auto uses = paramUses.find(key);
        if (uses != paramUses.end())
            for (const ParamUse& u : uses->second) {
                if (u.kind == ParamUse::Other || (u.kind == ParamUse::PassedOn && !copiedIn(u.to, u.toParam))) yes = false;
                for (const FunctionDecl* l : u.lambdas) {
                    auto site = lambdaSites.find(l);
                    yes = yes && site != lambdaSites.end() && copiedIn(site->second.fn, site->second.param);
                }
                if (!yes) break;
            }
        copiedKnown[key] = yes ? 2 : 3;
        return yes;
    }

    // After the whole program: an inline function may not call itself, directly or through
    // others, since copying it into its calls would never end.
    void judgeInline() {
        std::map<FuncInfo*, int> state;  // 1 on the path, 2 done
        std::unordered_set<FuncInfo*> told;
        std::vector<FuncInfo*> path;
        std::function<void(FuncInfo*)> visit = [&](FuncInfo* f) {
            state[f] = 1;
            path.push_back(f);
            for (const auto& [g, at] : inlineCallsFrom[f]) {
                if (state[g] == 1) {
                    // Each function of the cycle is told, starting from itself.
                    std::vector<FuncInfo*> cycle;
                    bool on = false;
                    for (FuncInfo* p : path) {
                        on = on || p == g;
                        if (on) cycle.push_back(p);
                    }
                    for (size_t i = 0; i < cycle.size(); i++) {
                        FuncInfo* p = cycle[i];
                        if (p->unit == nullptr || !told.insert(p).second) continue;
                        std::string text;
                        for (size_t j = 0; j <= cycle.size(); j++) text += (j > 0 ? " -> " : "") + cycle[(i + j) % cycle.size()]->name;
                        Unit* saved = unit;
                        unit = p->unit;
                        report(*p->decl, "inline fun " + p->name + " calls itself (" + text +
                                             "): copied into each call, it would never end; take 'inline' from one of them (D59)");
                        unit = saved;
                    }
                } else if (state[g] == 0) {
                    visit(g);
                }
            }
            path.pop_back();
            state[f] = 2;
        };
        for (const auto& [f, calls] : inlineCallsFrom)
            if (state[f] == 0) visit(f);
        // A member's calls are copies of its own body: nothing may take its place, and it has one.
        for (const auto& [decl, f] : funcOf) {
            if (!decl->isInline || f->unit == nullptr) continue;
            Unit* saved = unit;
            if (decl->isAbstract) {
                unit = f->unit;
                report(*decl, "inline fun " + f->name + " has no body to copy into its calls (D59)");
            }
            if (f->owner != nullptr && !f->isStatic)
                for (FuncInfo* g : overridersOf(f)) {
                    if (g->unit == nullptr || g->decl == nullptr) continue;
                    unit = g->unit;
                    report(*g->decl, "'" + g->name + "' would take the place of inline fun " + f->owner->name + "." + f->name +
                                         ", whose calls are copies of its own body: an inline member cannot be overridden (D59)");
                }
            unit = saved;
        }
    }

    // `a name b` calls a function marked infix (D72).
    std::unordered_map<const CallExpr*, const FuncInfo*> infixCallees;
    void checkInfixCall(const FuncInfo* f, const CallExpr& c) {
        if (c.isInfix && silent == 0) infixCallees[&c] = f;
        if (!c.isInfix || f->decl == nullptr || f->decl->isInfix) return;
        const auto& m = static_cast<const MemberExpr&>(*c.callee);
        report(m, "'" + m.name + "' is not an infix function, so it is called " + placeText(*m.object) + "." + m.name + "(" +
                      placeText(*c.args[0]) + "); to call it this way, declare it infix fun (D72)");
    }

    // How near a declaration is to the code being checked: its own file, its folder, another
    // part of the program, the standard library.
    int nearness(const FuncInfo* f) const {
        if (f->unit == unit) return 0;
        if (f->unit != nullptr && unit != nullptr && f->unit->ns == unit->ns) return 1;
        if (f->unit != nullptr && !f->unit->prelude) return 2;
        return 3;
    }

    // Which form of a name a call means, chosen as Kotlin does: of the forms the arguments
    // fit, the most specific, the one whose parameters all fit those of each other. Between
    // two that are as specific, one that needs neither a vararg nor a default it is not given
    // goes first. Null where none fits or several do, which is reported, and where an argument
    // or a parameter has a type not known well enough to choose by, which is not.
    std::unordered_map<const CallExpr*, FuncInfo*> chosenForms;

    struct Fit {
        size_t index = 0;
        Binding binding;
        Env env;
        int defaultsUsed = 0;
        bool vararg = false;
        int conversions = 0;  // lambdas given as interfaces (D28)
        int narrowed = 0;     // int constants taken as bytes (D19)
    };

    template <typename EnvOf>
    FuncInfo* chooseForm(const std::vector<FuncInfo*>& fs, const EnvOf& envOfForm, const Args& args,
                         const std::vector<TypeAnnotation>& typeArgs, const Type* receiver, const Node& at,
                         const std::string& what, size_t& index) {
        // The arguments' types, once, without reporting: the call is checked again against
        // the form chosen.
        std::vector<Type> types(args.exprs.size());
        bool unknownArgument = false;
        silent++;
        for (size_t i = 0; i < args.exprs.size(); i++) {
            if (args.exprs[i]->kind == ExprKind::Function) continue;
            types[i] = expr(*args.exprs[i]);
            if (types[i].unknown()) unknownArgument = true;
        }
        std::vector<Fit> candidates;
        bool receiverFits = false;  // some form is for the object the call is made on
        for (size_t k = 0; k < fs.size(); k++) {
            FuncInfo* f = fs[k];
            resolveFunc(f);
            Fit fit;
            fit.index = k;
            fit.env = envOfForm(k);
            std::vector<std::string> vars = f->typeParams;
            for (size_t i = 0; i < typeArgs.size() && i < f->typeParams.size(); i++) fit.env[f->typeParams[i]] = resolve(typeArgs[i], *unit);
            vars.erase(std::remove_if(vars.begin(), vars.end(), [&](const std::string& v) { return fit.env.count(v) > 0; }), vars.end());
            if (receiver != nullptr && f->isExtension) {
                unify(f->receiver, *receiver, fit.env, vars);
                fixByReceiver(f->receiver, fit.env, vars);
                if (!fits(*receiver, subst(f->receiver, fit.env))) continue;
                receiverFits = true;
            }
            fit.binding = bind(f, args, at, what + "(...)");
            if (fit.binding.failed || fit.binding.missing) continue;
            bool ok = true;
            for (size_t i = 0; i < args.exprs.size() && ok; i++) {
                const int p = fit.binding.paramOf[i];
                if (p < 0) continue;
                if (p == f->vararg) fit.vararg = true;
                // By name, a vararg parameter takes the list itself.
                const bool byName = i < args.names.size() && !args.names[i].empty();
                const Type want = p == f->vararg ? (byName ? listOf(f->varargItem) : f->varargItem) : f->params[static_cast<size_t>(p)];
                if (args.exprs[i]->kind == ExprKind::Function) {
                    const auto& given = static_cast<const FunctionExpr&>(*args.exprs[i]);
                    const Type plain = withNull(subst(want, fit.env), false);
                    std::string method;
                    Type fn;
                    if (plain.kind == Type::Class && plain.cls != nullptr && plain.cls->isInterface) {
                        // An interface takes a lambda if it has one abstract method (D28).
                        ok = samType(plain, method, fn) && lambdaFits(given, fn);
                        fit.conversions++;
                    } else if (plain.kind == Type::Function || plain.unknown() || plain.kind == Type::Any || plain.kind == Type::TypeParam) {
                        ok = lambdaFits(given, subst(want, fit.env));
                    } else {
                        ok = false;  // a list, a number, a class: not what a lambda is
                    }
                } else {
                    unify(want, types[i], fit.env, vars);
                    ok = fits(types[i], subst(want, fit.env));
                    // An int constant that fits is a byte where a byte goes (D19).
                    if (!ok && types[i].kind == Type::Int && subst(want, fit.env).kind == Type::Byte && literalsOnly(*args.exprs[i])) {
                        const ConstValue known = foldQuietly(*args.exprs[i]);
                        ok = known.kind == ConstValue::Int && known.i >= 0 && known.i <= 255;
                        if (ok) fit.narrowed++;
                    }
                }
            }
            if (!ok) continue;
            for (size_t p = 0; p < f->params.size(); p++)
                if (!fit.binding.given[p] && f->hasDefault[p]) fit.defaultsUsed++;
            candidates.push_back(std::move(fit));
        }
        silent--;

        auto argumentsText = [&] {
            std::string s;
            for (size_t i = 0; i < args.exprs.size(); i++) {
                if (i > 0) s += ", ";
                s += args.exprs[i]->kind == ExprKind::Function ? std::string("a lambda") : describe(types[i]);
            }
            return s.empty() ? std::string("no arguments") : s;
        };
        auto formsText = [&](const std::vector<size_t>& which) {
            std::string s;
            for (size_t i = 0; i < which.size(); i++) {
                if (i > 0) s += i + 1 == which.size() ? " and " : ", ";
                FuncInfo* f = fs[which[i]];
                Env e = envOfForm(which[i]);
                if (receiver != nullptr && f->isExtension) {
                    std::vector<std::string> vars = f->typeParams;
                    unify(f->receiver, *receiver, e, vars);
                }
                s += formText(what, f, e);
            }
            return s;
        };

        if (candidates.empty()) {
            if (unknownArgument) return nullptr;
            std::vector<size_t> all;
            for (size_t k = 0; k < fs.size(); k++) all.push_back(k);
            if (receiver != nullptr && !receiverFits && fs[0]->isExtension)
                report(at, "no form of " + what + "(...) is for " + describe(*receiver) + "; there are " + formsText(all));
            else
                report(at, "no form of " + what + "(...) takes " + argumentsText() + "; there are " + formsText(all));
            return nullptr;
        }
        if (candidates.size() == 1) {
            index = candidates[0].index;
            return fs[index];
        }

        // The parameter each argument goes to, in a form; unknown where it goes to none.
        auto paramType = [&](const Fit& fit, size_t i) -> Type {
            FuncInfo* f = fs[fit.index];
            const int p = fit.binding.paramOf[i];
            if (p < 0) return unknownType();
            // A parameter without a type, or of a type parameter, takes anything: it is the least
            // specific there is.
            Type t = subst(p == f->vararg ? f->varargItem : f->params[static_cast<size_t>(p)], fit.env);
            return t.unknown() || t.kind == Type::TypeParam ? withNull(Type::of(Type::Any)) : t;
        };
        // Where an argument's type is not known, the forms cannot be told apart by it: that is
        // left to when the program runs.
        for (size_t i = 0; i < args.exprs.size(); i++) {
            if (!types[i].unknown() || args.exprs[i]->kind == ExprKind::Function) continue;
            const std::string first = argText(paramType(candidates[0], i));
            for (const Fit& fit : candidates)
                if (argText(paramType(fit, i)) != first) return nullptr;
        }
        // a is at least as specific as b: its receiver fits b's, and each argument's parameter
        // in a fits b's.
        auto asSpecific = [&](const Fit& a, const Fit& b) {
            FuncInfo* fa = fs[a.index];
            FuncInfo* fb = fs[b.index];
            if (fa->isExtension && fb->isExtension && !fits(subst(fa->receiver, a.env), subst(fb->receiver, b.env))) return false;
            for (size_t i = 0; i < args.exprs.size(); i++) {
                if (args.exprs[i]->kind == ExprKind::Function) continue;
                if (!fits(paramType(a, i), paramType(b, i))) return false;
            }
            return true;
        };
        auto better = [&](const Fit& a, const Fit& b) {
            const bool ab = asSpecific(a, b), ba = asSpecific(b, a);
            if (ab && !ba) return true;
            if (ba && !ab) return false;
            // Neither more specific: one the arguments fit as they are before one taking an int
            // constant as a byte.
            if (!ab) return a.narrowed < b.narrowed;
            if (a.narrowed != b.narrowed) return a.narrowed < b.narrowed;
            if (a.conversions != b.conversions) return a.conversions < b.conversions;  // a function type before an interface
            if (a.vararg != b.vararg) return !a.vararg;
            return a.defaultsUsed < b.defaultsUsed;
        };
        std::vector<size_t> best;
        for (size_t i = 0; i < candidates.size(); i++) {
            bool beaten = false;
            for (size_t j = 0; j < candidates.size() && !beaten; j++)
                if (i != j && better(candidates[j], candidates[i])) beaten = true;
            if (!beaten) best.push_back(i);
        }
        if (best.size() == 1) {
            index = candidates[best[0]].index;
            return fs[index];
        }
        // Forms told apart only by parameters whose types are not written cannot be chosen
        // between yet: that is left to when the program runs.
        for (size_t i : best) {
            FuncInfo* f = fs[candidates[i].index];
            for (const Type& p : f->params)
                if (p.unknown()) return nullptr;
            if (f->isExtension && f->receiver.unknown()) return nullptr;
        }
        std::vector<size_t> which;
        for (size_t i : best) which.push_back(candidates[i].index);
        bool lambdaGiven = false;
        for (size_t i : best) lambdaGiven = lambdaGiven || candidates[i].conversions > 0;
        report(at, "this call fits more than one form of " + what + "(...): " + formsText(which) +
                       (lambdaGiven ? "; the lambda could be either interface: say which, as in Name { ... } (D28)"
                                    : "; give an argument a type that tells them apart, with a typed variable or 'as'"));
        return nullptr;
    }

    // Whether a lambda literal could be given for a parameter of this type: a function type
    // taking as many parameters as it names (one or none, if it names none and may use `it`).
    static bool lambdaFits(const FunctionExpr& lambda, const Type& want) {
        if (want.kind != Type::Function || !want.knownParams) return true;
        const FunctionDecl& d = *lambda.fn;
        // Without an arrow a lambda has `it`, a parameter with a default: it takes one argument or none.
        const bool it = d.params.size() == 1 && d.params[0] == "it" && !d.paramTypes[0].present() && d.paramDefaults.size() == 1 &&
                        d.paramDefaults[0] != nullptr;
        const size_t n = it ? 0 : d.params.size();
        const size_t wanted = want.args.size();
        return n == 0 ? wanted <= 1 : n == wanted;
    }

    // ---- Keys (D26) ----
    //
    // A key must not be able to change: a list, map or set only frozen, no Bytes, and an object
    // only of a class whose == and hashCode read nothing that can change, following the methods
    // and getters they call. What each method reads is gathered as its body is checked; the
    // keys going into maps and sets are judged after the whole program, when all of it is known.

    struct KeyFacts {
        std::string varRead;  // the first var field read
        std::vector<std::pair<std::string, Type>> valReads;
        std::vector<std::pair<std::string, FuncInfo*>> calls;  // methods and getters called
        std::vector<std::pair<std::string, FieldInfo*>> properties;  // properties worked out by a getter, read
    };
    std::unordered_map<FuncInfo*, KeyFacts> keyFacts;
    struct PendingKey {
        Unit* unit = nullptr;
        const Expr* at = nullptr;
        Type type;
    };
    std::vector<PendingKey> pendingKeys;
    std::unordered_map<ClassInfo*, std::string> keyTroubles;
    std::unordered_set<ClassInfo*> keyTroubleBusy;

    // The method or getter the code being checked is in, a lambda's in the one around it; null
    // at the top level.
    FuncInfo* keyFactsFn() {
        for (FnCtx* c = ctx; c != nullptr; c = c->parent) {
            if (c->fn != nullptr) return c->fn->owner != nullptr && !c->fn->isStatic ? c->fn : nullptr;
            if (!c->isLambda) break;
        }
        return nullptr;
    }

    // A member of an object read or called in a method.
    void noteKeyMember(const Member& m, const std::string& name) {
        FuncInfo* f = keyFactsFn();
        if (f == nullptr) return;
        KeyFacts& facts = keyFacts[f];
        if (m.field != nullptr && m.field->decl != nullptr && m.field->decl->getter && !m.field->decl->init) {
            // A property worked out by its getter: what the getter reads, followed when judged (D26).
            facts.properties.emplace_back(name, m.field);
        } else if (m.field != nullptr) {
            if (!m.field->isVal) {
                if (facts.varRead.empty()) facts.varRead = name;
            } else if (facts.valReads.size() < 64) {
                facts.valReads.emplace_back(name, subst(fieldType(m.field), m.env));
            }
        } else if (m.getter != nullptr) {
            facts.calls.emplace_back(name, m.getter);
        } else {
            for (FuncInfo* g : m.methods) facts.calls.emplace_back(name, g);
        }
    }

    // A value going in as a key: judged after the whole program.
    void noteKey(const Expr& at, const Type& t) {
        if (silent == 0 && t.known()) pendingKeys.push_back(PendingKey{unit, &at, t});
    }

    // Why an object of the class cannot be a key, as words after "cannot be a key:"; empty when
    // it can, or when only the program running can tell (an interface, which any class may be).
    std::string keyTrouble(ClassInfo* k) {
        if (k == nullptr || k->isEnum || k->isObject || k->isInterface) return "";
        if (auto found = keyTroubles.find(k); found != keyTroubles.end()) return found->second;
        if (!keyTroubleBusy.insert(k).second) return "";
        std::string why;
        ClassInfo* own = nullptr;  // the nearest class with its own == or hashCode
        for (ClassInfo* c = k; c != nullptr && own == nullptr; c = c->superclass) {
            resolveSupers(c);
            if (c->methods.count("==") != 0 || c->methods.count("hashCode") != 0) own = c;
        }
        if (own != nullptr) {
            // ==, hashCode and what they call, followed as far as they go.
            std::vector<std::pair<std::string, FuncInfo*>> todo;
            for (const char* name : {"==", "hashCode"})
                if (auto found = own->methods.find(name); found != own->methods.end())
                    for (FuncInfo* f : found->second) todo.emplace_back(name, f);
            std::unordered_set<FuncInfo*> followed;
            while (!todo.empty() && why.empty()) {
                const auto [name, f] = todo.back();
                todo.pop_back();
                if (!followed.insert(f).second) continue;
                if (f->decl == nullptr || f->isExternal || f->decl->isAbstract) {
                    why = "its == or hashCode calls '" + name + "', which the checker cannot follow";
                    break;
                }
                const KeyFacts& facts = keyFacts[f];
                if (!facts.varRead.empty() && !k->isValue) {
                    why = "its == or hashCode reads the var '" + facts.varRead + "', which can change";
                    break;
                }
                for (const auto& [read, type] : facts.valReads)
                    if (!keyProblem(type, "").empty()) {
                        why = "its == or hashCode reads '" + read + "', " + describe(withNull(type, false)) + ", which can change";
                        break;
                    }
                for (const auto& call : facts.calls) todo.push_back(call);
                for (const auto& [read, property] : facts.properties)
                    if (property->getterFn != nullptr) todo.emplace_back(read, property->getterFn);
            }
        } else if ((k->isData || k->isValue) && k->decl != nullptr) {
            // A data class by its header; a value type by all it holds, which goes in a map as a copy.
            std::vector<std::string> names;
            for (const HeaderParam& p : k->decl->header) names.push_back(p.name);
            if (k->isValue)
                for (const auto& field : k->decl->fields) names.push_back(field->name);
            for (const std::string& name : names) {
                auto found = k->fields.find(name);
                if (found == k->fields.end()) continue;
                const FieldInfo* f = found->second;
                if (!f->isVal && !k->isValue) {
                    why = "its var '" + name + "' can change";
                    break;
                }
                const Type type = fieldType(const_cast<FieldInfo*>(f));
                if (!keyProblem(type, "").empty()) {
                    why = "'" + name + "' is " + describe(withNull(type, false)) + ", which can change";
                    break;
                }
            }
        }
        if (why.empty() && own == nullptr && k->superclass != nullptr) why = keyTrouble(k->superclass);
        keyTroubleBusy.erase(k);
        keyTroubles[k] = why;
        return why;
    }

    // Why a value of type `t` cannot be a key; empty when it can, or when only the program
    // running can tell (any, an interface). `place` is the key's text, for the advice.
    std::string keyProblem(const Type& t, const std::string& place) {
        Type plain = withNull(t, false);
        plain.open = false;  // a key changes or not whatever the name it is reached by
        switch (plain.kind) {
            case Type::List:
            case Type::Map:
            case Type::Set:
                if (!plain.frozen)
                    return describe(plain) + " can change, so it cannot be a key" + (place.empty() ? std::string() : ": a frozen copy can, " + (place == "..." ? std::string("key") : place) + ".frozen()");
                for (const Type& a : plain.args)
                    if (const std::string why = keyProblem(a, ""); !why.empty()) return describe(plain) + " cannot be a key: it holds what can change; " + why;
                return "";
            case Type::Bytes:
                return std::string("Bytes can change, so they cannot be a key") + (place.empty() ? std::string() : ": a frozen list of them can, " + (place == "..." ? std::string("key") : place) + ".toList().frozen()");
            case Type::Class:
                if (const std::string why = keyTrouble(plain.cls); !why.empty()) return describe(plain) + " cannot be a key: " + why;
                return "";
            default:
                return "";
        }
    }

    void judgeKeys() {
        Unit* saved = unit;
        for (const PendingKey& p : pendingKeys) {
            unit = p.unit;
            if (const std::string why = keyProblem(p.type, placeText(*p.at)); !why.empty()) report(*p.at, why + " (D26)");
        }
        pendingKeys.clear();
        unit = saved;
    }

    // The error for changing what nobody may change.
    static std::string frozenChange(const Type& t) {
        const char* kind = t.kind == Type::List ? "List" : t.kind == Type::Map ? "Map" : "Set";
        return std::string("a Frozen") + kind + " cannot be changed: toOpen" + kind + "() makes a copy that can be (D26)";
    }

    // Read-only and open collections (D32): whether a list, map or set of type `t` may only be
    // read through the name it has.
    static bool readOnly(const Type& t) {
        return (t.kind == Type::List || t.kind == Type::Map || t.kind == Type::Set) && !t.open && !t.frozen;
    }

    std::string readOnlyChange(const Type& t, const Expr& through) {
        const char* kind = t.kind == Type::List ? "List" : t.kind == Type::Map ? "Map" : "Set";
        Type open = withNull(t, false);
        open.open = true;
        return placeText(through) + " is " + describe(withNull(t, false)) + ", which may only be read through it: one changed is declared " + text(open) +
               ", or toOpen" + kind + "() makes a copy that can be (D32)";
    }

    // A union has nothing of its own when the program runs (D40).
    static std::string unionTested() {
        return "a union is not tested as a whole: nothing of it is kept when the program runs; test its types one by one, " +
               std::string("with is or a when (D40)");
    }

    // `x is open List<int>`, `x as open List<int>` where x may only be read (D46).
    void notMadeOpen(const Type& value, const Type& to, const Expr& at, const char* how) {
        const Type v = withNull(value, false);
        if (!readOnly(v) || v.kind != to.kind) return;
        const char* kind = v.kind == Type::List ? "List" : v.kind == Type::Map ? "Map" : "Set";
        report(at, std::string("'") + how + "' cannot make " + describe(v) + " open: what may only be read stays so; toOpen" + kind +
                       "() makes a copy that can be changed (D46)");
    }

    // Whether a value of type `t` may be a map's whole entry: a MapEntry or a Pair, or one whose
    // type is not known (D140).
    bool wholeEntry(const Type& t) {
        if (t.unknown() || t.kind == Type::Any || t.kind == Type::TypeParam) return true;
        return t.kind == Type::Class && t.cls != nullptr && (t.cls->name == "MapEntry" || t.cls->name == "Pair");
    }

    // A method of a value: declared on its class or above, or an extension.
    Type callMethod(const Type& objIn, const CallExpr& c, const MemberExpr& m) {
        Type obj = objIn;
        if (!unionMember(obj, m.name, m)) {
            for (const ExprPtr& a : c.args) expr(*a);
            return unknownType();
        }
        // What nobody may change, and keys that cannot change (D26).
        if (obj.kind == Type::List || obj.kind == Type::Map || obj.kind == Type::Set) {
            static const std::unordered_set<std::string> changing = {"add", "addAll", "put", "putAll", "getOrPut", "update", "putIfAbsent"};
            // Said once: the call is checked on as if it could change the collection.
            if (obj.frozen && (listChangingName(m.name, 2) || changing.count(m.name) != 0)) {
                report(m, frozenChange(obj));
                obj.frozen = false;
                obj.open = true;
            } else if (readOnly(obj) && (listChangingName(m.name, 2) || changing.count(m.name) != 0)) {
                report(m, readOnlyChange(obj, *m.object));
                obj.open = true;
            } else if (!c.args.empty() && c.namedCount == 0 &&
                       ((obj.kind == Type::Map && (m.name == "put" || m.name == "set" || m.name == "getOrPut" || m.name == "update" || m.name == "putIfAbsent")) ||
                        (obj.kind == Type::Set && m.name == "add"))) {
                noteKey(*c.args[0], typeQuietly(*c.args[0]));
            }
        }
        // A map's contains takes a whole entry; a key or a value is asked of keys or values (D140).
        if (obj.kind == Type::Map && !obj.nullable && m.name == "contains" && c.args.size() == 1 && c.namedCount == 0) {
            const Type given = expr(*c.args[0]);
            if (!wholeEntry(given)) {
                report(*c.args[0], "map.contains(...) asks for a whole entry, (key to value); for a key write map.keys.contains(" +
                                       placeText(*c.args[0]) + "), and for a value map.values.contains(" + placeText(*c.args[0]) + ") (D140)");
                return Type::of(Type::Bool);
            }
        }
        if (obj.kind == Type::TypeParam) {
            if (obj.bound == nullptr) {
                Type plain = withNull(obj, false);
                std::vector<FuncInfo*> ext = findExtensions(plain, m.name);
                if (!ext.empty()) return callFunctions(ext, c, m.name, {}, &plain);
                for (const ExprPtr& a : c.args) expr(*a);
                return unknownType();
            }
            obj = *obj.bound;
        }
        Type plain = withNull(obj, false);
        if (obj.kind == Type::Class && obj.cls != nullptr) {
            ClassInfo* k = obj.cls;
            Member mem = lookupMember(plain, m.name, true);
            noteKeyMember(mem, m.name);
            // A member that cannot take a lambda gives way to an extension that can, as in Kotlin:
            // `obj.let { }` where obj has a let(int) of its own (D58).
            if (!mem.methods.empty() && c.args.size() == 1 && c.args[0]->kind == ExprKind::Function) {
                bool takes = false;
                for (FuncInfo* f : mem.methods) {
                    resolveFunc(f);
                    if (f->params.empty()) continue;
                    const Type& p = f->params[0];
                    takes = takes || p.kind == Type::Function || p.unknown() || p.kind == Type::Any || p.kind == Type::TypeParam;
                }
                if (!takes)
                    if (std::vector<FuncInfo*> ext = findExtensions(plain, m.name); !ext.empty()) return callFunctions(ext, c, m.name, {}, &plain);
            }
            if (!mem.methods.empty()) return callFunctions(mem.methods, c, k->name + "." + m.name, mem.env, nullptr, &mem.methodEnvs);
            if (mem.field || mem.getter) {
                if (c.isInfix) report(m, "'" + m.name + "' is a property, not an infix function (D72)");
                return callValue(memberOf(plain, m.name, m, false), c, m.name);
            }
            std::vector<FuncInfo*> ext = findExtensions(plain, m.name);
            if (!ext.empty()) return callFunctions(ext, c, m.name, {}, &plain);
            auto st = k->staticMethods.find(m.name);
            if (st != k->staticMethods.end()) return callFunctions(st->second, c, k->name + "." + m.name, {}, nullptr);
            for (const ExprPtr& a : c.args) expr(*a);
            if (reportMissing(k, m.name)) {
                std::string advice = (k->unit != nullptr && k->unit->prelude) ? goneAdvice(m.name) : std::string();
                if (k->unit != nullptr && k->unit->prelude && k->name == "Lock") advice = lockAdvice(m.name);
                if (advice.empty()) advice = preludeAdvice(k, m.name);
                report(m, advice.empty() ? k->name + " has no method '" + m.name + "'" : advice);
            }
            return unknownType();
        }
        std::vector<FuncInfo*> ext = findExtensions(plain, m.name);
        if (!ext.empty()) return callFunctions(ext, c, m.name, {}, &plain);
        for (const ExprPtr& a : c.args) expr(*a);
        if (builtinDeclared(plain) && !anyExtension(m.name) && !builtinMember(m.name)) {
            Type property;
            const std::string advice = m.name == "len" || m.name == "str" || m.name == "int" || m.name == "float" ? "" : goneAdvice(m.name);
            if (m.name == "toChar" && (plain.kind == Type::Int || plain.kind == Type::Number))
                report(m, "toChar() is Kotlin's; the character numbered n is n.char (D142)");
            else if (!advice.empty())
                report(m, advice);
            else if (!builtinProperty(plain, m.name, property)) report(m, describe(plain) + " has no method '" + m.name + "'");
        }
        return unknownType();
    }

    // Whether the methods of a built-in type are all declared (src/std/builtins.astra), so
    // one that is not there does not exist.
    bool builtinDeclared(const Type& t) const {
        switch (t.kind) {
            case Type::String:
            case Type::List:
            case Type::Map:
            case Type::Range:
            case Type::Bytes:
            case Type::Int:
            case Type::Float:
            case Type::Number:
            case Type::Char:
            case Type::Byte:
            case Type::Index:
            case Type::Set:
                return !builtinMethods.empty();
            default:
                return false;
        }
    }

    // Whether an extension on every value (`fun any.name`, `fun <T> T.name`) has this name.
    bool anyExtension(const std::string& name) const {
        for (const char* key : {"any", "<T>"}) {
            auto e = extensions.find(key);
            if (e != extensions.end() && e->second.count(name)) return true;
        }
        return false;
    }

    // ---- Bundles of named arguments (D143) ----

    // The names each `...bundle` of a call passes on, as the compiler expands it.
    std::unordered_map<const Expr*, std::vector<std::string>> bundleExpansions;
    // `params(...)` calls, `bundle.copy(...)` calls and `a + b` of bundles, for the compiler.
    std::unordered_set<const Node*> bundleMakes, bundleCopies, bundleAdds;

    // `params(name = value, ...)`, which makes a bundle, and `bundle.copy(name = value)`.
    bool bundleCall(const CallExpr& c, Type& result) {
        if (c.callee->kind == ExprKind::Name) {
            const auto& n = static_cast<const NameExpr&>(*c.callee);
            if (n.name != "params" || findLocal("params") != nullptr || unit == nullptr || findGlobal(*unit, "params") != nullptr) return false;
            result = Type::of(Type::Bundle);
            for (size_t i = 0; i < c.args.size(); i++) {
                const Type t = expr(*c.args[i]);
                const std::string name = i < c.names.size() ? c.names[i] : std::string();
                if (name.empty() || name == "...") {
                    report(*c.args[i], "params(...) holds named arguments: params(bold = true, size = 14); to add another bundle's, write a + b");
                    continue;
                }
                result.fields.push_back(name);
                result.args.push_back(t);
            }
            if (silent == 0) bundleMakes.insert(&c);
            return true;
        }
        if (c.callee->kind != ExprKind::Member) return false;
        const auto& m = static_cast<const MemberExpr&>(*c.callee);
        const Type on = withNull(typeQuietly(*m.object), false);
        if (on.kind != Type::Bundle) return false;
        expr(*m.object);
        if (m.name != "copy") {
            for (const ExprPtr& a : c.args) expr(*a);
            report(m, "a bundle has copy(name = value) and +, and is passed with ...: it has no '" + m.name + "'");
            result = unknownType();
            return true;
        }
        result = on;
        for (size_t i = 0; i < c.args.size(); i++) {
            const Type t = expr(*c.args[i]);
            const std::string name = i < c.names.size() ? c.names[i] : std::string();
            auto at = std::find(on.fields.begin(), on.fields.end(), name);
            if (name.empty() || name == "...") report(*c.args[i], "copy(...) on a bundle takes its names: style.copy(size = 18)");
            else if (at == on.fields.end()) report(*c.args[i], "the bundle has no '" + name + "' to change; add it with + params(" + name + " = ...)");
            else if (!fits(t, on.args[static_cast<size_t>(at - on.fields.begin())]))
                report(*c.args[i], "'" + name + "' in the bundle is " + describe(on.args[static_cast<size_t>(at - on.fields.begin())]) + ", not " + describe(t));
        }
        if (silent == 0) bundleCopies.insert(&c);
        return true;
    }

    Type call(const CallExpr& c) {
        if (Type changed; atomicCompareAndSet(c, changed)) return changed;
        if (Type made; bundleCall(c, made)) return made;
        if (c.callee->kind == ExprKind::Name) {
            const std::string& n = static_cast<const NameExpr&>(*c.callee).name;
            if (Local* l = findLocal(n)) {
                noteUse(*l, ParamUse::Called);
                if (l->paramOf != nullptr) noteParamCall(l->paramOf, l->paramIndex, c);
                // What a function given as an argument does, its caller accounts for.
                if (l->paramOf != nullptr) effects[effectKeyHere()].callsParams = true;
                else effects[effectKeyHere()].any = true;
                callingKnown = true;
                // A generic function declared here (G1): called as one, so that its type
                // arguments are worked out and handed to it.
                const Type lt = localType(n, *l);
                if (lt.kind == Type::Function && !lt.nullable && lt.fnRef != nullptr && !lt.fnRef->typeParams.empty() && l->paramOf == nullptr &&
                    lt.fnRef->decl != nullptr) {
                    noteValueCallThrows(lt, c);
                    Args args{c.args, c.names, c.trailingLambda, c.hasSpread};
                    return callWith(lt.fnRef, args, c.typeArgs, {}, nullptr, c, n + "(...)");
                }
                return callValue(lt, c, n);
            }
            if (ClassInfo* k = localClass(n); k != nullptr && !k->isObject) {
                Args args{c.args, c.names, c.trailingLambda, c.hasSpread};
                return constructorCall(k, args, c.typeArgs, c);
            }
            // A method of an object the code runs on.
            for (FnCtx* x = ctx; x != nullptr; x = x->isLambda ? x->parent : nullptr) {
                if (x->isLambda && x->receiverUnknown) {
                    for (const ExprPtr& a : c.args) expr(*a);
                    return unknownType();
                }
                if (x->hasReceiver) {
                    const Type& r = x->receiver;
                    if (r.kind == Type::Class && !r.nullable) {
                        Member mem = lookupMember(r, n, true);
                        // A private member is not what a bare name means in a block written outside its class.
                        if (mem.found() && hiddenHere(mem)) mem = Member{};
                        if (!mem.methods.empty()) return callFunctions(mem.methods, c, n, mem.env, nullptr, &mem.methodEnvs);
                        if (mem.field || mem.getter) return callValue(subst(mem.field ? fieldType(mem.field) : resultOf(mem.getter), mem.env), c, n);
                        std::vector<FuncInfo*> ext = findExtensions(r, n);
                        if (!ext.empty()) return callFunctions(ext, c, n, {}, &r);
                        if (!membersKnown(r)) {
                            for (const ExprPtr& a : c.args) expr(*a);
                            return unknownType();
                        }
                    } else {
                        std::vector<FuncInfo*> ext = findExtensions(withNull(r, false), n);
                        if (!ext.empty()) {
                            Type plain = withNull(r, false);
                            return callFunctions(ext, c, n, {}, &plain);
                        }
                        for (const ExprPtr& a : c.args) expr(*a);
                        return unknownType();
                    }
                }
                if (!x->isLambda) break;
            }
            if (ctx != nullptr && ctx->cls != nullptr) {
                if (!ctx->isStatic) {
                    Type self = selfType(ctx->cls);
                    Member mem = lookupMember(self, n, true);
                    noteKeyMember(mem, n);
                    if (!mem.methods.empty()) return callFunctions(mem.methods, c, n, mem.env, nullptr, &mem.methodEnvs);
                    if (mem.field || mem.getter)
                        return callValue(subst(mem.field ? fieldType(mem.field) : resultOf(mem.getter), mem.env), c, n);
                    std::vector<FuncInfo*> ext = findExtensions(self, n);
                    if (!ext.empty() && findGlobal(*unit, n) == nullptr) return callFunctions(ext, c, n, {}, &self);
                }
                for (ClassInfo* k = ctx->cls; k != nullptr; k = k->outer) {
                    auto s = k->staticMethods.find(n);
                    if (s != k->staticMethods.end()) return callFunctions(s->second, c, n, {}, nullptr);
                    auto nested = k->nested.find(n);
                    if (nested != k->nested.end() && !nested->second->isObject) {
                        Args args{c.args, c.names, c.trailingLambda, c.hasSpread};
                        return constructorCall(nested->second, args, c.typeArgs, c);
                    }
                }
            }
            if (const GlobalSym* g = findGlobal(*unit, n)) {
                if (g->cls != nullptr && !g->cls->isObject) {
                    Args args{c.args, c.names, c.trailingLambda, c.hasSpread};
                    return constructorCall(g->cls, args, c.typeArgs, c);
                }
                if (!g->funcs.empty()) return callFunctions(g->funcs, c, n, {}, nullptr);
                if (g->var != nullptr) return callValue(fieldType(g->var), c, n);
            }
            for (const ExprPtr& a : c.args) expr(*a);
            // The free conversion functions that went for members (D142, D74).
            if (n == "char" || n == "chr")
                report(c, n + "(n) is written n.char: the character numbered n, as c.code is the number of the character c");
            else if (n == "ord")
                report(c, "ord(c) is written c.code: the number of the character c, as n.char is the character numbered n");
            else if (n == "len" || n == "str" || n == "int" || n == "float")
                report(c, goneAdvice(n));
            else if (std::string advice = removedName(n); !advice.empty())
                report(c, advice);
            return unknownType();
        }
        if (c.callee->kind == ExprKind::Member) {
            const auto& m = static_cast<const MemberExpr&>(*c.callee);
            if (m.object->kind == ExprKind::Name) {
                const std::string& base = static_cast<const NameExpr&>(*m.object).name;
                if (findLocal(base) == nullptr && !nameIsMember(base)) {
                    if (Namespace* ns = findAlias(*unit, base)) {
                        if (const GlobalSym* g = findIn(ns, m.name)) {
                            if (g->cls != nullptr && !g->cls->isObject) {
                                Args args{c.args, c.names, c.trailingLambda, c.hasSpread};
                                return constructorCall(g->cls, args, c.typeArgs, c);
                            }
                            if (!g->funcs.empty()) return callFunctions(g->funcs, c, m.name, {}, nullptr);
                        }
                        for (const ExprPtr& a : c.args) expr(*a);
                        return unknownType();
                    }
                    if (ClassInfo* k = classNamed(base)) {
                        auto sm = k->staticMethods.find(m.name);
                        if (sm != k->staticMethods.end()) return callFunctions(sm->second, c, k->name + "." + m.name, {}, nullptr);
                        auto nested = k->nested.find(m.name);
                        if (nested != k->nested.end() && !nested->second->isObject) {
                            Args args{c.args, c.names, c.trailingLambda, c.hasSpread};
                            return constructorCall(nested->second, args, c.typeArgs, c);
                        }
                        for (const ExprPtr& a : c.args) expr(*a);
                        return unknownType();
                    }
                }
            }
            Type obj = expr(*m.object);
            if (obj.unknown()) {
                for (const ExprPtr& a : c.args) expr(*a);
                return unknownType();
            }
            if (obj.kind == Type::Nothing) return obj;
            if (obj.kind == Type::Null) {
                // `null to x`, `null.let { }`: a function for any T takes null, as in Kotlin.
                if (!m.safe) {
                    std::vector<FuncInfo*> any;
                    for (FuncInfo* f : findExtensions(Type::of(Type::Any), m.name)) {
                        resolveFunc(f);
                        if (f->isExtension && f->receiver.kind == Type::TypeParam && f->receiver.bound == nullptr) any.push_back(f);
                    }
                    if (!any.empty()) return callFunctions(any, c, m.name, {}, &obj);
                }
                for (const ExprPtr& a : c.args) expr(*a);
                return m.safe ? Type::of(Type::Null) : unknownType();
            }
            // `obj.block(...)`: a local of a function type run on an object, where the object has
            // no member of that name (D58).
            if (Local* l = findLocal(m.name)) {
                const Type ft = localType(m.name, *l);
                const Type plain = withNull(obj, false);
                if (ft.kind == Type::Function && ft.receiver && !ft.nullable &&
                    !(plain.kind == Type::Class && lookupMember(plain, m.name, true).found()) && findExtensions(plain, m.name).empty()) {
                    if (!fits(plain, *ft.receiver)) report(m, "'" + m.name + "' runs on " + describe(*ft.receiver) + ", not " + describe(plain));
                    noteUse(*l, ParamUse::Called);
                    if (l->paramOf != nullptr) effects[effectKeyHere()].callsParams = true;
                    else effects[effectKeyHere()].any = true;
                    callingKnown = true;
                    if (silent == 0) memberLocalCalls.insert(&c);
                    Type plainCall = ft;
                    plainCall.receiver.reset();
                    return callValue(plainCall, c, m.name);
                }
            }
            if (obj.nullable && !m.safe) {
                // `fun string?.isNullOrEmpty()`: an extension that takes null.
                Type plain = withNull(obj, false);
                std::vector<FuncInfo*> takesNull;
                for (FuncInfo* f : findExtensions(plain, m.name)) {
                    resolveFunc(f);
                    // `fun <T> T.let(...)`: T may stand for a type that takes null, as in Kotlin.
                    if (f->isExtension && (f->receiver.nullable || (f->receiver.kind == Type::TypeParam && f->receiver.bound == nullptr)))
                        takesNull.push_back(f);
                }
                if (takesNull.empty()) {
                    // `fun any?.isNullOrEmpty()`, which findExtensions passes over for a built-in type.
                    auto every = extensions.find("any");
                    if (every != extensions.end()) {
                        auto named = every->second.find(m.name);
                        if (named != every->second.end())
                            for (FuncInfo* f : named->second) {
                                resolveFunc(f);
                                if (f->receiver.nullable) takesNull.push_back(f);
                            }
                    }
                }
                if (!takesNull.empty()) return callFunctions(takesNull, c, m.name, {}, &obj);
            }
            if (nullUse(obj, m.name, m, m.safe, true)) {
                for (const ExprPtr& a : c.args) expr(*a);
                return unknownType();
            }
            if (obj.kind == Type::Any) {
                std::vector<FuncInfo*> ext = findExtensions(withNull(obj, false), m.name);
                if (!ext.empty() || builtinMember(m.name)) {
                    if (!ext.empty()) {
                        Type plain = withNull(obj, false);
                        return callFunctions(ext, c, m.name, {}, &plain);
                    }
                    for (const ExprPtr& a : c.args) expr(*a);
                    return unknownType();
                }
                report(m, "'" + m.name + "' cannot be called on a value of type any; cast it first (x as Type) or test it (if (x is Type))");
                for (const ExprPtr& a : c.args) expr(*a);
                return unknownType();
            }
            Type result = callMethod(obj, c, m);
            return m.safe ? withNull(result) : result;
        }
        if (c.callee->kind == ExprKind::Super) {
            const auto& s = static_cast<const SuperExpr&>(*c.callee);
            if (ctx != nullptr && ctx->cls != nullptr) {
                resolveSupers(ctx->cls);
                if (ctx->cls->superclass != nullptr) {
                    Type sup = selfTypeArgs(ctx->cls->superclass, ctx->cls->superArgs);
                    if (s.method == "init") {
                        Args args{c.args, c.names, c.trailingLambda, c.hasSpread};
                        superCall = true;
                        return constructorCall(ctx->cls->superclass, args, {}, c);
                    }
                    Member mem = lookupMember(sup, s.method);
                    if (!mem.methods.empty()) return callFunctions(mem.methods, c, "super." + s.method, mem.env, nullptr);
                }
            }
            for (const ExprPtr& a : c.args) expr(*a);
            return unknownType();
        }
        Type callee = expr(*c.callee);
        return callValue(callee, c, "");
    }

    // Calling a value whose type is a function.
    bool callingKnown = false;  // the call being checked is accounted for in effects already
    const Expr* valueCallArg = nullptr;  // an argument of a call of a function value, being checked

    Type callValue(const Type& t, const CallExpr& c, const std::string& name) {
        if (!std::exchange(callingKnown, false)) effects[effectKeyHere()].any = true;  // what it runs is not known
        noteValueCallThrows(t, c);
        if (t.unknown() || t.kind == Type::Class || t.kind == Type::TypeParam) {
            for (const ExprPtr& a : c.args) expr(*a);
            return unknownType();
        }
        const std::string who = name.empty() ? std::string("this value") : "'" + name + "'";
        if (t.kind == Type::Function && t.suspend) noteWait(c, name.empty() ? std::string("this function") : name);
        if (t.nullable) report(c, who + " may be null, so it cannot be called; check it first");
        else if (t.kind != Type::Function) report(c, who + " is " + describe(t) + ", which cannot be called");
        // `block(obj, ...)`: a function run on an object is given it first, as Kotlin has it (D58).
        if (t.kind == Type::Function && t.receiver && t.knownParams && !c.hasSpread && c.namedCount == 0 &&
            c.args.size() == t.args.size() + 1) {
            const Type given = exprExpected(*c.args[0], *t.receiver);
            if (!fits(given, *t.receiver)) report(*c.args[0], who + " runs on " + describe(*t.receiver) + ", not " + describe(given));
            for (size_t i = 1; i < c.args.size(); i++) {
                valueCallArg = c.args[i].get();
                const Type a = exprExpected(*c.args[i], t.args[i - 1]);
                valueCallArg = nullptr;
                if (!fits(a, t.args[i - 1])) report(*c.args[i], who + " takes " + describe(t.args[i - 1]) + " here, not " + describe(a));
            }
            if (silent == 0) onObjectCalls.insert(&c);
            return t.result ? *t.result : unknownType();
        }
        if (t.kind == Type::Function && t.knownParams && !c.hasSpread && c.namedCount == 0) {
            size_t least = t.required < 0 ? t.args.size() : static_cast<size_t>(t.required);
            // A lambda after the call goes to the last parameter; those between take their defaults.
            if (c.trailingLambda) least = std::min(least, c.args.size());
            if (c.args.size() < least || c.args.size() > t.args.size())
                report(c, who + " takes " + (least == t.args.size() ? std::string() : std::to_string(least) + " to ") +
                              std::to_string(t.args.size()) + " argument" + (t.args.size() == 1 ? "" : "s") + ", not " +
                              std::to_string(c.args.size()));
            for (size_t i = 0; i < c.args.size(); i++) {
                const bool last = c.trailingLambda && i + 1 == c.args.size() && !t.args.empty();
                const size_t p = last ? t.args.size() - 1 : i;
                Type want = p < t.args.size() ? t.args[p] : unknownType();
                valueCallArg = c.args[i].get();
                Type a = exprExpected(*c.args[i], want);
                valueCallArg = nullptr;
                if (p < t.args.size() && !fits(a, want))
                    report(*c.args[i], who + " takes " + describe(want) + " here, not " + describe(a));
            }
        } else {
            for (const ExprPtr& a : c.args) {
                valueCallArg = a.get();
                expr(*a);
                valueCallArg = nullptr;
            }
        }
        if (t.kind == Type::Function && t.result) return *t.result;
        return unknownType();
    }

    // ---- The whole program ----

    // Checks the files added since the last time: the standard library on its own first, then
    // a program and what it imports, then any worker's script the program names.
    size_t checkedUnits = 0;
    void runAll() {
        const size_t first = checkedUnits;
        checkedUnits = units.size();
        formsAboveKnown.clear();
        familyKnown.clear();
        for (size_t i = first; i < units.size(); i++) gather(*units[i]);
        for (size_t i = first; i < units.size(); i++)
            if (!units[i]->skipBodies) checkUnit(units[i].get());
        // The bodies of a kept file's inline functions are checked all the same: they are
        // copied into the code that calls them (D59).
        for (size_t i = first; i < units.size(); i++) {
            if (!units[i]->skipBodies) continue;
            eachFunction(*units[i]->statements, [&](const FunctionDecl& d) {
                if (!d.isInline) return;
                auto known = funcOf.find(&d);
                if (known != funcOf.end()) checkFunctionBody(known->second, nullptr);
            });
        }
        ctx = nullptr;
        judgeWaits();
        judgeKeys();
        judging.clear();
        for (size_t i = first; i < units.size(); i++) judging.insert(units[i].get());
        judgeInline();
        judgeThrows();
    }

    // A file's top level, in order, then its functions, classes and extensions.
    void checkUnit(Unit* u) {
        unit = u;
        FnCtx top;
        top.topLevel = true;
        ctx = &top;
        scopes.clear();
        narrowed.clear();
        tparams.clear();
        push();
        for (const StmtPtr& s : *u->statements) stmt(*s);
        pop();
        for (const StmtPtr& s : *u->statements) {
            if (s->kind == StmtKind::Function) {
                auto known = funcOf.find(static_cast<const FunctionStmt&>(*s).fn.get());
                if (known != funcOf.end()) checkFunctionBody(known->second, nullptr);
            } else if (s->kind == StmtKind::Class) {
                checkClass(classOf[static_cast<const ClassStmt*>(s.get())]);
            }
        }
        unit = u;
        ctx = &top;
        scopes.clear();
        push();
        for (const StmtPtr& s : *u->statements) {
            if (s->kind == StmtKind::Function) annotationsOfFunction(*static_cast<const FunctionStmt&>(*s).fn);
            else if (s->kind == StmtKind::Class) annotationsOfClass(static_cast<const ClassStmt&>(*s));
        }
        pop();
        ctx = nullptr;
    }

    // ---- Annotations (D153) ----

    // Where an annotation is written, for the standard library's, which go in one place only.
    enum class AnnotationPlace { Class, Function, Field, Parameter };

    void annotationsOfClass(const ClassStmt& c) {
        checkAnnotations(c.annotations, AnnotationPlace::Class);
        if (c.isAnnotation) annotationValues(c);
        for (const HeaderParam& p : c.header) checkAnnotations(p.annotations, p.isField ? AnnotationPlace::Field : AnnotationPlace::Parameter);
        for (const auto& field : c.fields) checkAnnotations(field->annotations, AnnotationPlace::Field);
        for (const auto& field : c.staticFields)
            if (!field->annotations.empty()) report(field->annotations[0], "an annotation on a static field is never read: reflection lists an object's members (D153)");
        for (const auto& method : c.methods) annotationsOfFunction(*method);
        for (const auto& nested : c.nested) annotationsOfClass(*nested);
    }

    void annotationsOfFunction(const FunctionDecl& fn) {
        checkAnnotations(fn.annotations, AnnotationPlace::Function);
        for (const Annotations& written : fn.paramAnnotations) checkAnnotations(written, AnnotationPlace::Parameter);
    }

    // What an annotation names: its class, or null where that is not an annotation.
    ClassInfo* annotationClass(const Annotation& a) {
        TypeAnnotation written;
        written.line = a.line;
        written.col = a.col;
        written.name = a.name;
        silent++;
        const Type t = resolve(written, *unit);
        silent--;
        return t.kind == Type::Class && t.cls != nullptr && t.cls->decl != nullptr && t.cls->decl->isAnnotation ? t.cls : nullptr;
    }

    void checkAnnotations(const Annotations& written, AnnotationPlace place) {
        for (const Annotation& a : written) {
            ClassInfo* k = annotationClass(a);
            if (k == nullptr) {
                report(a, "'" + a.name + "' is no annotation: one is declared annotation " + a.name + "(val value : T) (D153)");
                continue;
            }
            expr(*a.call);  // its arguments as a call to its class takes them
            for (const ExprPtr& arg : static_cast<const CallExpr&>(*a.call).args) annotationArgument(*arg);
            for (const Annotation& other : written)
                if (&other < &a && annotationClass(other) == k) report(a, "@" + a.name + " is written twice here");
            if (k->unit != nullptr && k->unit->prelude) {
                // The standard library's own, read by `astra test`, each for one place.
                const std::string& n = k->name;
                const bool onFunction = n == "Test" || n == "BeforeTest" || n == "AfterTest" || n == "BeforeAll" || n == "AfterAll" || n == "Timeout";
                if (onFunction && place != AnnotationPlace::Function) report(a, "@" + n + " goes on a function, for astra test (D153)");
                if (n == "TempDir" && place != AnnotationPlace::Parameter)
                    report(a, "@TempDir goes on a test's parameter, a Path given a directory of its own (D153)");
            }
        }
    }

    // What an annotation's argument may be: a constant, a class (`Box#`), an enum's entry,
    // another annotation, or a list of those, each known when compiling.
    void annotationArgument(const Expr& e) {
        if (foldQuietly(e).kind != ConstValue::None) return;
        if (e.kind == ExprKind::ClassOf && namedClasses.count(&e) != 0) return;
        if (e.kind == ExprKind::List) {
            for (const ExprPtr& item : static_cast<const ListExpr&>(e).items) annotationArgument(*item);
            return;
        }
        silent++;
        const Type t = expr(e);
        silent--;
        if ((e.kind == ExprKind::Name || e.kind == ExprKind::Member) && t.kind == Type::Class && t.cls != nullptr && t.cls->isEnum) return;
        if (e.kind == ExprKind::Call && t.kind == Type::Class && t.cls != nullptr && t.cls->decl != nullptr && t.cls->decl->isAnnotation) {
            for (const ExprPtr& arg : static_cast<const CallExpr&>(e).args) annotationArgument(*arg);
            return;
        }
        report(e, "an annotation is given constants: numbers, text, a class (Box#), an enum's entry, another annotation, or a list of those (D153)");
    }

    // What an annotation may hold: what its arguments may be.
    void annotationValues(const ClassStmt& c) {
        ClassInfo* k = classOf[&c];
        if (k == nullptr) return;
        std::function<bool(const Type&)> holdable = [&](const Type& t) {
            switch (t.kind) {
                case Type::Int:
                case Type::Float:
                case Type::Bool:
                case Type::String:
                case Type::Char:
                case Type::Byte: return t.storage == 0;
                case Type::List: return t.args.size() == 1 && holdable(t.args[0]);
                case Type::Class:
                    return t.cls != nullptr && (t.cls->isEnum || (t.cls->decl != nullptr && t.cls->decl->isAnnotation) ||
                                                (t.cls->name == "Class" && t.cls->unit != nullptr && t.cls->unit->prelude));
                default: return false;
            }
        };
        for (const HeaderParam& p : c.header) {
            auto f = k->fields.find(p.name);
            if (f == k->fields.end()) continue;
            const Type t = fieldType(f->second);
            if (t.known() && (t.nullable || !holdable(t)))
                report(p, "an annotation holds constants: an int, a float, a bool, a char, a byte, a string, a Class, an enum, another annotation, or a list of those; not " +
                              describe(t) + " (D153)");
        }
    }

    // ---- Files whose bodies are not checked again (D145) ----

    // A file's functions, in the order they are written: its own, and its classes' methods,
    // those of classes inside classes included. The same each time for the same text.
    static void eachFunction(const std::vector<StmtPtr>& statements, const std::function<void(const FunctionDecl&)>& visit) {
        std::function<void(const ClassStmt&)> inClass = [&](const ClassStmt& c) {
            for (const auto& m : c.methods) visit(*m);
            for (const auto& nested : c.nested) inClass(*nested);
        };
        for (const StmtPtr& s : statements) {
            if (s->kind == StmtKind::Function) visit(*static_cast<const FunctionStmt&>(*s).fn);
            else if (s->kind == StmtKind::Class) inClass(static_cast<const ClassStmt&>(*s));
        }
    }

    Unit* unitByKey(const std::string& fileKey) {
        auto found = unitsByKey.find(fileKey);
        return found == unitsByKey.end() ? nullptr : found->second;
    }

    // Whether a function of a file whose bodies were not checked runs what is passed for a
    // parameter there and then, as its kept facts say.
    std::unordered_map<const FunctionDecl*, std::string> keptInPlace;
    std::unordered_set<Unit*> keptInPlaceRead;
    bool keptFact(FuncInfo* f, int param) {
        Unit* u = f->unit;
        if (keptInPlaceRead.insert(u).second) {
            size_t i = 0;
            eachFunction(*u->statements, [&](const FunctionDecl& d) {
                if (i < u->inPlaceFacts.size()) keptInPlace[&d] = u->inPlaceFacts[i];
                i++;
            });
        }
        auto found = keptInPlace.find(f->decl);
        if (found == keptInPlace.end()) return false;
        const size_t end = found->second.find('!');
        const std::string marks = found->second.substr(0, end);
        return static_cast<size_t>(param) < marks.size() && marks[static_cast<size_t>(param)] == '1';
    }

    // What a function of a file whose bodies were not checked may throw, as its kept facts
    // say: after a '!', each class as its file and name (D35).
    std::vector<ClassInfo*> keptThrown(FuncInfo* f) {
        std::vector<ClassInfo*> list;
        keptFact(f, 0);  // reads the file's facts, if not yet
        auto found = keptInPlace.find(f->decl);
        if (found == keptInPlace.end()) return list;
        const size_t bang = found->second.find('!');
        if (bang == std::string::npos) return list;
        std::istringstream items(found->second.substr(bang + 1));
        for (std::string item; std::getline(items, item, ';');)
            if (ClassInfo* k = classByKey(item)) list.push_back(k);
        return list;
    }
    static std::string classKey(const ClassInfo* k) { return (k->unit != nullptr ? k->unit->fileKey : std::string()) + "\t" + k->name; }
    ClassInfo* classByKey(const std::string& key) {
        const size_t tab = key.find('\t');
        if (tab == std::string::npos) return nullptr;
        Unit* u = unitByKey(key.substr(0, tab));
        if (u == nullptr) return nullptr;
        std::istringstream parts(key.substr(tab + 1));
        ClassInfo* k = nullptr;
        for (std::string part; std::getline(parts, part, '.');) {
            if (k == nullptr) {
                const GlobalSym* g = findGlobal(*u, part);
                k = g != nullptr ? g->cls : nullptr;
            } else {
                auto inner = k->nested.find(part);
                k = inner != k->nested.end() ? inner->second : nullptr;
            }
            if (k == nullptr) return nullptr;
        }
        return k;
    }
    // A function's line of facts: its parameters run in place, then what it may throw.
    std::string throwsFact(FuncInfo* f) {
        auto found = thrownBy.find(f->decl);
        if (found == thrownBy.end() || found->second.classes.empty()) return {};
        std::string s = "!";
        for (ClassInfo* k : found->second.classes) s += classKey(k) + ";";
        return s;
    }

    std::string inPlaceFactsOf(const std::string& fileKey) {
        Unit* u = unitByKey(fileKey);
        if (u == nullptr) return {};
        std::string facts;
        eachFunction(*u->statements, [&](const FunctionDecl& d) {
            auto known = funcOf.find(&d);
            std::string line;
            if (known != funcOf.end()) {
                FuncInfo* f = known->second;
                resolveFunc(f);
                for (size_t p = 0; p < f->params.size(); p++)
                    line += f->params[p].kind != Type::Function ? '.' : inPlace(f, static_cast<int>(p)) ? '1' : '0';
                line += throwsFact(f);
            }
            facts += line + "\n";
        });
        return facts;
    }

    // ---- Constants: `const` (R2, D139) ----
    //
    // A constant's value is worked out when the program is checked, from literals and other
    // constants, with what the VM does to them (ints wrapping round, `/` on two ints dropping
    // the fraction, numbers joined to text as print shows them), and the compiler writes it in
    // place of each read.

    std::unordered_map<const Expr*, FieldInfo*> constUses;  // a read of a constant: which
    std::unordered_map<const VarStmt*, std::pair<int, ConstValue>> constValues;  // 1 being worked out, 2 known
    int workingOutConst = 0;

    FieldInfo* noteConst(const Expr& at, FieldInfo* f) {
        if (f == nullptr || f->decl == nullptr || (silent > 0 && workingOutConst == 0)) return f;
        // A read that the access words refuse is left to be refused when the program runs.
        if (!readableHere(f)) return f;
        if (f->decl->isConst) constUses[&at] = f;
        else if (foldableVal(f) && silent == 0) valUses[&at] = f->decl;
        return f;
    }

    // Whether code here may read a field, so that folding it skips no check: a member that is
    // public, or read from inside its class; a top-level name that is public, or of this file.
    bool readableHere(const FieldInfo* f) const {
        if (f->owner == nullptr) return f->decl->visibility == 0 || f->unit == unit;
        const uint8_t access = f->decl->access;
        if (access == kAccessWordNone || access == kAccessWordPublic) return true;
        for (ClassInfo* c = classAround(); c != nullptr; c = c->outer)
            if (c == f->owner) return true;
        return false;
    }

    // A val that is the same for every read, so that a constant value may be folded: one of
    // the top level, an object or a class's static code. An instance field of a class may be
    // overridden, and one with an accessor is worked out each time.
    bool foldableVal(const FieldInfo* f) const {
        const VarStmt& d = *f->decl;
        if (!d.isVal || d.isConst || d.init == nullptr || d.getter != nullptr || d.isExternal || !d.pattern.empty()) return false;
        if (f->owner == nullptr || f->owner->isObject) return true;
        auto st = f->owner->statics.find(f->name);
        return st != f->owner->statics.end() && st->second == f;
    }

    // Reads of vals that may be folded, and what each val came to (1 being worked out, 2 known).
    std::unordered_map<const Expr*, const VarStmt*> valUses;
    std::unordered_map<const VarStmt*, std::pair<int, ConstValue>> valValues;

    // A constant int kept where a byte is declared is a byte (D19), read as one.
    static ConstValue asDeclared(const VarStmt& d, ConstValue v) {
        if (v.kind == ConstValue::Int && d.type.present() && d.type.name == "byte" && !d.type.nullable && v.i >= 0 && v.i <= 255)
            v.kind = ConstValue::Byte;
        return v;
    }

    ConstValue valueOfVal(const VarStmt* d) {
        auto known = valValues.find(d);
        if (known != valValues.end()) return known->second.first == 2 ? known->second.second : ConstValue{};
        valValues[d].first = 1;
        ConstValue v = asDeclared(*d, foldQuietly(*d->init));
        valValues[d] = {2, v};
        return v;
    }

    ConstValue constantOf(FieldInfo* f) {
        auto known = constValues.find(f->decl);
        if (known != constValues.end()) {
            if (known->second.first == 2) return known->second.second;
            report(*f->decl, "const '" + f->name + "' is worked out from itself");
            return ConstValue{};
        }
        constValues[f->decl].first = 1;
        ConstValue v;
        // Its value is read in its own place, so that the constants it names are known.
        Unit* savedUnit = unit;
        FnCtx* savedCtx = ctx;
        auto savedScopes = std::move(scopes);
        auto savedNarrowed = std::move(narrowed);
        TParamsSaved savedParams(*this);
        scopes.clear();
        narrowed.clear();
        unit = f->unit;
        FnCtx temp;
        temp.cls = f->owner;
        temp.topLevel = f->owner == nullptr;
        if (f->owner != nullptr) {
            auto st = f->owner->statics.find(f->name);
            temp.isStatic = st != f->owner->statics.end() && st->second == f;
        }
        ctx = &temp;
        pushClassTypeParams(f->owner);
        silent++;
        workingOutConst++;
        push();
        expr(*f->decl->init);
        pop();
        workingOutConst--;
        silent--;
        v = asDeclared(*f->decl, constEval(*f->decl->init, f->name));
        unit = savedUnit;
        ctx = savedCtx;
        scopes = std::move(savedScopes);
        narrowed = std::move(savedNarrowed);
        constValues[f->decl] = {2, v};
        return v;
    }

    // Folding an expression outside a constant's value: nothing is reported, the constants it
    // names are those already worked out, and what was found is kept.
    int quietFold = 0;
    std::unordered_map<const Expr*, ConstValue> foldedKnown;

    ConstValue foldQuietly(const Expr& e) {
        auto known = foldedKnown.find(&e);
        if (known != foldedKnown.end()) return known->second;
        quietFold++;
        ConstValue v = constEval(e, std::string());
        quietFold--;
        foldedKnown[&e] = v;
        return v;
    }

    ConstValue notConstant(const Expr& at, const std::string& of, const std::string& why) {
        if (quietFold > 0) return ConstValue{};
        report(at, "const '" + of + "' is worked out when compiling, from literals and other constants; " + why);
        return ConstValue{};
    }

    static ConstValue constInt(int64_t i) {
        ConstValue v;
        v.kind = ConstValue::Int;
        v.i = i;
        return v;
    }
    static ConstValue constFloat(double d) {
        ConstValue v;
        v.kind = ConstValue::Float;
        v.d = d;
        return v;
    }
    static ConstValue constBool(bool b) {
        ConstValue v;
        v.kind = ConstValue::Bool;
        v.b = b;
        return v;
    }
    static ConstValue constString(std::string s) {
        ConstValue v;
        v.kind = ConstValue::String;
        v.s = std::move(s);
        return v;
    }
    // As print shows it, and as `+` joins it to text.
    static std::string constText(const ConstValue& v) {
        switch (v.kind) {
            case ConstValue::Int: return std::to_string(v.i);
            case ConstValue::Float: return formatFloat(v.d);
            case ConstValue::Bool: return v.b ? "true" : "false";
            case ConstValue::String: return v.s;
            case ConstValue::Char: {
                std::string text;
                appendUtf8(text, static_cast<uint32_t>(v.i));
                return text;
            }
            case ConstValue::Byte: return std::to_string(v.i);
            default: return "";
        }
    }
    static bool isNumberConst(const ConstValue& v) { return v.kind == ConstValue::Int || v.kind == ConstValue::Float; }
    static double asDouble(const ConstValue& v) { return v.kind == ConstValue::Int ? static_cast<double>(v.i) : v.d; }

    // The value of a constant's expression; None where it is not constant (reported) or does
    // not fit together (reported by the checking of types).
    ConstValue constEval(const Expr& e, const std::string& of) {
        switch (e.kind) {
            case ExprKind::Literal: {
                const auto& l = static_cast<const LiteralExpr&>(e);
                switch (l.type) {
                    case LiteralExpr::Type::Bool: return constBool(l.boolValue);
                    case LiteralExpr::Type::Int: return constInt(l.intValue);
                    case LiteralExpr::Type::Float: return constFloat(l.floatValue);
                    case LiteralExpr::Type::String: return constString(l.stringValue);
                    case LiteralExpr::Type::Char: {
                        ConstValue c;
                        c.kind = ConstValue::Char;
                        c.i = l.intValue;
                        return c;
                    }
                    default: return notConstant(e, of, "null is not a constant's value");
                }
            }
            case ExprKind::Interp: {
                std::string joined;
                for (const ExprPtr& part : static_cast<const InterpExpr&>(e).parts) {
                    ConstValue p = constEval(*part, of);
                    if (p.kind == ConstValue::None) return ConstValue{};
                    joined += constText(p);
                }
                return constString(std::move(joined));
            }
            case ExprKind::Name:
            case ExprKind::Member: {
                if (e.kind == ExprKind::Member && static_cast<const MemberExpr&>(e).object->kind == ExprKind::Name) {
                    // `int.maxValue` and the like (D76).
                    const auto& m = static_cast<const MemberExpr&>(e);
                    const std::string& base = static_cast<const NameExpr&>(*m.object).name;
                    ConstValue limit;
                    if ((base == "int" || base == "float") && numberLimit(base, m.name, limit)) return limit;
                }
                auto use = constUses.find(&e);
                if (use != constUses.end() && quietFold > 0) {
                    auto known = constValues.find(use->second->decl);
                    return known != constValues.end() && known->second.first == 2 ? known->second.second : ConstValue{};
                }
                // Folding outside a constant's value, a val whose value is constant counts too.
                if (quietFold > 0) {
                    auto val = valUses.find(&e);
                    return val != valUses.end() ? valueOfVal(val->second) : ConstValue{};
                }
                if (use != constUses.end()) return constantOf(use->second);
                const std::string written = placeText(e);
                return notConstant(e, of, "'" + written + "' is not a constant");
            }
            case ExprKind::Unary: {
                const auto& u = static_cast<const UnaryExpr&>(e);
                ConstValue a = quietFold > 0 ? foldQuietly(*u.operand) : constEval(*u.operand, of);
                if (a.kind == ConstValue::None) return a;
                if (a.kind == ConstValue::Byte) a.kind = ConstValue::Int;
                if (u.op == TokenType::Minus && a.kind == ConstValue::Int) {
                    if (a.i == INT64_MIN) return notConstant(e, of, "its negation does not fit in an int");  // (D27)
                    return constInt(-a.i);
                }
                if (u.op == TokenType::Minus && a.kind == ConstValue::Float) return constFloat(-a.d);
                if (u.op == TokenType::Bang && a.kind == ConstValue::Bool) return constBool(!a.b);
                if (u.op == TokenType::Tilde && a.kind == ConstValue::Int) return constInt(~a.i);
                return ConstValue{};
            }
            case ExprKind::Logical: {
                const auto& l = static_cast<const LogicalExpr&>(e);
                ConstValue a = quietFold > 0 ? foldQuietly(*l.left) : constEval(*l.left, of);
                ConstValue b = quietFold > 0 ? foldQuietly(*l.right) : constEval(*l.right, of);
                if (a.kind != ConstValue::Bool || b.kind != ConstValue::Bool) return ConstValue{};
                return constBool(l.isAnd ? (a.b && b.b) : (a.b || b.b));
            }
            case ExprKind::Binary: return constBinary(static_cast<const BinaryExpr&>(e), of);
            case ExprKind::Call: return notConstant(e, of, "a call is made when the program runs");
            case ExprKind::List:
            case ExprKind::Map:
            case ExprKind::Range: return notConstant(e, of, "a collection is made when the program runs");
            default: return notConstant(e, of, "this is worked out when the program runs");
        }
    }

    // Whether an int sum, difference or product leaves the 64 bits an int has (D27); its
    // wrapped result in `r` either way.
    static bool intOverflows(TokenType op, int64_t a, int64_t b, int64_t& r) {
        const uint64_t x = static_cast<uint64_t>(a), y = static_cast<uint64_t>(b);
        switch (op) {
            case TokenType::Plus:
                r = static_cast<int64_t>(x + y);
                return ((a ^ r) & (b ^ r)) < 0;
            case TokenType::Minus:
                r = static_cast<int64_t>(x - y);
                return ((a ^ b) & (a ^ r)) < 0;
            default:
                r = static_cast<int64_t>(x * y);
                if (a == -1) return b == INT64_MIN;  // (and dividing by -1 below would overflow itself)
                return a != 0 && r / a != b;
        }
    }

    // Constants with a char among them (D25).
    ConstValue constChars(const BinaryExpr& e, const ConstValue& a, const ConstValue& b, const std::string& of) {
        auto character = [&](int64_t c) {
            if (!isCharacter(c)) return notConstant(e, of, "it is not a character: characters are numbered 0 to 1114111, leaving out 55296 to 57343");
            ConstValue v;
            v.kind = ConstValue::Char;
            v.i = c;
            return v;
        };
        const bool ac = a.kind == ConstValue::Char, bc = b.kind == ConstValue::Char;
        switch (e.op) {
            case TokenType::Plus:
                if (a.kind == ConstValue::String || b.kind == ConstValue::String || (ac && bc)) return constString(constText(a) + constText(b));
                if (ac && b.kind == ConstValue::Int) return character(a.i + b.i);
                if (a.kind == ConstValue::Int && bc) return constInt(a.i + b.i);
                return ConstValue{};
            case TokenType::Minus:
                if (ac && b.kind == ConstValue::Int) return character(a.i - b.i);
                if (ac && bc) return constInt(a.i - b.i);
                if (a.kind == ConstValue::Int && bc) return constInt(a.i - b.i);
                return ConstValue{};
            case TokenType::EqEq:
            case TokenType::BangEq: {
                const ConstValue& c = ac ? a : b;
                const ConstValue& other = ac ? b : a;
                bool same = false;
                if (other.kind == ConstValue::Char) same = other.i == c.i;
                else if (other.kind == ConstValue::String) same = other.s == constText(c);
                return constBool(e.op == TokenType::EqEq ? same : !same);
            }
            case TokenType::Lt:
            case TokenType::LtEq:
            case TokenType::Gt:
            case TokenType::GtEq:
                if (!ac || !bc) return ConstValue{};
                if (e.op == TokenType::Lt) return constBool(a.i < b.i);
                if (e.op == TokenType::LtEq) return constBool(a.i <= b.i);
                if (e.op == TokenType::Gt) return constBool(a.i > b.i);
                return constBool(a.i >= b.i);
            default:
                return ConstValue{};
        }
    }

    ConstValue constBinary(const BinaryExpr& e, const std::string& of) {
        ConstValue a = quietFold > 0 ? foldQuietly(*e.left) : constEval(*e.left, of);
        ConstValue b = quietFold > 0 ? foldQuietly(*e.right) : constEval(*e.right, of);
        if (a.kind == ConstValue::None || b.kind == ConstValue::None) return ConstValue{};
        // A byte in arithmetic is the int it holds (D19).
        if (a.kind == ConstValue::Byte) a.kind = ConstValue::Int;
        if (b.kind == ConstValue::Byte) b.kind = ConstValue::Int;
        if (a.kind == ConstValue::Char || b.kind == ConstValue::Char) return constChars(e, a, b, of);
        const bool ints = a.kind == ConstValue::Int && b.kind == ConstValue::Int;
        const bool numbers = isNumberConst(a) && isNumberConst(b);
        const uint64_t x = static_cast<uint64_t>(a.i), y = static_cast<uint64_t>(b.i);
        switch (e.op) {
            case TokenType::Plus:
                if (a.kind == ConstValue::String || b.kind == ConstValue::String) return constString(constText(a) + constText(b));
                if (ints) {
                    int64_t r;
                    if (intOverflows(TokenType::Plus, a.i, b.i, r)) return notConstant(e, of, "its sum does not fit in an int");
                    return constInt(r);
                }
                if (numbers) return constFloat(asDouble(a) + asDouble(b));
                return ConstValue{};
            case TokenType::Minus:
                if (ints) {
                    int64_t r;
                    if (intOverflows(TokenType::Minus, a.i, b.i, r)) return notConstant(e, of, "its difference does not fit in an int");
                    return constInt(r);
                }
                if (numbers) return constFloat(asDouble(a) - asDouble(b));
                return ConstValue{};
            case TokenType::Star:
                if (ints) {
                    int64_t r;
                    if (intOverflows(TokenType::Star, a.i, b.i, r)) return notConstant(e, of, "its product does not fit in an int");
                    return constInt(r);
                }
                if (numbers) return constFloat(asDouble(a) * asDouble(b));
                return ConstValue{};
            case TokenType::AmpPlus: return ints ? constInt(static_cast<int64_t>(x + y)) : ConstValue{};
            case TokenType::AmpMinus: return ints ? constInt(static_cast<int64_t>(x - y)) : ConstValue{};
            case TokenType::AmpStar: return ints ? constInt(static_cast<int64_t>(x * y)) : ConstValue{};
            case TokenType::Slash:
                if (ints) {
                    if (b.i == 0) return notConstant(e, of, "this divides by zero");
                    if (b.i == -1 && a.i == INT64_MIN) return notConstant(e, of, "its quotient does not fit in an int");
                    return constInt(b.i == -1 ? static_cast<int64_t>(0 - x) : a.i / b.i);
                }
                if (numbers) return constFloat(asDouble(a) / asDouble(b));
                return ConstValue{};
            case TokenType::Percent:
                if (ints) {
                    if (b.i == 0) return notConstant(e, of, "this divides by zero");
                    return constInt(b.i == -1 ? 0 : a.i % b.i);
                }
                if (numbers) return constFloat(std::fmod(asDouble(a), asDouble(b)));
                return ConstValue{};
            case TokenType::Lt:
            case TokenType::LtEq:
            case TokenType::Gt:
            case TokenType::GtEq: {
                int order;
                if (ints) order = a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
                else if (numbers) order = asDouble(a) < asDouble(b) ? -1 : asDouble(a) > asDouble(b) ? 1 : 0;
                else if (a.kind == ConstValue::String && b.kind == ConstValue::String) order = a.s.compare(b.s);
                else return ConstValue{};
                if (e.op == TokenType::Lt) return constBool(order < 0);
                if (e.op == TokenType::LtEq) return constBool(order <= 0);
                if (e.op == TokenType::Gt) return constBool(order > 0);
                return constBool(order >= 0);
            }
            case TokenType::EqEq:
            case TokenType::BangEq: {
                bool same;
                if (numbers && !ints) same = asDouble(a) == asDouble(b);
                else if (a.kind != b.kind) same = false;
                else if (ints) same = a.i == b.i;
                else if (a.kind == ConstValue::Bool) same = a.b == b.b;
                else same = a.s == b.s;
                return constBool(e.op == TokenType::EqEq ? same : !same);
            }
            case TokenType::Amp: return ints ? constInt(a.i & b.i) : ConstValue{};
            case TokenType::Pipe: return ints ? constInt(a.i | b.i) : ConstValue{};
            case TokenType::Caret: return ints ? constInt(std::bit_xor<int64_t>()(a.i, b.i)) : ConstValue{};
            case TokenType::Shl:
            case TokenType::Shr:
            case TokenType::Ushr: {
                if (!ints) return ConstValue{};
                if (b.i < 0) return notConstant(e, of, "it shifts by a negative amount");
                if (e.op == TokenType::Shl) return constInt(b.i >= 64 ? 0 : static_cast<int64_t>(x << b.i));
                if (e.op == TokenType::Ushr) return constInt(b.i >= 64 ? 0 : static_cast<int64_t>(x >> b.i));
                if (b.i >= 64) return constInt(a.i < 0 ? -1 : 0);
                return constInt(a.i < 0 ? ~(~a.i >> b.i) : a.i >> b.i);
            }
            default:
                return notConstant(e, of, "this is worked out when the program runs");
        }
    }

    // ---- Waiting: `suspend` (R1, D30) ----
    //
    // A call of a suspend function may stand where its code may wait: in a suspend function, at
    // the top level, in a lambda given a suspend type (what `startCoroutine { }` and `thread { }`
    // take), or in a lambda run in place by a function that only calls it, which may wait where
    // that call may (D59's rule, applied to checking). Nothing waits in a getter or setter,
    // toString, ==, hashCode, the making of an object, a sort's comparison or a parallel section.
    // Whether a function only calls a lambda is known once its body has been checked, so the
    // calls are judged after the whole program.

    // What a lambda is passed to: a function's parameter, or a scope function the compiler and
    // the VM run in place.
    struct LambdaSite {
        FuncInfo* fn = nullptr;
        int param = -1;
        bool inPlace = false;
        NoWait noWait = CanWait;
    };
    std::unordered_map<const FunctionDecl*, LambdaSite> lambdaSites;

    // One use of a parameter of a function type, in its function's body.
    struct ParamUse {
        enum Kind : uint8_t { Called, PassedOn, Other };
        Kind kind = Other;
        FuncInfo* to = nullptr;  // PassedOn: where it is passed
        int toParam = -1;
        std::vector<const FunctionDecl*> lambdas;  // the lambdas the use is inside, within the function
    };
    std::map<std::pair<FuncInfo*, int>, std::vector<ParamUse>> paramUses;
    const Expr* passedUse = nullptr;  // the argument being checked, already noted as passed on

    struct WaitFrame {
        const FunctionDecl* lambda = nullptr;
        bool suspendLambda = false;
        FuncInfo* fn = nullptr;
        bool topLevel = false;
        NoWait noWait = CanWait;
    };
    struct WaitingCall {
        Unit* unit = nullptr;
        const Node* at = nullptr;
        std::string callee;
        bool gives = false;             // a sequence's yield, or a suspend function of SequenceScope's
        std::vector<WaitFrame> frames;  // the code around the call, innermost first
    };
    std::vector<WaitingCall> waitingCalls;
    std::map<std::pair<FuncInfo*, int>, int> inPlaceKnown;  // 1 being worked out, 2 yes, 3 no
    std::unordered_map<FuncInfo*, std::vector<FuncInfo*>> overriders;
    bool overridersKnown = false;

    static NoWait noWaitOf(const FunctionDecl& d, bool method) {
        if (d.accessor == FunctionDecl::Accessor::Get) return Getter;
        if (d.accessor == FunctionDecl::Accessor::Set) return Setter;
        if (!method) return CanWait;
        if (d.name == "toString" && d.params.empty()) return ToString;
        if (d.name == "==") return Equals;
        if (d.name == "hashCode" && d.params.empty()) return HashCode;
        if (d.name == "init") return ObjectMade;
        return CanWait;
    }

    static std::string noWaitReason(NoWait why, const std::string& name) {
        switch (why) {
            case Getter: return "a getter must give its value at once: whoever reads '" + name + "' would be held up";
            case Setter: return "a setter must finish at once: whoever sets '" + name + "' would be held up";
            case ToString: return "toString must give its text at once: whoever prints the object would be held up";
            case Equals: return "== must give its answer at once: every comparison would be held up";
            case HashCode: return "hashCode must give its answer at once: every map and set holding the object would be held up";
            case ObjectMade: return "an object is made at once: whoever makes it would be held up";
            case StaticValue: return "a static value must be there at once: the class is being set up";
            case LazyValue: return "a lazy starting value must be there at once: whoever first reads it would be held up";
            case SortComparison: return "a sort's comparison must give its answer at once: the sort cannot wait in the middle";
            case ParallelSection: return "nothing waits in a parallel section: its work runs straight through on several cores";
            case SequenceBlock:
                return "a sequence's block waits only where it gives a value (yield, yieldAll): its values are worked out as they are asked for, "
                       "by whoever asks";
            default: return "";
        }
    }

    // A lambda written as an argument: what it is passed to.
    void noteLambdaSite(const Expr& e, FuncInfo* f, int param) {
        if (silent > 0) return;
        LambdaSite site;
        site.fn = f;
        site.param = param;
        if (f->unit != nullptr && f->unit->prelude) {
            static const std::unordered_set<std::string> sorts = {
                "sort", "sortWith", "sortBy", "sortByDescending", "sortedWith", "sortedBy", "sortedByDescending",
                "compareBy", "compareByDescending", "thenBy", "thenByDescending"};
            if (sorts.count(f->name)) site.noWait = SortComparison;
            else if ((f->owner != nullptr && f->owner->name == "ParallelStream") || f->name == "parallelChunks" || f->name == "parallel")
                site.noWait = ParallelSection;
            else if (f->name == "sequence" && f->owner == nullptr)
                site.noWait = SequenceBlock;
        }
        lambdaSites[static_cast<const FunctionExpr&>(e).fn.get()] = site;
    }

    void noteUse(const Local& l, ParamUse::Kind kind, FuncInfo* to = nullptr, int toParam = -1) {
        if (silent > 0 || l.paramOf == nullptr) return;
        ParamUse use;
        use.kind = kind;
        use.to = to;
        use.toParam = toParam;
        bool found = false;
        for (FnCtx* x = ctx; x != nullptr; x = x->parent) {
            if (x->placeDecl == nullptr) {
                found = x->fn == l.paramOf;
                break;
            }
            use.lambdas.push_back(x->placeDecl);
        }
        if (!found) use.kind = ParamUse::Other;  // reached from a function of its own, which may keep it
        paramUses[{l.paramOf, l.paramIndex}].push_back(std::move(use));
    }

    // Whether a function is one a sequence's block gives its values with (D98): yield, yieldAll,
    // or a suspend function of SequenceScope's of the program's own.
    static bool givesToSequence(const FuncInfo* f) {
        if (f->owner != nullptr) return f->owner->name == "SequenceScope";
        return f->isExtension && f->receiver.kind == Type::Class && f->receiver.cls != nullptr && f->receiver.cls->name == "SequenceScope";
    }

    void noteWait(const Node& at, const std::string& callee, bool gives = false) {
        // While it waits, other code runs, which may change any list (M6.6 part 4): an item is not
        // read where it is across the wait.
        effects[effectKeyHere()].any = true;
        if (silent > 0 || unit == nullptr) return;
        WaitingCall w;
        w.unit = unit;
        w.at = &at;
        w.callee = callee;
        w.gives = gives;
        for (FnCtx* x = ctx; x != nullptr; x = x->parent) {
            WaitFrame f;
            f.lambda = x->placeDecl;
            f.suspendLambda = x->suspendLambda;
            f.fn = x->fn;
            f.topLevel = x->topLevel;
            f.noWait = x->noWait;
            w.frames.push_back(f);
            if (x->placeDecl == nullptr) break;
        }
        waitingCalls.push_back(std::move(w));
    }

    // The methods that take the place of one, in the classes below its own.
    const std::vector<FuncInfo*>& overridersOf(FuncInfo* f) {
        if (!overridersKnown) {
            overridersKnown = true;
            for (const auto& [decl, g] : funcOf) {
                if (g->owner == nullptr || g->isStatic || g->decl == nullptr || !g->decl->isOverride) continue;
                for (FuncInfo* above : formsAbove(g->owner, g->name))
                    if (above != g && takesPlaceOf(g, above)) overriders[above].push_back(g);
            }
        }
        static const std::vector<FuncInfo*> none;
        auto found = overriders.find(f);
        return found == overriders.end() ? none : found->second;
    }

    // Whether a function only calls what is passed for one of its parameters, there and then:
    // calls it, or passes it on to a function that does, and nothing else; and so does every
    // method that takes its place.
    bool inPlace(FuncInfo* f, int param) {
        if (f == nullptr || param < 0) return false;
        if (f->isExternal) return f->name == "with" && f->unit != nullptr && f->unit->prelude;
        if (f->unit != nullptr && f->unit->skipBodies) return keptFact(f, param);
        if (f->decl == nullptr || param == f->vararg) return false;
        resolveFunc(f);
        // Only a parameter of a function type is followed; anything else may keep what it is given.
        if (static_cast<size_t>(param) >= f->params.size() || f->params[static_cast<size_t>(param)].kind != Type::Function) return false;
        const auto key = std::make_pair(f, param);
        auto known = inPlaceKnown.find(key);
        if (known != inPlaceKnown.end()) return known->second != 3;  // being worked out: so far, yes
        inPlaceKnown[key] = 1;
        bool yes = !f->decl->isAbstract;
        if (yes) {
            auto uses = paramUses.find(key);
            if (uses != paramUses.end())
                for (const ParamUse& u : uses->second) {
                    if (u.kind == ParamUse::Other || (u.kind == ParamUse::PassedOn && !inPlace(u.to, u.toParam))) yes = false;
                    for (const FunctionDecl* l : u.lambdas) yes = yes && lambdaInPlace(l);
                    if (!yes) break;
                }
        }
        if (f->owner != nullptr && !f->isStatic) {
            const std::vector<FuncInfo*>& takingPlace = overridersOf(f);
            if (f->decl->isAbstract) yes = !takingPlace.empty();
            for (FuncInfo* g : takingPlace) yes = yes && inPlace(g, param);
        }
        inPlaceKnown[key] = yes ? 2 : 3;
        return yes;
    }

    bool lambdaInPlace(const FunctionDecl* d) {
        auto site = lambdaSites.find(d);
        if (site == lambdaSites.end()) return false;
        return site->second.inPlace || inPlace(site->second.fn, site->second.param);
    }

    // A function as a message names it: a constructor by its class.
    static std::string calledName(const FuncInfo* f) {
        return f->name == "init" && f->owner != nullptr ? f->owner->name : f->name;
    }

    void judgeWaits() {
        inPlaceKnown.clear();
        overriders.clear();
        overridersKnown = false;
        Unit* saved = unit;
        for (const WaitingCall& w : waitingCalls) {
            unit = w.unit;
            judgeWait(w);
        }
        waitingCalls.clear();
        unit = saved;
    }

    void judgeWait(const WaitingCall& w) {
        const std::string callee = "'" + w.callee + "'";
        // The advice names what D81 and D89 call a task and its result (`task { }`, `then { }`),
        // as decided, ahead of the library's own renaming in M6.6.
        const std::string start = "task { " + w.callee + "() }";
        const std::string mayWait = callee + " may wait (it is a suspend function), and ";
        auto orTask = [&](const std::string& use) {
            return ". Call it from a suspend function, or start it in a task and use the result when it is ready: " + start +
                   ".then { " + use + " }";
        };
        for (const WaitFrame& f : w.frames) {
            if (f.noWait != CanWait) {
                const std::string name = f.fn != nullptr ? f.fn->name : std::string();
                report(*w.at, mayWait + noWaitReason(f.noWait, name) + orTask(f.noWait == Getter ? name + " = it" : "..."));
                return;
            }
            if (f.lambda != nullptr) {
                auto site = lambdaSites.find(f.lambda);
                if (site != lambdaSites.end() && site->second.noWait == SequenceBlock) {
                    if (w.gives) return;
                    report(*w.at, mayWait + noWaitReason(SequenceBlock, std::string()) +
                                      ". Wait before making the sequence, or make a list in a task instead: task { buildList { ... } }");
                    return;
                }
                if (f.suspendLambda) return;
                if (site != lambdaSites.end() && site->second.noWait != CanWait) {
                    report(*w.at, mayWait + noWaitReason(site->second.noWait, std::string()) + orTask("..."));
                    return;
                }
                if (lambdaInPlace(f.lambda)) continue;
                if (site != lambdaSites.end() && site->second.fn != nullptr)
                    report(*w.at, mayWait + "'" + calledName(site->second.fn) +
                                      "' does not run this lambda there and then: it keeps it or hands it on, and nothing waits "
                                      "for it. Start a task in it: " + start);
                else
                    report(*w.at, mayWait + "this lambda is kept to run later, where nothing waits for it. Give it a suspend "
                                            "type (suspend () -> unit), or start a task in it: " + start);
                return;
            }
            if (f.topLevel) return;
            if (f.fn != nullptr) {
                if (f.fn->decl != nullptr && f.fn->decl->isSuspend) return;
                const std::string& name = f.fn->name;
                report(*w.at, "'" + name + "' calls " + callee + ", which may wait, so '" + name +
                                  "' may wait too: mark it 'suspend fun " + name + "()', or run the call in a task of its own: " + start);
                return;
            }
        }
    }
};

Checker::Checker() : impl_(std::make_unique<Impl>()) {}

const std::vector<int>* Checker::argumentOrder(const CallExpr& call) const {
    auto found = impl_->argumentOrders.find(&call);
    return found == impl_->argumentOrders.end() ? nullptr : &found->second;
}

bool Checker::byteAt(const Expr& e, uint8_t& out) const {
    auto found = impl_->byteConstants.find(&e);
    if (found == impl_->byteConstants.end()) return false;
    out = found->second;
    return true;
}

bool Checker::folded(const Expr& e, ConstValue& out) {
    if (e.kind != ExprKind::Binary && e.kind != ExprKind::Unary && e.kind != ExprKind::Logical && e.kind != ExprKind::Interp)
        return false;
    out = impl_->foldQuietly(e);
    return out.kind != ConstValue::None;
}

bool Checker::constantAt(const Expr& at, ConstValue& out) {
    if (auto limit = impl_->builtinConstants.find(&at); limit != impl_->builtinConstants.end()) {
        out = limit->second;
        return true;
    }
    auto val = impl_->valUses.find(&at);
    if (val != impl_->valUses.end()) {
        out = impl_->valueOfVal(val->second);
        return out.kind != ConstValue::None;
    }
    auto use = impl_->constUses.find(&at);
    if (use == impl_->constUses.end()) return false;
    auto known = impl_->constValues.find(use->second->decl);
    if (known == impl_->constValues.end() || known->second.first != 2 || known->second.second.kind == ConstValue::None) return false;
    out = known->second.second;
    return true;
}
Checker::~Checker() = default;

void Checker::addUnit(const std::vector<StmtPtr>& statements, const std::string& file, const std::string& fileKey,
                      Namespace* ns, bool prelude, bool shell) {
    auto u = std::make_unique<Unit>();
    u->file = file;
    u->fileKey = fileKey;
    u->ns = ns;
    u->prelude = prelude;
    u->shell = shell;
    u->statements = &statements;
    auto pending = impl_->unitsByKey.find(fileKey);
    if (pending != impl_->unitsByKey.end()) u->imports = std::move(pending->second->imports);
    Unit* raw = u.get();
    impl_->units.push_back(std::move(u));
    impl_->unitsByKey[fileKey] = raw;
}

void Checker::forgetShellUnit() { impl_->forgetShellUnit(); }

void Checker::skipBodiesOf(const std::string& fileKey, std::string inPlaceFacts) {
    Unit* u = impl_->unitByKey(fileKey);
    if (u == nullptr) return;
    u->skipBodies = true;
    u->inPlaceFacts.clear();
    std::istringstream lines(inPlaceFacts);
    for (std::string line; std::getline(lines, line);) u->inPlaceFacts.push_back(line);
}

void Checker::checkBodiesOf(const std::string& fileKey) {
    Unit* u = impl_->unitByKey(fileKey);
    if (u == nullptr || !u->skipBodies) return;
    u->skipBodies = false;
    impl_->out = &diagnostics;
    impl_->inPlaceKnown.clear();
    impl_->checkUnit(u);
    impl_->judgeWaits();
}

std::string Checker::inPlaceFacts(const std::string& fileKey) {
    impl_->inPlaceKnown.clear();
    return impl_->inPlaceFactsOf(fileKey);
}

std::string Checker::summaryFacts(const std::string& fileKey) {
    Impl& c = *impl_;
    Unit* u = c.unitByKey(fileKey);
    if (u == nullptr) return {};
    c.inPlaceKnown.clear();
    std::string facts;
    Impl::eachFunction(*u->statements, [&](const FunctionDecl& d) {
        auto known = c.funcOf.find(&d);
        if (known == c.funcOf.end()) {
            facts += "\n";
            return;
        }
        FuncInfo* f = known->second;
        c.resolveFunc(f);
        if (!f->resultWritten) facts += c.argText(c.resultOf(f));
        facts += "|";
        for (size_t p = 0; p < f->params.size(); p++)
            facts += f->params[p].kind != Type::Function ? '.' : c.inPlace(f, static_cast<int>(p)) ? '1' : '0';
        facts += c.throwsFact(f);
        facts += "\n";
    });
    return facts;
}

void Checker::addImport(const std::string& fileKey, const std::string& alias, bool all, Namespace* ns) {
    auto found = impl_->unitsByKey.find(fileKey);
    Unit* u;
    if (found == impl_->unitsByKey.end()) {
        // The file's imports are compiled before the file is handed over: keep them until then.
        auto holder = std::make_unique<Unit>();
        holder->fileKey = fileKey;
        u = holder.get();
        impl_->unitsByKey[fileKey] = u;
        impl_->pendingHolders.push_back(std::move(holder));
    } else {
        u = found->second;
    }
    u->imports.push_back(ImportInfo{alias, all, ns});
}

const FunctionDecl* Checker::formRoot(const FunctionDecl& method) { return impl_->formRoot(method); }

std::vector<const FunctionDecl*> Checker::formRoots(const ClassStmt& cls, const std::string& name) {
    return impl_->formRoots(cls, name);
}

const TypeAnnotation* Checker::extensionReceiver(const FunctionDecl& fn) const {
    auto found = impl_->funcOf.find(&fn);
    // A built-in method (`external`) is the C++'s, which tells its forms apart itself.
    if (found == impl_->funcOf.end() || !found->second->isExtension || found->second->isExternal || found->second->stmt == nullptr)
        return nullptr;
    return &found->second->stmt->receiverType;
}

namespace {

// A checker's type as the compiler writes it for when the program runs; Unknown where a part
// of it cannot be (the type of a function's parameters, a class declared inside a function).
RuntimeType runtimeTypeOf(const Type& t) {
    RuntimeType r;
    r.nullable = t.nullable;
    switch (t.kind) {
        case Type::Any: r.kind = RuntimeType::Any; return r;
        case Type::Bool: r.kind = RuntimeType::Bool; return r;
        case Type::Int:
        case Type::Float:
            r.kind = t.kind == Type::Int ? RuntimeType::Int : RuntimeType::Float;
            r.storage = t.storage;
            return r;
        case Type::Char: r.kind = RuntimeType::Char; return r;
        case Type::Byte: r.kind = RuntimeType::Byte; return r;
        case Type::Index: r.kind = RuntimeType::Index; return r;
        case Type::Set:
            r.kind = RuntimeType::Set;
            r.frozen = t.frozen;
            if (t.args.size() != 1) return RuntimeType{};
            r.args.push_back(runtimeTypeOf(t.args[0]));
            if (r.args.back().kind == RuntimeType::Unknown) return RuntimeType{};
            return r;
        case Type::Number: r.kind = RuntimeType::Number; return r;
        case Type::String: r.kind = RuntimeType::String; return r;
        case Type::Bytes: r.kind = RuntimeType::Bytes; return r;
        case Type::Range: r.kind = RuntimeType::Range; return r;
        case Type::TypeValue: r.kind = RuntimeType::TypeValue; return r;
        case Type::Function: r.kind = RuntimeType::Function; return r;
        case Type::TypeParam:
            r.kind = RuntimeType::TypeParam;
            r.typeParam = t.name;
            return r;
        case Type::List:
        case Type::Map: {
            r.kind = t.kind == Type::List ? RuntimeType::List : RuntimeType::Map;
            r.frozen = t.frozen;
            for (const Type& arg : t.args) {
                r.args.push_back(runtimeTypeOf(arg));
                if (r.args.back().kind == RuntimeType::Unknown) return RuntimeType{};
            }
            if (r.args.size() != (t.kind == Type::List ? 1u : 2u)) return RuntimeType{};
            return r;
        }
        case Type::Class: {
            // A class declared inside a function, or of an anonymous object, is no global: not
            // named here. Nor is an object declared with `object`, whose class is out of sight.
            if (t.cls == nullptr || t.cls->unit == nullptr || t.cls->decl == nullptr || !t.cls->global || t.cls->isObject) return RuntimeType{};
            r.kind = RuntimeType::Class;
            r.global = t.cls->name;  // `Outer.Inner` for one declared inside another
            r.ns = t.cls->unit->ns;
            if (t.cls->decl->visibility == GlobalName::Private) r.privateFile = t.cls->unit->fileKey;
            for (const Type& arg : t.args) {
                r.args.push_back(runtimeTypeOf(arg));
                if (r.args.back().kind == RuntimeType::Unknown) return RuntimeType{};
            }
            return r;
        }
        default:
            return RuntimeType{};
    }
}

}  // namespace

bool Checker::runsOnObject(const FunctionDecl& lambda) const { return impl_->onObject.count(&lambda) != 0; }

bool Checker::hasNoIt(const FunctionDecl& lambda) const { return impl_->noItLambdas.count(&lambda) != 0; }

bool Checker::samConversion(const Expr& lambda, RuntimeType& iface, std::string& method, int& arity) const {
    auto found = impl_->samConversions.find(&lambda);
    if (found == impl_->samConversions.end()) return false;
    Type plain = found->second.iface;
    plain.args.clear();  // the class alone: what it was made for is not kept in its objects
    iface = runtimeTypeOf(plain);
    method = found->second.method;
    arity = found->second.arity;
    return iface.kind == RuntimeType::Class;
}

bool Checker::samCall(const CallExpr& call) const { return impl_->samCalls.count(&call) != 0; }

bool Checker::inlineCall(const CallExpr& call, InlineCall& out) const {
    auto found = impl_->inlineCalls.find(&call);
    if (found == impl_->inlineCalls.end()) return false;
    const FuncInfo* f = found->second.fn;
    if (f->decl == nullptr || f->unit == nullptr) return false;
    out.fn = f->decl;
    out.paramOf = found->second.paramOf;
    out.fileKey = f->unit->fileKey;
    out.ns = f->unit->ns;
    out.owner = f->owner != nullptr ? f->owner->decl : nullptr;
    out.ownerName = f->owner != nullptr ? f->owner->name : std::string();
    out.extension = f->isExtension;
    out.prelude = f->unit->prelude;
    return true;
}

bool Checker::copiedIn(const FunctionDecl& fn, int param) const {
    auto found = impl_->funcOf.find(&fn);
    return found != impl_->funcOf.end() && impl_->copiedIn(found->second, param);
}

bool Checker::rangeWord(const CallExpr& call, std::string& which) const {
    auto found = impl_->infixCallees.find(&call);
    if (found == impl_->infixCallees.end()) return false;
    const FuncInfo* f = found->second;
    if (f->unit == nullptr || !f->unit->prelude || !f->isExternal || (f->name != "downTo" && f->name != "step")) return false;
    which = f->name;
    return true;
}

bool Checker::callsOnObject(const CallExpr& call) const { return impl_->onObjectCalls.count(&call) != 0; }
bool Checker::memberLocalCall(const CallExpr& call) const { return impl_->memberLocalCalls.count(&call) != 0; }

bool Checker::reflected(const ClassStmt& cls, ReflectedClass& out) {
    auto found = impl_->classOf.find(&cls);
    if (found == impl_->classOf.end()) return false;
    ClassInfo* k = found->second;
    Impl& c = *impl_;
    auto runtime = [](const Type& t) {
        RuntimeType r = runtimeTypeOf(readType(t));
        if (r.kind == RuntimeType::Unknown && t.kind != Type::Unit && t.kind != Type::Nothing) {
            r.kind = RuntimeType::Any;  // what cannot be named when the program runs: anything
            r.nullable = true;
        }
        return r;
    };
    auto function = [&](FuncInfo* f, ReflectedMember& m) {
        c.resolveFunc(f);
        for (size_t i = 0; i < f->params.size(); i++) {
            if (f->decl != nullptr && i < f->decl->paramAnnotations.size()) {
                m.paramAnnotations.resize(i + 1, nullptr);
                m.paramAnnotations[i] = &f->decl->paramAnnotations[i];
            }
            m.paramNames.push_back(i < f->paramNames.size() ? f->paramNames[i] : "");
            m.params.push_back(runtime(static_cast<int>(i) == f->vararg ? openListOf(f->varargItem) : f->params[i]));
            m.optional.push_back(i < f->hasDefault.size() && f->hasDefault[i]);
        }
    };
    if (k->isEnum) {
        // Every entry's name and place (D48).
        ReflectedMember name, ordinal;
        name.name = "name";
        name.type.kind = RuntimeType::String;
        ordinal.name = "ordinal";
        ordinal.type.kind = RuntimeType::Int;
        out.fields.push_back(name);
        out.fields.push_back(ordinal);
    }
    for (const auto& field : cls.fields) {
        auto f = k->fields.find(field->name);
        if (f == k->fields.end()) continue;
        ReflectedMember m;
        m.name = field->name;
        m.annotations = f->second->header != nullptr ? &f->second->header->annotations : &field->annotations;
        m.type = runtime(c.fieldType(f->second));
        m.flags = (!field->isVal || field->setter != nullptr ? 1 : 0) | (field->getter != nullptr && field->init == nullptr ? 2 : 0);
        out.fields.push_back(std::move(m));
    }
    for (const auto& method : cls.methods) {
        if (method->isStatic) continue;
        auto f = c.funcOf.find(method.get());
        if (f == c.funcOf.end()) continue;
        if (method->accessor == FunctionDecl::Accessor::Get) {
            // `get name()`: a property, which may be set where a setter is written too.
            ReflectedMember m;
            m.name = method->name;
            m.annotations = &method->annotations;
            m.type = runtime(c.resultOf(f->second));
            m.flags = 2;
            for (const auto& other : cls.methods)
                if (!other->isStatic && other->accessor == FunctionDecl::Accessor::Set && other->name == method->name) m.flags |= 1;
            out.fields.push_back(std::move(m));
            continue;
        }
        if (method->accessor != FunctionDecl::Accessor::None) continue;
        ReflectedMember m;
        m.name = method->name;
        m.annotations = &method->annotations;
        m.fn = method.get();
        function(f->second, m);
        if (method->name == "init") {
            if (!k->isAbstract && !k->isInterface && !k->isObject && !k->isEnum) out.constructors.push_back(std::move(m));
            continue;
        }
        m.type = runtime(c.resultOf(f->second));
        out.methods.push_back(std::move(m));
    }
    if (k->inits.empty() && !k->isAbstract && !k->isInterface && !k->isObject && !k->isEnum) {
        // The init the class is given: its header's parameters, or none.
        ReflectedMember m;
        m.name = "init";
        function(c.headerFunc(k), m);
        for (const HeaderParam& p : cls.header) m.paramAnnotations.push_back(&p.annotations);
        out.constructors.push_back(std::move(m));
    }
    if (k->isEnumClass)
        for (ClassInfo* kind : c.kindsOf(k)) {
            RuntimeType r;
            if (kind->isObject && kind->global && kind->unit != nullptr && kind->decl != nullptr) {
                // An object's class, kept out of sight under a name of its own.
                r.kind = RuntimeType::Class;
                r.global = "(object " + kind->name + ")";
                r.ns = kind->unit->ns;
                if (kind->decl->visibility == GlobalName::Private) r.privateFile = kind->unit->fileKey;
            } else {
                r = runtimeTypeOf(c.classType(kind));
            }
            if (r.kind != RuntimeType::Unknown) out.kinds.push_back(std::move(r));
        }
    return true;
}

bool Checker::selfCall(const CallExpr& call, bool& overridden) const {
    auto found = impl_->selfCalls.find(&call);
    if (found == impl_->selfCalls.end()) return false;
    overridden = impl_->overriddenBelow(found->second);
    return true;
}

bool Checker::callsItself(const CallExpr& call) const {
    auto found = impl_->selfCalls.find(&call);
    return found != impl_->selfCalls.end() && !impl_->overriddenBelow(found->second);
}

bool Checker::typeArguments(const Node& call, std::vector<RuntimeType>& out) const {
    auto found = impl_->typeArgumentsAt.find(&call);
    if (found == impl_->typeArgumentsAt.end()) return false;
    out.clear();
    for (const Type& t : found->second) out.push_back(runtimeTypeOf(t));
    return true;
}

bool Checker::externalResult(const Node& call, RuntimeType& out, std::string& name) const {
    auto found = impl_->externalCalls.find(&call);
    if (found == impl_->externalCalls.end()) return false;
    const Type& result = found->second.result;
    if (result.kind == Type::Unit || result.kind == Type::Nothing || result.kind == Type::Null) return false;
    out = runtimeTypeOf(result);
    name = found->second.name;
    return out.kind != RuntimeType::Unknown;
}

bool Checker::addsNewValue(const Node& call) const { return impl_->newValueAdds.count(&call) != 0; }

bool Checker::isValuePlace(const Expr& e) const { return impl_->valuePlaces.count(&e) != 0; }

bool Checker::extensionGetter(const MemberExpr& e) const { return impl_->extensionGetters.count(&e) != 0; }

bool Checker::atomicField(const Expr& e, AtomicFieldOp& out) const {
    auto found = impl_->atomicOps.find(&e);
    if (found == impl_->atomicOps.end()) return false;
    out.kind = found->second.kind;
    out.object = found->second.object;
    out.field = found->second.field;
    out.operand = found->second.operand;
    out.expected = found->second.expected;
    return true;
}

int Checker::protocolLoop(const ForInStmt& loop) const {
    auto found = impl_->protocolLoops.find(&loop);
    return found == impl_->protocolLoops.end() ? 0 : found->second ? 2 : 1;
}

int Checker::namedClassOf(const ClassOfExpr& e, RuntimeType& out) const {
    auto found = impl_->namedClasses.find(&e);
    if (found == impl_->namedClasses.end()) return 0;
    const Type& t = found->second;
    if (t.kind == Type::List || t.kind == Type::Set || t.kind == Type::Map) {
        // `List#`: a list of anything.
        Type any = Type::of(Type::Any);
        any.nullable = true;
        Type whole = t;
        for (Type& arg : whole.args)
            if (arg.unknown()) arg = any;
        out = runtimeTypeOf(whole);
    } else {
        out = runtimeTypeOf(t);
    }
    out.nullable = false;
    if (out.kind != RuntimeType::Unknown) return 1;
    return t.kind == Type::Class && t.cls != nullptr && !t.cls->global ? 2 : 0;
}

bool Checker::readsInPlace(const IndexExpr& index) const { return impl_->readsInPlace(index); }
bool Checker::readsFieldInPlace(const Node& read) const { return impl_->readsFieldInPlace(read); }
bool Checker::inferredFieldType(const VarStmt& field, RuntimeType& out) const {
    if (impl_->fieldByDecl.empty())
        for (const auto& f : impl_->fieldStore)
            if (f->decl != nullptr) impl_->fieldByDecl[f->decl] = f.get();
    auto found = impl_->fieldByDecl.find(&field);
    if (found == impl_->fieldByDecl.end() || found->second->typeWritten || found->second->state != 2) return false;
    const Type& t = found->second->type;
    if (t.nullable || (t.kind != Type::Int && t.kind != Type::Float && t.kind != Type::Bool)) return false;
    out = runtimeTypeOf(t);
    return true;
}

bool Checker::returnsValue(const FunctionDecl& fn) const {
    if (impl_->valueLambdas.count(&fn) != 0) return true;
    auto found = impl_->funcOf.find(&fn);
    if (found == impl_->funcOf.end() || fn.name == "init") return false;
    const Type result = impl_->resultOf(found->second);
    return result.kind == Type::Class && !result.nullable && result.cls != nullptr && result.cls->isValue;
}
bool Checker::constructsValue(const CallExpr& call) const { return impl_->constructedValues.count(&call) != 0; }
bool Checker::buildsInPlace(const AssignExpr& assign) const { return impl_->builtInPlace.count(&assign) != 0; }
bool Checker::loopsInPlace(const ForInStmt& loop) const { return impl_->loopsInPlace(loop); }
bool Checker::loopPassesOn(const ForInStmt& loop) const {
    auto found = impl_->loopCandidates.find(&loop);
    return found != impl_->loopCandidates.end() && found->second.uses.size() == 1 && found->second.argUses.size() == 1;
}
std::vector<int> Checker::paramsOwned(const FunctionDecl& fn) const { return impl_->paramsOwned(fn); }

// A method of a value type that is not mutating, run on an item where it is: copied as it
// starts if it may change a list of its type, so that what it reads stays as it was.
bool Checker::ownsThis(const FunctionDecl& fn) const {
    auto found = impl_->funcOf.find(&fn);
    if (found == impl_->funcOf.end()) return false;
    const FuncInfo* f = found->second;
    if (f->owner == nullptr || !f->owner->isValue || f->isStatic || fn.isMutating || fn.name == "init") return false;
    return impl_->mayChangeLists(&fn, f->owner);
}

bool Checker::literalType(const Expr& literal, RuntimeType& out) const {
    auto found = impl_->literalTypes.find(&literal);
    if (found == impl_->literalTypes.end()) return false;
    out = runtimeTypeOf(found->second);
    return out.kind != RuntimeType::Unknown;
}

bool Checker::unstableKey(const ClassStmt& cls) {
    auto found = impl_->classOf.find(&cls);
    return found != impl_->classOf.end() && !impl_->keyTrouble(found->second).empty();
}

size_t Checker::inheritedDataFields(const ClassStmt& cls) {
    auto found = impl_->classOf.find(&cls);
    return found == impl_->classOf.end() ? 0 : impl_->inheritedDataFields(found->second);
}

const TypeAnnotation* Checker::aliasExpansion(const TypeAnnotation& use) const {
    auto found = impl_->aliasExpansions.find(&use);
    return found == impl_->aliasExpansions.end() ? nullptr : found->second.get();
}

const std::string* Checker::kindName(const Node& at) const {
    auto found = impl_->kindNames.find(&at);
    return found == impl_->kindNames.end() ? nullptr : &found->second;
}

const std::vector<std::string>* Checker::bundleExpansion(const Expr& bundle) const {
    auto found = impl_->bundleExpansions.find(&bundle);
    return found == impl_->bundleExpansions.end() ? nullptr : &found->second;
}
bool Checker::bundleMake(const Node& at) const { return impl_->bundleMakes.count(&at) != 0; }
bool Checker::bundleCopy(const Node& at) const { return impl_->bundleCopies.count(&at) != 0; }
bool Checker::bundleAdd(const Node& at) const { return impl_->bundleAdds.count(&at) != 0; }

const FunctionDecl* Checker::secondaryConstructor(const CallExpr& call) const {
    auto found = impl_->secondaryCalls.find(&call);
    return found == impl_->secondaryCalls.end() ? nullptr : found->second->decl;
}

const FunctionDecl* Checker::chosenForm(const CallExpr& call) const {
    auto found = impl_->chosenForms.find(&call);
    // A built-in's forms are told apart by the C++ itself, under its one name.
    if (found == impl_->chosenForms.end() || found->second->isExternal) return nullptr;
    return found->second->decl;
}

void Checker::run() {
    impl_->out = &diagnostics;
    impl_->reportPrelude = reportStandardLibrary;
    impl_->requireTypes = requireTypes;
    impl_->runAll();
}

}  // namespace astra
