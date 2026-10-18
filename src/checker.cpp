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
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "compiler.h"

namespace astra {

namespace {

struct ClassInfo;

// ---- Types ----

struct Type {
    enum Kind : uint8_t {
        Unknown,    // not known: fits everything, and everything fits it
        Any,        // any value but null (`any?` takes null too)
        Nothing,    // never a value: a jump, a function that never returns
        Unit,       // what a function that gives nothing gives
        Null,       // the type of `null` alone
        Bool, Int, Float, Number, String, Bytes, Range,
        List, Map,
        Function,
        Class,
        TypeParam,  // `T` inside the class or function that declares it
    };
    Kind kind = Unknown;
    bool nullable = false;
    ClassInfo* cls = nullptr;          // Class
    std::vector<Type> args;            // List [T], Map [K, V], Class: its type arguments, Function: the parameters
    std::shared_ptr<Type> result;      // Function: the result
    std::shared_ptr<Type> receiver;    // Function: the object it runs on (`Grid.() -> unit`)
    bool knownParams = false;          // Function: whether the parameters are known
    int required = -1;                 // Function: how many of them a call must give, those after having defaults; -1 all
    std::string name;                  // TypeParam: its name
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
};

struct ClassInfo {
    std::string name;
    const ClassStmt* decl = nullptr;
    Unit* unit = nullptr;
    ClassInfo* outer = nullptr;
    bool isInterface = false, isObject = false, isEnum = false, isData = false, isAbstract = false;
    std::vector<std::string> typeParams;

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
};

struct Unit {
    std::string file, fileKey;
    Namespace* ns = nullptr;
    bool prelude = false;
    const std::vector<StmtPtr>* statements = nullptr;  // the compiler's, which outlive the checker's use
    std::vector<ImportInfo> imports;
};

// ---- Checking a body ----

struct Local {
    Type type;
    bool isVal = false;
};

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
};

const char* article(const std::string& word) {
    if (word.empty()) return "a";
    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(word[0])));
    return (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') ? "an" : "a";
}

// The arguments of a call, as the checker reads them.
// A call with no named arguments.
const std::vector<std::string> kNoNames;

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
            case Type::Int: s = "int"; break;
            case Type::Float: s = "float"; break;
            case Type::Number: s = "Number"; break;
            case Type::String: s = "string"; break;
            case Type::Bytes: s = "Bytes"; break;
            case Type::Range: s = "Range"; break;
            case Type::List: s = "List<" + (t.args.empty() ? std::string("*") : argText(t.args[0])) + ">"; break;
            case Type::Map:
                s = "Map<" + (t.args.size() < 2 ? std::string("*") : argText(t.args[0])) + ", " +
                    (t.args.size() < 2 ? std::string("*") : argText(t.args[1])) + ">";
                break;
            case Type::Function:
                if (!t.knownParams) {
                    s = "Function";
                } else {
                    s = t.receiver ? argText(*t.receiver) + ".(" : "(";
                    for (size_t i = 0; i < t.args.size(); i++) s += (i ? ", " : "") + argText(t.args[i]);
                    s += ") -> " + (t.result ? argText(*t.result) : std::string("*"));
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
        }
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
            return false;
        };
        return walk(sub);
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
        switch (to.kind) {
            case Type::Number:
                return from.kind == Type::Int || from.kind == Type::Float || from.kind == Type::Number;
            case Type::List:
                if (from.kind != Type::List) return false;
                if (to.args.empty() || from.args.empty()) return true;
                return fits(from.args[0], to.args[0]);  // read through that name only (D32)
            case Type::Map:
                if (from.kind != Type::Map) return false;
                if (to.args.size() < 2 || from.args.size() < 2) return true;
                return fits(from.args[0], to.args[0]) && fits(from.args[1], to.args[1]);
            case Type::Function: {
                if (from.kind != Type::Function) return false;
                if (!to.knownParams || !from.knownParams) return true;
                // A function whose last parameters have defaults can be called with fewer arguments.
                const size_t least = from.required < 0 ? from.args.size() : static_cast<size_t>(from.required);
                if (to.args.size() < least || to.args.size() > from.args.size()) return false;
                for (size_t i = 0; i < to.args.size(); i++)
                    if (!fits(to.args[i], from.args[i])) return false;
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
            default:
                return from.kind == to.kind;
        }
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
        return unknownType();
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

    FieldInfo* newField(const std::string& name, Unit* u, ClassInfo* owner) {
        fieldStore.push_back(std::make_unique<FieldInfo>());
        FieldInfo* f = fieldStore.back().get();
        f->name = name;
        f->unit = u;
        f->owner = owner;
        return f;
    }

    ClassInfo* gatherClass(const ClassStmt& c, Unit* u, ClassInfo* outer) {
        classStore.push_back(std::make_unique<ClassInfo>());
        ClassInfo* k = classStore.back().get();
        k->name = outer ? outer->name + "." + c.name : c.name;
        k->decl = &c;
        k->unit = u;
        k->outer = outer;
        k->isInterface = c.isInterface;
        k->isObject = c.isObject;
        k->isEnum = c.isEnum;
        k->isData = c.isData;
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
        for (const auto& inner : c.nested) k->nested[inner->name] = gatherClass(*inner, u, k);
        return k;
    }

    void gather(Unit& u) {
        if (u.ns != nullptr && u.ns->directory == "<builtin>") builtinNs = u.ns;
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
                            symbolsFor(u, stmt->visibility)[name].var = f;
                        }
                        break;
                    }
                    FieldInfo* f = newField(v.name, &u, nullptr);
                    f->decl = &v;
                    f->isVal = v.isVal;
                    symbolsFor(u, stmt->visibility)[v.name].var = f;
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
                        extensions[fs.receiverType.typeParam ? std::string("<T>") : fs.receiver][fs.fn->name].push_back(f);
                    } else {
                        symbolsFor(u, stmt->visibility)[fs.fn->name].funcs.push_back(f);
                    }
                    break;
                }
                case StmtKind::Class: {
                    const auto& c = static_cast<const ClassStmt&>(*stmt);
                    symbolsFor(u, stmt->visibility)[c.name].cls = gatherClass(c, &u, nullptr);
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
        Type t;
        if (a.isFunction()) {
            t.kind = Type::Function;
            t.knownParams = true;
            for (const TypeAnnotation& p : a.params) t.args.push_back(resolve(p, u));
            t.result = std::make_shared<Type>(resolve(a.result[0], u));
            if (!a.receiver.empty()) t.receiver = std::make_shared<Type>(resolve(a.receiver[0], u));
        } else if (a.typeParam) {
            t = typeParamType(a.name);
        } else {
            const std::string& n = a.name;
            if (n == "int") t.kind = Type::Int;
            else if (n == "float") t.kind = Type::Float;
            else if (n == "Number") t.kind = Type::Number;
            else if (n == "string") t.kind = Type::String;
            else if (n == "bool") t.kind = Type::Bool;
            else if (n == "Bytes") t.kind = Type::Bytes;
            else if (n == "Range") t.kind = Type::Range;
            else if (n == "any") t.kind = Type::Any;
            else if (n == "unit" || n == "Unit") t.kind = Type::Unit;
            else if (n == "nothing" || n == "Nothing") t.kind = Type::Nothing;
            else if (n == "Function") t.kind = Type::Function;
            else if (n == "List") {
                t.kind = Type::List;
                t.args.push_back(a.args.empty() ? unknownType() : resolve(a.args[0], u));
            } else if (n == "Map") {
                t.kind = Type::Map;
                t.args.push_back(a.args.size() < 2 ? unknownType() : resolve(a.args[0], u));
                t.args.push_back(a.args.size() < 2 ? unknownType() : resolve(a.args[1], u));
            } else if (ClassInfo* k = findClass(u, n, classAround()); k != nullptr) {
                t.kind = Type::Class;
                t.cls = k;
                for (size_t i = 0; i < k->typeParams.size(); i++)
                    t.args.push_back(i < a.args.size() ? resolve(a.args[i], u) : unknownType());
            } else {
                return unknownType();  // a name only the VM knows
            }
        }
        t.nullable = a.nullable;
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
            f->params[static_cast<size_t>(f->vararg)] = listOf(f->varargItem);
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
            return false;
        };
        walk(obj.cls, envOf(obj));
        if (!m.methods.empty()) addInheritedForms(obj, name, m);
        return m;
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
            case Type::Bool: keys.push_back("bool"); break;
            case Type::Function: keys.push_back("Function"); break;
            default: break;
        }
        keys.push_back("any");
        keys.push_back("<T>");
        for (const std::string& key : keys) {
            const auto* found = in(key);
            if (found == nullptr) continue;
            // A built-in type's own methods (in C++) come before extensions for every type when
            // the program runs, and the checker does not know them yet (part 3): the standard
            // library's `any?.equals` is not what `"a".equals("A", ignoreCase = true)` calls.
            if (t.kind != Type::Class && (key == "any" || key == "<T>")) {
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

    // Members every object has, which the VM gives without a declaration.
    static bool builtinMember(const std::string& name) {
        return name == "toString" || name == "hashCode" || name == "equals" || name == "class" || name == "let" ||
               name == "also" || name == "apply" || name == "run" || name == "takeIf" || name == "takeUnless";
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
                if (i.negated == whenTrue) return;
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
                Type item = unknownType();
                if (!over.nullable) {
                    if (over.kind == Type::Range || over.kind == Type::Bytes) item = Type::of(Type::Int);
                    else if (over.kind == Type::String) item = Type::of(Type::String);
                    else if (over.kind == Type::List && !over.args.empty()) item = over.args[0];
                } else if (over.known() && over.kind != Type::Null) {
                    report(*f.iterable, "a for cannot go over " + describe(over) + ", which may be null");
                }
                push();
                if (f.pattern.empty()) {
                    declareLocal(f.variable, item, true);
                } else {
                    std::vector<Type> parts = partsOf(item, f.pattern.size());
                    for (size_t i = 0; i < f.pattern.size(); i++) declareLocal(f.pattern[i], parts[i], true);
                }
                for (const StmtPtr& st : f.body->statements) stmt(*st);
                pop();
                break;
            }
            case StmtKind::Return:
                returnStmt(static_cast<const ReturnStmt&>(s));
                break;
            case StmtKind::Break:
            case StmtKind::Continue:
                break;
            case StmtKind::Throw:
                expr(*static_cast<const ThrowStmt&>(s).value);
                break;
            case StmtKind::Try: {
                const auto& t = static_cast<const TryStmt&>(s);
                blockMaybeCut(t.body->statements);
                for (const CatchClause& c : t.catches) {
                    push();
                    if (c.hasVariable) {
                        Type caught = c.type.present() ? resolve(c.type, *unit) : unknownType();
                        declareLocal(c.variable, caught, true);
                    }
                    for (const StmtPtr& st : c.body->statements) stmt(*st);
                    pop();
                }
                if (t.finalizer) block(t.finalizer->statements);
                break;
            }
            case StmtKind::Import:
                break;
            case StmtKind::Function: {
                const auto& fs = static_cast<const FunctionStmt&>(s);
                if (ctx != nullptr && ctx->topLevel && scopes.size() <= 1) break;  // checked with the file's functions
                // A function declared inside another: a local whose type is a function.
                FuncInfo* f = newFunc(*fs.fn, unit, ctx ? ctx->cls : nullptr);
                resolveFunc(f);
                declareLocal(fs.fn->name, functionValue(f), true);
                checkFunctionBody(f, nullptr);
                break;
            }
            case StmtKind::Class: {
                if (ctx != nullptr && ctx->topLevel && scopes.size() <= 1) break;  // a file's class: already gathered
                const auto& cs = static_cast<const ClassStmt&>(s);
                ClassInfo* k = gatherClass(cs, unit, nullptr);
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
        Type declared = v.type.present() ? resolve(v.type, *unit) : unknownType();
        Type value = v.init ? exprExpected(*v.init, declared) : unknownType();
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
            if (v.init && value.kind == Type::Null)
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
    }

    void returnStmt(const ReturnStmt& r) {
        if (ctx == nullptr) return;
        const bool checked = ctx->returnKnown && r.label.empty() && !(ctx->isLambda && !r.implicit);
        Type value = r.value ? (checked ? exprExpected(*r.value, ctx->expected) : expr(*r.value)) : Type::of(Type::Unit);
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

    // A lambda or `fun (x) { ... }` used as a value, with the function type expected where it
    // stands, if one is: its parameters and the object it runs on are taken from it.
    Type lambda(const FunctionDecl& d, const Type* expected) {
        const bool typed = expected != nullptr && expected->kind == Type::Function && expected->knownParams;
        FnCtx c;
        c.parent = ctx;
        c.cls = ctx ? ctx->cls : nullptr;
        c.isStatic = ctx ? ctx->isStatic : false;
        c.isLambda = d.isLambda;
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
                c.receiverUnknown = true;
            }
        }
        FnCtx* saved = ctx;
        ctx = &c;
        push();
        Type ft = Type::of(Type::Function);
        const bool itLambda = d.isLambda && d.params.size() == 1 && d.params[0] == "it" && !d.paramTypes[0].present() &&
                              d.paramDefaults.size() == 1 && d.paramDefaults[0] != nullptr;
        ft.knownParams = !itLambda || typed;
        for (size_t i = 0; i < d.params.size(); i++) {
            Type p = i < d.paramTypes.size() ? resolve(d.paramTypes[i], *unit) : unknownType();
            if (p.unknown() && typed && i < expected->args.size()) p = expected->args[i];
            ft.args.push_back(p);
            declareLocal(d.params[i], p, true);
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
        for (const StmtPtr& s : d.body) stmt(*s);
        pop();
        ctx = saved;
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

    void checkClass(ClassInfo* k) {
        const ClassStmt& c = *k->decl;
        // A method written override takes the place of a form above with its parameters.
        for (const auto& m : c.methods) {
            if (!m->isOverride || m->isStatic || m->accessor != FunctionDecl::Accessor::None || m->name == "init") continue;
            auto known = funcOf.find(m.get());
            if (known == funcOf.end()) continue;
            FuncInfo* f = known->second;
            const std::vector<FuncInfo*> above = formsAbove(k, m->name);
            if (above.empty()) continue;
            bool same = false;
            for (FuncInfo* g : above) same = same || sameForm(f, g);
            if (same) continue;
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
            Type declared = fieldType(f);
            if (requireTypes && !field->type.present() && !field->init && field->getter == nullptr && f->header == nullptr)
                report(*field, "'" + field->name + "' has no value to take its type from; write one, as in " +
                                   (field->isVal ? "val " : "var ") + field->name + " : Type");
            if (field->init) {
                FnCtx fc;
                fc.cls = k;
                withCtx(fc, [&] {
                    Type value = exprExpected(*field->init, declared);
                    if (field->type.present() && !fits(value, declared))
                        report(*field->init, "'" + field->name + "' is " + describe(declared) + "; it cannot start as " +
                                                 describe(value));
                    if (!field->type.present() && value.kind == Type::Null && (f->header == nullptr || !f->header->type.present()))
                        report(*field, "'" + field->name + "' starts as null, so its type must be written (" + field->name +
                                           " : Type? = null)");
                });
            }
            for (const auto* acc : {field->getter.get(), field->setter.get()}) {
                if (acc == nullptr) continue;
                FuncInfo* fn = newFunc(*acc, k->unit, k);
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
            Type declared = fieldType(f);
            if (field->init) {
                FnCtx fc;
                fc.cls = k;
                fc.isStatic = true;
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
            withCtx(fc, [&] { blockMaybeCut(body->statements); });
        }
        if (c.hasSuperArgs && k->superclass != nullptr) {
            FnCtx fc;
            fc.cls = k;
            withCtx(fc, [&] {
                // The header's parameters are in reach of the superclass's arguments.
                for (const HeaderParam& h : c.header) declareLocal(h.name, resolve(h.type, *k->unit), true);
                Args args{c.superArgs, kNoNames, false, false};
                Type made = constructorCall(k->superclass, args, {}, c);
                (void)made;
            });
        }
        for (const EnumEntry& e : c.entries) {
            if (e.args.empty() && !k->headerInit && k->inits.empty()) continue;
            FnCtx fc;
            fc.cls = k;
            fc.isStatic = true;
            withCtx(fc, [&] {
                Args args{e.args, kNoNames, false, false};
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
    Type exprExpected(const Expr& e, const Type& expected) {
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
                if (all) return withNull(expected, false);
                return listOf(item.kind == Type::Null ? unknownType() : item);
            }
            if (e.kind == ExprKind::Map && expected.kind == Type::Map && expected.args.size() == 2 && !mentionsTypeParam(expected)) {
                const auto& m = static_cast<const MapExpr&>(e);
                bool all = true;
                Type keys, values;
                for (size_t i = 0; i < m.entries.size(); i++) {
                    Type k = exprExpected(*m.entries[i].first, expected.args[0]);
                    Type v = exprExpected(*m.entries[i].second, expected.args[1]);
                    if (!fits(k, expected.args[0]) || !fits(v, expected.args[1])) all = false;
                    keys = i == 0 ? k : common(keys, k);
                    values = i == 0 ? v : common(values, v);
                }
                if (all) return withNull(expected, false);
                Type t = Type::of(Type::Map);
                t.args = {keys, values};
                return t;
            }
        }
        return expr(e);
    }

    Type expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Literal: {
                const auto& l = static_cast<const LiteralExpr&>(e);
                switch (l.type) {
                    case LiteralExpr::Type::Null: return Type::of(Type::Null);
                    case LiteralExpr::Type::Bool: return Type::of(Type::Bool);
                    case LiteralExpr::Type::Int: return Type::of(Type::Int);
                    case LiteralExpr::Type::Float: return Type::of(Type::Float);
                    case LiteralExpr::Type::String: return Type::of(Type::String);
                }
                return unknownType();
            }
            case ExprKind::Interp:
                for (const ExprPtr& p : static_cast<const InterpExpr&>(e).parts) expr(*p);
                return Type::of(Type::String);
            case ExprKind::Name:
                return name(static_cast<const NameExpr&>(e));
            case ExprKind::This:
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
            case ExprKind::Call:
                return call(static_cast<const CallExpr&>(e));
            case ExprKind::Member:
                return member(static_cast<const MemberExpr&>(e));
            case ExprKind::Index:
                return index(static_cast<const IndexExpr&>(e));
            case ExprKind::List: {
                const auto& l = static_cast<const ListExpr&>(e);
                if (l.items.empty()) return listOf(unknownType());
                Type item = expr(*l.items[0]);
                for (size_t i = 1; i < l.items.size(); i++) item = common(item, expr(*l.items[i]));
                if (item.kind == Type::Null) item = unknownType();
                return listOf(item);
            }
            case ExprKind::Map: {
                const auto& m = static_cast<const MapExpr&>(e);
                Type t = Type::of(Type::Map);
                Type k = unknownType(), v = unknownType();
                for (size_t i = 0; i < m.entries.size(); i++) {
                    Type ki = expr(*m.entries[i].first), vi = expr(*m.entries[i].second);
                    k = i == 0 ? ki : common(k, ki);
                    v = i == 0 ? vi : common(v, vi);
                }
                if (k.kind == Type::Null) k = unknownType();
                if (v.kind == Type::Null) v = unknownType();
                t.args = {k, v};
                return t;
            }
            case ExprKind::Range: {
                const auto& r = static_cast<const RangeExpr&>(e);
                for (const Expr* part : {r.start.get(), r.end.get(), r.step.get()}) {
                    if (part == nullptr) continue;
                    Type t = expr(*part);
                    if (t.known() && !(t.kind == Type::Int && !t.nullable) && t.kind != Type::Nothing && t.kind != Type::TypeParam)
                        report(*part, "a range is made of ints; this is " + describe(t));
                }
                return Type::of(Type::Range);
            }
            case ExprKind::Is:
                expr(*static_cast<const IsExpr&>(e).value);
                return Type::of(Type::Bool);
            case ExprKind::Cast: {
                const auto& c = static_cast<const CastExpr&>(e);
                expr(*c.value);
                Type t = resolve(c.type, *unit);
                return c.safe ? withNull(t) : t;
            }
            case ExprKind::In: {
                const auto& i = static_cast<const InExpr&>(e);
                expr(*i.value);
                expr(*i.container);
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
            Type value = statementValueOf(*t.body);
            for (const CatchClause& c : t.catches) {
                push();
                if (c.hasVariable) declareLocal(c.variable, c.type.present() ? resolve(c.type, *unit) : unknownType(), true);
                value = common(value, statementValueOf(*c.body));
                pop();
            }
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
        for (const WhenBranch& b : w.branches) {
            if (b.isElse) hasElse = true;
            std::unordered_map<std::string, Type> narrow;
            for (const WhenCondition& c : b.conditions) {
                if (c.kind == WhenCondition::Kind::Is) {
                    if (w.subject && w.subject->kind == ExprKind::Name && !c.negated && b.conditions.size() == 1) {
                        Type t = resolve(c.type, *unit);
                        if (t.known()) narrow[static_cast<const NameExpr&>(*w.subject).name] = t;
                    }
                } else if (c.value) {
                    if (w.subject && c.kind == WhenCondition::Kind::Value) noteCovered(*c.value, subject, covered);
                    Type t = w.subject && c.kind == WhenCondition::Kind::Value && subject.kind == Type::Class && subject.cls &&
                                     subject.cls->isEnum && c.value->kind == ExprKind::Name
                                 ? enumEntry(subject, static_cast<const NameExpr&>(*c.value).name, *c.value)
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
            } else if (subject.kind == Type::Bool) {
                for (const char* v : {"true", "false"})
                    if (!covered.count(v)) missing.push_back(v);
            }
            if (!missing.empty() || (subject.kind == Type::Class && subject.cls && subject.cls->isEnum) || subject.kind == Type::Bool) {
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
        } else if (subject.kind == Type::Class && subject.cls != nullptr && subject.cls->isEnum) {
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
    Type name(const NameExpr& n) {
        if (Local* l = findLocal(n.name)) return localType(n.name, *l);
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
                if (m.field) return subst(fieldType(m.field), m.env);
                if (m.getter) return subst(resultOf(m.getter), m.env);
            }
            for (ClassInfo* c = ctx->cls; c != nullptr; c = c->outer) {
                auto s = c->statics.find(n.name);
                if (s != c->statics.end()) return fieldType(s->second);
                auto nested = c->nested.find(n.name);
                if (nested != c->nested.end()) return nested->second->isObject ? classType(nested->second) : unknownType();
            }
        }
        if (const GlobalSym* g = findGlobal(*unit, n.name)) {
            if (g->var != nullptr) return fieldType(g->var);
            if (g->cls != nullptr) return g->cls->isObject ? classType(g->cls) : unknownType();
            if (g->funcs.size() == 1) return functionValue(g->funcs[0]);
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
                if (!isNumeric(t)) report(u, "'-' needs a number; this is " + describe(t));
                return isNumeric(t) ? t : unknownType();
            case TokenType::Tilde:
                if (t.kind == Type::Class) return operatorResult(t, "unary~", u, {});
                if (!(t.kind == Type::Int && !t.nullable)) report(u, "'~' needs an int; this is " + describe(t));
                return Type::of(Type::Int);
            case TokenType::BangBang:
                return withNull(t, false);
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
            default: return "?";
        }
    }

    Type binary(const BinaryExpr& b) {
        Type l = expr(*b.left);
        Type r = expr(*b.right);
        const TokenType op = b.op;
        switch (op) {
            case TokenType::EqEq:
            case TokenType::BangEq:
            case TokenType::EqEqEq:
            case TokenType::BangEqEq:
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
        switch (op) {
            case TokenType::Plus:
            case TokenType::Minus:
            case TokenType::Star:
            case TokenType::Slash:
            case TokenType::Percent:
                if (isNumeric(l) && isNumeric(r)) return arithmetic(l, r);
                if (op == TokenType::Plus && l.kind == Type::List && r.kind == Type::List) return common(l, r);
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
                if (l.kind != Type::Int || r.kind != Type::Int) {
                    report(b, "'" + opText(op) + "' works on ints; these are " + describe(l) + " and " + describe(r));
                    return unknownType();
                }
                return Type::of(Type::Int);
            default:
                return unknownType();
        }
    }

    static Type arithmetic(const Type& l, const Type& r) {
        if (l.kind == Type::Int && r.kind == Type::Int) return Type::of(Type::Int);
        if (l.kind == Type::Float || r.kind == Type::Float) return Type::of(Type::Float);
        if (l.kind == Type::Number || r.kind == Type::Number) return Type::of(Type::Number);
        return Type::of(Type::Float);
    }

    // ---- Assignment ----

    Type assign(const AssignExpr& a) {
        Type target = unknownType();
        std::string targetName;
        Local* local = nullptr;
        if (a.target->kind == ExprKind::Name) {
            const auto& n = static_cast<const NameExpr&>(*a.target);
            targetName = n.name;
            local = findLocal(n.name);
            target = local != nullptr ? local->type : name(n);
            // The name of a class, an object or a function is never given a value; the compiler
            // says so, which tells more than what the value's type is.
            if (local == nullptr) {
                const GlobalSym* g = localClass(n.name) == nullptr ? findGlobal(*unit, n.name) : nullptr;
                if (localClass(n.name) != nullptr || (g != nullptr && g->var == nullptr)) target = unknownType();
            }
        } else if (a.target->kind == ExprKind::Member) {
            const auto& m = static_cast<const MemberExpr&>(*a.target);
            targetName = m.name;
            target = assignedMember(m);
        } else if (a.target->kind == ExprKind::Index) {
            target = index(static_cast<const IndexExpr&>(*a.target));
        } else {
            expr(*a.target);
        }
        Type value = exprExpected(*a.value, a.op == TokenType::Eq ? target : unknownType());
        if (local != nullptr) {
            forget(targetName);
            // `next = Dog("Fido")`: from here on it is that value, not null.
            if (a.op == TokenType::Eq && target.nullable && value.known() && !value.nullable && value.kind != Type::Null &&
                value.kind != Type::Nothing && fits(value, target) && !narrowed.empty())
                narrowed.back()[targetName] = withNull(target, false);
        }
        if (target.unknown()) return Type::of(Type::Unit);
        Type result = value;
        if (a.op != TokenType::Eq) {
            TokenType op;
            switch (a.op) {
                case TokenType::PlusEq: op = TokenType::Plus; break;
                case TokenType::MinusEq: op = TokenType::Minus; break;
                case TokenType::StarEq: op = TokenType::Star; break;
                case TokenType::SlashEq: op = TokenType::Slash; break;
                case TokenType::PercentEq: op = TokenType::Percent; break;
                default: return Type::of(Type::Unit);
            }
            if (target.unknown() || value.unknown()) return Type::of(Type::Unit);
            // a?.count += 1 does nothing when a is null, as with ++
            if (target.nullable && safeChain(*a.target)) target = withNull(target, false);
            if (!nullablePlace(*a.target, target, opText(op) + "=", a.nullSafe, a)) target = withNull(target, false);
            else return Type::of(Type::Unit);
            if (op == TokenType::Plus && (target.kind == Type::String || value.kind == Type::String)) result = Type::of(Type::String);
            else if (isNumeric(target) && isNumeric(value)) result = arithmetic(target, value);
            else return Type::of(Type::Unit);
        }
        if (!fits(result, target)) {
            std::string who = targetName.empty() ? "this place" : "'" + targetName + "'";
            if (a.op == TokenType::Eq)
                report(*a.value, who + " is " + describe(target) + "; it cannot be given " + describe(result));
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
                report(m, plain.cls->name + " has no field '" + m.name + "'; fields must be declared in the class with val or var");
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
            if (findLocal(base) == nullptr && !nameIsMember(base)) {
                if (Namespace* ns = findAlias(*unit, base)) {
                    if (const GlobalSym* g = findIn(ns, m.name)) {
                        if (g->var) return fieldType(g->var);
                        if (g->cls) return g->cls->isObject ? classType(g->cls) : unknownType();
                        if (g->funcs.size() == 1) return functionValue(g->funcs[0]);
                    }
                    return unknownType();
                }
                if (ClassInfo* k = classNamed(base)) return staticMember(k, m);
            }
        }
        Type obj = expr(*m.object);
        return memberOf(obj, m.name, m, m.safe);
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
        if (s != k->statics.end()) return fieldType(s->second);
        auto n = k->nested.find(m.name);
        if (n != k->nested.end()) return n->second->isObject ? classType(n->second) : unknownType();
        auto sm = k->staticMethods.find(m.name);
        if (sm != k->staticMethods.end() && sm->second.size() == 1) return functionValue(sm->second[0]);
        return unknownType();
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

    Type memberOf(const Type& objIn, const std::string& name, const MemberExpr& at, bool safe) {
        Type obj = objIn;
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
            if (m.field) {
                result = subst(fieldType(m.field), m.env);
            } else if (m.getter) {
                result = subst(resultOf(m.getter), m.env);
            } else if (m.setter) {
                resolveFunc(m.setter);
                result = m.setter->params.empty() ? unknownType() : subst(m.setter->params[0], m.env);
            } else if (m.methods.size() == 1) {
                result = subst(functionValue(m.methods[0]), m.env);
            } else if (m.methods.empty()) {
                auto st = k->statics.find(name);
                if (st != k->statics.end()) result = fieldType(st->second);
                else if (k->isEnum && name == "ordinal") result = Type::of(Type::Int);
                else if (k->isEnum && name == "name") result = Type::of(Type::String);
                else if (findExtensions(plain, name).empty() && reportMissing(k, name))
                    report(at, k->name + " has no member '" + name + "'");
            }
        } else if (!builtinProperty(obj, name, result)) {
            if ((obj.kind == Type::List || obj.kind == Type::String || obj.kind == Type::Map || obj.kind == Type::Bytes) &&
                (name == "size" || name == "length" || name == "lastIndex"))
                result = Type::of(Type::Int);
        }
        return safe ? withNull(result) : result;
    }

    Type index(const IndexExpr& i) {
        Type obj = expr(*i.object);
        Type idx = expr(*i.index);
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
            if (idx.known() && !(idx.kind == Type::Int && !idx.nullable) && idx.kind != Type::Nothing &&
                idx.kind != Type::Range && idx.kind != Type::TypeParam)
                report(*i.index, "an index must be an int, not " + describe(idx));
        };
        switch (obj.kind) {
            case Type::List:
                needInt();
                if (idx.kind == Type::Range) return obj;
                return obj.args.empty() ? unknownType() : obj.args[0];
            case Type::String:
                needInt();
                return Type::of(Type::String);
            case Type::Bytes:
                needInt();
                return Type::of(Type::Int);
            case Type::Map:
                if (obj.args.size() == 2 && !fits(idx, obj.args[0]))
                    report(*i.index, "the keys of this map are " + text(obj.args[0]) + "s; this is " + describe(idx));
                return obj.args.size() == 2 ? withNull(obj.args[1]) : unknownType();
            case Type::Class:
                return operatorResult(obj, "[]", i, {idx});
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
            fixByReceiver(env, vars);
        }
        if (args.spread) {
            for (const ExprPtr& a : args.exprs) expr(*a);
            if (envOut != nullptr) *envOut = env;
            return finish(resultOf(f), env, vars);
        }
        Binding b = bind(f, args, at, what);
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
                types[i] = exprExpected(*args.exprs[i], expected);
                unify(want, types[i], env, vars);
            }
        }
        for (size_t i = 0; i < args.exprs.size(); i++) {
            if (b.paramOf[i] < 0) continue;
            Type want = subst(wanted(i), env);
            if (!fits(types[i], want)) {
                const std::string& param = f->paramNames[static_cast<size_t>(b.paramOf[i])];
                report(*args.exprs[i], what + " takes " + describe(want) + " for '" + param + "', not " + describe(types[i]));
            }
        }
        if (envOut != nullptr) *envOut = env;
        return finish(resultOf(f), env, vars);
    }

    // What the object a function is called on says of its type parameters is so: `words.add(1)` on
    // a List<string> does not make T something both fit, as an argument would.
    static void fixByReceiver(const Env& env, std::vector<std::string>& vars) {
        vars.erase(std::remove_if(vars.begin(), vars.end(),
                                  [&](const std::string& v) {
                                      auto found = env.find(v);
                                      return found != env.end() && found->second.known();
                                  }),
                   vars.end());
    }

    // A result with what is known of the type parameters filled in, and the rest unknown.
    Type finish(const Type& result, Env env, const std::vector<std::string>& vars) {
        for (const std::string& v : vars)
            if (!env.count(v)) env[v] = unknownType();
        return subst(result, env);
    }

    Type constructorCall(ClassInfo* k, const Args& args, const std::vector<TypeAnnotation>& typeArgs, const Node& at) {
        const std::string what = k->name + "(...)";
        Env env;
        for (size_t i = 0; i < typeArgs.size() && i < k->typeParams.size(); i++) env[k->typeParams[i]] = resolve(typeArgs[i], *unit);
        FuncInfo* init = k->inits.size() == 1 ? k->inits[0] : (k->headerInit ? headerFunc(k) : nullptr);
        if (init != nullptr) {
            callWith(init, args, {}, env, nullptr, at, what, &env);
        } else {
            for (const ExprPtr& a : args.exprs) expr(*a);
            if (k->inits.empty() && !k->decl->hasHeader && k->decl->supertypes.empty() && !args.exprs.empty())
                report(at, what + " takes no arguments");
        }
        Type t = classType(k);
        for (const std::string& p : k->typeParams) t.args.push_back(env.count(p) ? env[p] : unknownType());
        return t;
    }

    // A call of a name that may have several forms (overloads, D56): the form it means is
    // chosen, and the call checked against it. `envs`, when given, holds each form's own `env`.
    Type callFunctions(const std::vector<FuncInfo*>& fs, const CallExpr& c, const std::string& what, const Env& env,
                       const Type* receiver, const std::vector<Env>* envs = nullptr) {
        Args args{c.args, c.names, c.trailingLambda, c.hasSpread};
        auto envOfForm = [&](size_t i) -> const Env& { return envs != nullptr && i < envs->size() ? (*envs)[i] : env; };
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
        if (forms.size() == 1) return callWith(forms[0], args, c.typeArgs, formEnvs[0], receiver, c, what + "(...)");
        size_t index = 0;
        auto envOfChosen = [&](size_t i) -> const Env& { return formEnvs[i]; };
        FuncInfo* f = args.spread ? nullptr : chooseForm(forms, envOfChosen, args, c.typeArgs, receiver, c, what, index);
        if (f == nullptr) {
            for (const ExprPtr& a : c.args) expr(*a);
            return unknownType();
        }
        chosenForms[&c] = f;
        return callWith(f, args, c.typeArgs, formEnvs[index], receiver, c, what + "(...)");
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
                fixByReceiver(fit.env, vars);
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
                    ok = lambdaFits(static_cast<const FunctionExpr&>(*args.exprs[i]), subst(want, fit.env));
                } else {
                    unify(want, types[i], fit.env, vars);
                    ok = fits(types[i], subst(want, fit.env));
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
            if (!types[i].unknown()) continue;
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
            if (!ab) return false;
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
        report(at, "this call fits more than one form of " + what + "(...): " + formsText(which) +
                       "; give an argument a type that tells them apart, with a typed variable or 'as'");
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

    // A method of a value: declared on its class or above, or an extension.
    Type callMethod(const Type& objIn, const CallExpr& c, const MemberExpr& m) {
        Type obj = objIn;
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
            if (!mem.methods.empty()) return callFunctions(mem.methods, c, k->name + "." + m.name, mem.env, nullptr, &mem.methodEnvs);
            if (mem.field || mem.getter) return callValue(memberOf(plain, m.name, m, false), c, m.name);
            std::vector<FuncInfo*> ext = findExtensions(plain, m.name);
            if (!ext.empty()) return callFunctions(ext, c, m.name, {}, &plain);
            auto st = k->staticMethods.find(m.name);
            if (st != k->staticMethods.end()) return callFunctions(st->second, c, k->name + "." + m.name, {}, nullptr);
            for (const ExprPtr& a : c.args) expr(*a);
            if (reportMissing(k, m.name)) report(m, k->name + " has no method '" + m.name + "'");
            return unknownType();
        }
        std::vector<FuncInfo*> ext = findExtensions(plain, m.name);
        if (!ext.empty()) return callFunctions(ext, c, m.name, {}, &plain);
        for (const ExprPtr& a : c.args) expr(*a);
        if (builtinDeclared(plain) && !anyExtension(m.name) && !builtinMember(m.name)) {
            Type property;
            if (!builtinProperty(plain, m.name, property)) report(m, describe(plain) + " has no method '" + m.name + "'");
        }
        return unknownType();
    }

    // let, also and takeIf with a lambda of one parameter written after them are compiled in
    // place, as Kotlin's scope functions, whatever the value is, null too, and whatever its
    // class has (compileScopeCall in compiler.cpp).
    static bool isScopeCall(const CallExpr& c, const MemberExpr& m) {
        if (c.args.size() != 1 || c.namedCount != 0 || c.hasSpread || c.args[0]->kind != ExprKind::Function) return false;
        const FunctionDecl& fn = *static_cast<const FunctionExpr&>(*c.args[0]).fn;
        return fn.isLambda && fn.params.size() == 1 && (m.name == "let" || m.name == "also" || m.name == "takeIf");
    }

    Type scopeCall(const Type& on, const CallExpr& c, const MemberExpr& m) {
        const Type obj = m.safe ? withNull(on, false) : on;
        Type want = Type::of(Type::Function);
        want.knownParams = true;
        want.args.push_back(obj);
        want.result = std::make_shared<Type>(m.name == "takeIf" ? Type::of(Type::Bool) : unknownType());
        Type lambda = exprExpected(*c.args[0], want);
        Type result;
        if (m.name == "let") result = lambda.kind == Type::Function && lambda.result ? *lambda.result : unknownType();
        else if (m.name == "also") result = obj;
        else result = withNull(obj);
        return m.safe && result.known() ? withNull(result) : result;
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

    Type call(const CallExpr& c) {
        if (c.callee->kind == ExprKind::Name) {
            const std::string& n = static_cast<const NameExpr&>(*c.callee).name;
            if (Local* l = findLocal(n)) return callValue(localType(n, *l), c, n);
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
                for (const ExprPtr& a : c.args) expr(*a);
                return m.safe ? Type::of(Type::Null) : unknownType();
            }
            if (isScopeCall(c, m)) return scopeCall(obj, c, m);
            if (obj.nullable && !m.safe) {
                // `fun string?.isNullOrEmpty()`: an extension that takes null.
                Type plain = withNull(obj, false);
                std::vector<FuncInfo*> takesNull;
                for (FuncInfo* f : findExtensions(plain, m.name)) {
                    resolveFunc(f);
                    if (f->isExtension && f->receiver.nullable) takesNull.push_back(f);
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
    Type callValue(const Type& t, const CallExpr& c, const std::string& name) {
        if (t.unknown() || t.kind == Type::Class || t.kind == Type::TypeParam) {
            for (const ExprPtr& a : c.args) expr(*a);
            return unknownType();
        }
        const std::string who = name.empty() ? std::string("this value") : "'" + name + "'";
        if (t.nullable) report(c, who + " may be null, so it cannot be called; check it first");
        else if (t.kind != Type::Function) report(c, who + " is " + describe(t) + ", which cannot be called");
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
                Type a = exprExpected(*c.args[i], want);
                if (p < t.args.size() && !fits(a, want))
                    report(*c.args[i], who + " takes " + describe(want) + " here, not " + describe(a));
            }
        } else {
            for (const ExprPtr& a : c.args) expr(*a);
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
        for (size_t i = first; i < units.size(); i++) {
            Unit* u = units[i].get();
            unit = u;
            // The top level, in order.
            FnCtx top;
            top.topLevel = true;
            ctx = &top;
            scopes.clear();
            narrowed.clear();
            tparams.clear();
            push();
            for (const StmtPtr& s : *u->statements) stmt(*s);
            pop();
            // Functions, classes and extensions.
            for (const StmtPtr& s : *u->statements) {
                if (s->kind == StmtKind::Function) {
                    auto known = funcOf.find(static_cast<const FunctionStmt&>(*s).fn.get());
                    if (known != funcOf.end()) checkFunctionBody(known->second, nullptr);
                } else if (s->kind == StmtKind::Class) {
                    checkClass(classOf[static_cast<const ClassStmt*>(s.get())]);
                }
            }
        }
        ctx = nullptr;
    }
};

Checker::Checker() : impl_(std::make_unique<Impl>()) {}
Checker::~Checker() = default;

void Checker::addUnit(const std::vector<StmtPtr>& statements, const std::string& file, const std::string& fileKey,
                      Namespace* ns, bool prelude) {
    auto u = std::make_unique<Unit>();
    u->file = file;
    u->fileKey = fileKey;
    u->ns = ns;
    u->prelude = prelude;
    u->statements = &statements;
    auto pending = impl_->unitsByKey.find(fileKey);
    if (pending != impl_->unitsByKey.end()) u->imports = std::move(pending->second->imports);
    Unit* raw = u.get();
    impl_->units.push_back(std::move(u));
    impl_->unitsByKey[fileKey] = raw;
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

const FunctionDecl* Checker::chosenForm(const CallExpr& call) const {
    auto found = impl_->chosenForms.find(&call);
    return found == impl_->chosenForms.end() ? nullptr : found->second->decl;
}

void Checker::run() {
    impl_->out = &diagnostics;
    impl_->reportPrelude = reportStandardLibrary;
    impl_->requireTypes = requireTypes;
    impl_->runAll();
}

}  // namespace astra
