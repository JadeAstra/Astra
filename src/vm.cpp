#include "vm.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace astra {

namespace {

// Counts a call from a built-in function back into the interpreter, and puts back the
// record of where that built-in's own arguments are when the call returns.
struct NativeCallGuard {
    int& depth;
    Value*& base;
    Value* const saved;
    NativeCallGuard(int& d, Value*& b) : depth(d), base(b), saved(b) { depth++; }
    ~NativeCallGuard() {
        depth--;
        base = saved;
    }
};

}  // namespace

VM::VM(bool worker) : output(&std::cout), input(&std::cin), rng(std::random_device{}()), isWorker_(worker) {
    // The thread that makes the VM is its main thread, and holds the lock from the start.
    mainThread_.name = "main";
    mainThread_.id = nextThreadId_++;
    threads_.push_back(&mainThread_);
    loadContext(&mainThread_.base);

    if (const char* stress = std::getenv("ASTRA_THREAD_STRESS"); stress != nullptr && *stress != '\0' && *stress != '0')
        threadStress_ = true;

    if (const char* stress = std::getenv("ASTRA_GC_STRESS"); stress != nullptr && *stress != '\0' && *stress != '0') {
        gcStress_ = true;
        nextGC_ = 0;
    }

    names.init = intern("init");
    names.toString = intern("toString");
    names.message = intern("message");
    names.length = intern("length");
    names.size = intern("size");
    names.indices = intern("indices");
    names.lastIndex = intern("lastIndex");
    names.keys = intern("keys");
    names.values = intern("values");
    names.key = intern("key");
    names.value = intern("value");
    names.code = intern("code");
    names.iterator = intern("iterator");
    names.first = intern("first");
    names.last = intern("last");
    names.absoluteValue = intern("absoluteValue");
    names.sign = intern("sign");
    names.start = intern("start");
    names.end = intern("end");
    names.step = intern("step");
    names.name = intern("name");
    names.className = intern("className");
    names.ordinal = intern("ordinal");
    names.entries = intern("entries");
    names.enumEntries = intern("(entries)");
    names.entryName = intern("(name)");
    names.entryOrdinal = intern("(ordinal)");
    names.opAdd = intern("+");
    names.opSub = intern("-");
    names.opMul = intern("*");
    names.opDiv = intern("/");
    names.opMod = intern("%");
    names.opEq = intern("==");
    names.opLt = intern("<");
    names.opLe = intern("<=");
    names.opGt = intern(">");
    names.opGe = intern(">=");
    names.opNeg = intern("unary-");
    names.opIndex = intern("[]");
    names.opIndexSet = intern("[]=");
    names.opShl = intern("<<");
    names.opShr = intern(">>");
    names.opBitAnd = intern("&");
    names.opBitOr = intern("|");
    names.opBitXor = intern("^");
    names.opBitNot = intern("unary~");

    opNames_[static_cast<int>(Op::Add)] = names.opAdd;
    opNames_[static_cast<int>(Op::Sub)] = names.opSub;
    opNames_[static_cast<int>(Op::Mul)] = names.opMul;
    opNames_[static_cast<int>(Op::Div)] = names.opDiv;
    opNames_[static_cast<int>(Op::Mod)] = names.opMod;
    opNames_[static_cast<int>(Op::Shl)] = names.opShl;
    opNames_[static_cast<int>(Op::Shr)] = names.opShr;
    opNames_[static_cast<int>(Op::BitAnd)] = names.opBitAnd;
    opNames_[static_cast<int>(Op::BitOr)] = names.opBitOr;
    opNames_[static_cast<int>(Op::BitXor)] = names.opBitXor;
    opNames_[static_cast<int>(Op::Lt)] = names.opLt;
    opNames_[static_cast<int>(Op::Le)] = names.opLe;
    opNames_[static_cast<int>(Op::Gt)] = names.opGt;
    opNames_[static_cast<int>(Op::Ge)] = names.opGe;

    registerBuiltins(*this);
}

VM::~VM() {
    parallelPool_.reset();  // its cores first: they work on this VM's objects
    Obj* obj = objects_;
    while (obj != nullptr) {
        Obj* next = obj->next;
        delete obj;
        obj = next;
    }
}

// ---- Strings ----

ObjString* VM::intern(std::string_view s) {
    if (root_ != nullptr) {
        // A core's VM has no table of its own: a name is the same object for every core.
        std::lock_guard<std::mutex> hold(root_->internMutex_);
        return root_->intern(s);
    }
    auto it = interned_.find(s);
    if (it != interned_.end()) return it->second;

    ObjString* str = alloc<ObjString>(std::string(s));
    str->immortal = true;
    interned_.emplace(std::string_view(str->chars), str);
    return str;
}

ObjString* VM::newString(std::string s) {
    account(s.size());
    return alloc<ObjString>(std::move(s));
}

// ---- Setup ----

void VM::defineNative(const std::string& name, int minArgs, int maxArgs, NativeFn fn) {
    builtins_[name] = Value::object(alloc<ObjNative>(name, minArgs, maxArgs, fn));
}

void VM::defineMethod(BuiltinType type, const std::string& name, int minArgs, int maxArgs, NativeMethodFn fn) {
    methods_[static_cast<int>(type)][intern(name)] = NativeMethod{name, minArgs, maxArgs, fn, {}};
}

bool VM::builtinArity(const std::string& name, int& minArgs, int& maxArgs) const {
    auto found = builtins_.find(name);
    if (found == builtins_.end() || !found->second.isObjType(ObjType::Native)) return false;
    const ObjNative* native = static_cast<const ObjNative*>(found->second.as.obj);
    minArgs = native->minArgs;
    maxArgs = native->maxArgs;
    return true;
}

bool VM::methodArity(BuiltinType type, const std::string& name, int& minArgs, int& maxArgs) {
    const auto& table = methods_[static_cast<int>(type)];
    auto found = table.find(intern(name));
    if (found == table.end()) return false;
    minArgs = found->second.minArgs;
    maxArgs = found->second.maxArgs;
    return true;
}

void VM::nameParams(BuiltinType type, const std::string& method, const std::vector<std::string>& params) {
    auto found = methods_[static_cast<int>(type)].find(intern(method));
    if (found == methods_[static_cast<int>(type)].end()) return;
    if (params.size() > found->second.params.size()) found->second.params = params;
}

void VM::nameFunctionParams(const std::string& function, const std::vector<std::string>& params) {
    auto found = builtins_.find(function);
    if (found == builtins_.end() || !found->second.isObjType(ObjType::Native)) return;
    ObjNative* native = asNative(found->second);
    if (params.size() > native->params.size()) native->params = params;
}

bool VM::builtinTypeNamed(const std::string& name, BuiltinType& type) {
    if (name == "string") type = BuiltinType::String;
    else if (name == "List") type = BuiltinType::List;
    else if (name == "Map") type = BuiltinType::Map;
    else if (name == "Range") type = BuiltinType::Range;
    else if (name == "Bytes") type = BuiltinType::Bytes;
    else if (name == "Number" || name == "int" || name == "float") type = BuiltinType::Number;
    else return false;
    return true;
}

void VM::collect(Value iterable, std::vector<Value>& out) {
    guardRead(iterable);
    if (iterable.isObjType(ObjType::List)) {
        const std::vector<Value>& items = asList(iterable)->items;
        out.insert(out.end(), items.begin(), items.end());
        return;
    }
    TempVector held(*this, out);
    TempRoot iterator(*this, makeIterator(iterable));
    ObjIterator* it = asIterator(iterator.get());
    Value item;
    while (iteratorNext(it, item)) out.push_back(item);
}

void VM::load(Program& program) {
    program_ = &program;

    size_t known = globals_.size();
    globals_.resize(program.globalNames.size(), Value::undefined());
    globalTypes_.resize(globals_.size());
    for (size_t i = known; i < globals_.size(); i++) {
        auto it = builtins_.find(program.globalNames[i]);
        if (it != builtins_.end()) globals_[i] = it->second;
    }
    loaded_.resize(program.modules.size(), false);
}

Value VM::runModule(int index) {
    if (!loaded_[0]) {
        // The prelude defines Error, which runtime errors are instances of.
        loaded_[0] = true;
        execute(program_->modules[0]);
        for (size_t i = 0; i < globals_.size(); i++) {
            if (program_->globalNames[i] == "Error" && globals_[i].isObjType(ObjType::Class))
                errorClass_ = asClass(globals_[i]);
            if (program_->globalNames[i] == "MapEntry" && globals_[i].isObjType(ObjType::Class) && mapEntryClass_ == nullptr)
                mapEntryClass_ = asClass(globals_[i]);
        }
    }
    loaded_[static_cast<size_t>(index)] = true;
    return execute(program_->modules[static_cast<size_t>(index)]);
}

Value VM::execute(const FunctionProto* proto) {
    try {
        ObjClosure* closure = alloc<ObjClosure>(proto, proto->upvalues.size());
        int base = frameCount_;
        *sp_++ = Value::object(closure);
        callClosure(closure, 0);
        run(base);
        return *--sp_;
    } catch (...) {
        resetState();
        throw;
    }
}

void VM::resetState() {
    closeUpvalues(stack_);
    sp_ = stack_;
    frameCount_ = 0;
    handlers_.clear();
    nativeDepth_ = 0;
}

Value VM::call(Value callee, int argc, const Value* args) {
    if (nativeDepth_ >= kMaxNativeDepth || stackEnd_ - sp_ < argc + 1) throw runtimeError("stack overflow");

    NativeCallGuard guard(nativeDepth_, nativeBase_);

    int base = frameCount_;
    *sp_++ = callee;
    for (int i = 0; i < argc; i++) *sp_++ = args[i];
    if (callValue(callee, argc)) run(base);
    return *--sp_;
}

Value VM::callOn(Value receiver, Value callee, int argc, const Value* args) {
    // Only a function written in Astra can be given an object to run on.
    if (!callee.isObjType(ObjType::Closure)) return call(callee, argc, args);
    if (nativeDepth_ >= kMaxNativeDepth || stackEnd_ - sp_ < argc + 1) throw runtimeError("stack overflow");

    NativeCallGuard guard(nativeDepth_, nativeBase_);

    int base = frameCount_;
    *sp_++ = receiver;  // slot 0, where a method has `this`
    for (int i = 0; i < argc; i++) *sp_++ = args[i];
    callClosure(asClosure(callee), argc);
    run(base);
    return *--sp_;
}

// The object a name in a lambda belongs to: the one the lambda is running on, or failing that
// the one an enclosing lambda is running on, whichever first has a member of that name.
// Undefined if none has, in which case the name is a global.
// The extension function called `name` that a value has, if any: one for its class, a class
// above it or an interface it implements; for its built-in type; or one for every value.
// An extension added to a type under a name: in place of one with the same signature (the
// receiver's type and the parameters' types, as written), or as another form beside those
// there already (D56). A name with several forms holds the set of them, and each form is also
// found under its own name, the name with its signature, for calls that know which they mean.
void VM::addExtensionForm(Table& table, ObjString* name, Value function) {
    ObjClosure* added = asClosure(function);
    const Value* existing = table.find(name);
    if (existing == nullptr || !existing->isObjType(ObjType::Closure) || added->proto->signature.empty()) {
        table.set(name, function);
        return;
    }
    TempRoot keep(*this, function);
    ObjClosure* before = asClosure(*existing);
    std::vector<ObjClosure*> forms;
    if (before->proto->chooser) forms = static_cast<const ObjForms*>(before)->forms;
    else forms.push_back(before);
    bool replaced = false;
    for (ObjClosure*& form : forms) {
        if (form->proto->signature.empty() || form->proto->signature == added->proto->signature) {
            form = added;
            replaced = true;
        }
    }
    if (!replaced) forms.push_back(added);
    if (forms.size() == 1) {
        table.set(name, function);
        return;
    }
    std::unique_ptr<FunctionProto>& proto = extensionSets_[name->chars];
    if (proto == nullptr) {
        proto = std::make_unique<FunctionProto>();
        proto->name = name->chars;
        proto->chooser = true;
    }
    for (ObjClosure* form : forms) table.set(intern(name->chars + form->proto->signature), Value::object(form));
    ObjForms* set = alloc<ObjForms>(proto.get());
    set->forms = std::move(forms);
    account(set->forms.size() * sizeof(ObjClosure*));
    table.set(name, Value::object(set));
}

const Value* VM::findExtension(Value receiver, const ObjString* name) const {
    if (!hasExtensions_) return nullptr;
    const Value* found = nullptr;
    switch (receiver.kind) {
        case ValueKind::Null:
            found = nullExtensions_.find(name);
            // Two types each have one that takes null (string?.orEmpty, list?.orEmpty), and
            // null does not say which is meant.
            if (found != nullptr && !found->isObjType(ObjType::Closure))
                throw const_cast<VM*>(this)->runtimeError(
                    name->chars + "() is called on null, and more than one type has its own " + name->chars +
                    "() that takes null, so there is no telling which is meant; use ?: to say what stands in for null");
            break;
        case ValueKind::Bool: found = typeExtensions_[kTypeBool].find(name); break;
        case ValueKind::Int: found = typeExtensions_[kTypeInt].find(name); break;
        case ValueKind::Float: found = typeExtensions_[kTypeFloat].find(name); break;
        case ValueKind::Obj:
            switch (receiver.as.obj->type) {
                case ObjType::String: found = typeExtensions_[kTypeString].find(name); break;
                case ObjType::List: found = typeExtensions_[kTypeList].find(name); break;
                case ObjType::Map: found = typeExtensions_[kTypeMap].find(name); break;
                case ObjType::Range: found = typeExtensions_[kTypeRange].find(name); break;
                case ObjType::Bytes: found = typeExtensions_[kTypeBytes].find(name); break;
                case ObjType::Closure:
                case ObjType::Native:
                case ObjType::BoundMethod:
                case ObjType::BoundNative: found = typeExtensions_[kTypeFunction].find(name); break;
                case ObjType::Instance: {
                    const ObjClass* cls = asInstance(receiver)->cls;
                    for (const ObjClass* k = cls; k != nullptr && found == nullptr; k = k->super) found = k->extensions.find(name);
                    for (size_t i = 0; i < cls->interfaces.size() && found == nullptr; i++)
                        found = cls->interfaces[i]->extensions.find(name);
                    break;
                }
                default: break;
            }
            break;
        default: break;
    }
    if (found == nullptr && receiver.isNumber()) found = typeExtensions_[kTypeNumber].find(name);
    if (found == nullptr && !receiver.isNull()) found = anyExtensions_.find(name);
    return found;
}

// Whether a bare name written in code that runs on `candidate` means a member of it.
bool VM::hasMember(Value candidate, const ObjString* name) const {
    guardRead(candidate);
    if (candidate.isObjType(ObjType::Instance)) {
        const ObjInstance* instance = asInstance(candidate);
        const ObjClass* cls = instance->cls;
        if (instance->fields.find(name) != nullptr || cls->methods.find(name) != nullptr ||
            cls->declaredIndex.find(name) != nullptr ||
            (cls->hasAccessors && (cls->getters.find(name) != nullptr || cls->setters.find(name) != nullptr)))
            return true;
        return findExtension(candidate, name) != nullptr;
    }
    // Slot 0 of an ordinary function is the function itself, which has no members to speak of.
    if (candidate.isObjType(ObjType::Closure) || candidate.isObjType(ObjType::Native) ||
        candidate.isObjType(ObjType::BoundMethod) || candidate.isObjType(ObjType::BoundNative) ||
        candidate.isObjType(ObjType::Class))
        return false;
    // An extension function on a built-in type runs on a string, a list, a number...
    if (!hasExtensions_) return false;
    if (findNativeMethod(candidate, name) != nullptr || findExtension(candidate, name) != nullptr) return true;
    if (!candidate.isObj()) return false;
    const ObjType type = candidate.as.obj->type;
    if (name == names.length || name == names.size)
        return type == ObjType::String || type == ObjType::List || type == ObjType::Map || type == ObjType::Range ||
               type == ObjType::Bytes;
    if (name == names.indices || name == names.lastIndex) return type == ObjType::String || type == ObjType::List;
    if (name == names.code) return type == ObjType::String;
    return type == ObjType::Range && (name == names.start || name == names.end || name == names.step);
}

// ---- Who may use a member ----
//
// A member written `private`, `protected` or `internal` belongs to some of the program's
// code only. The compiler gives every class and every library a number, and every function
// carries the numbers of the classes it is written inside of and of its library. The name
// of such a member leads to a mark: the numbers a function has to carry one of.
//
//   private     the class that declared it, and a subclass that declared it again, private too
//   protected   the class that declared it and each of its subclasses, down to the object's own
//   internal    the library the class is in
//
// A mark has two halves of 32 bits, the lower for using the member at all and the upper for
// assigning to it (`private set`). A half is count | kind << 8 | offset << 12, the offset
// and count of a run of numbers in the class's `accessIds`; a count of 0 is open to all.

namespace {

enum AccessKind { kAccessOpen = 0, kAccessPrivate = 1, kAccessProtected = 2, kAccessInternal = 3, kAccessAsAbove = 4 };
const int kAccessOfSetter = 0x10;

inline uint32_t halfOf(const Value& mark, bool write) {
    return static_cast<uint32_t>(static_cast<uint64_t>(mark.as.i) >> (write ? 32 : 0));
}
inline uint32_t halfCount(uint32_t half) { return half & 0xFF; }
inline int halfKind(uint32_t half) { return static_cast<int>((half >> 8) & 0xF); }
inline const uint16_t* halfRun(const ObjClass* cls, uint32_t half) { return cls->accessIds.data() + (half >> 12); }

// Whether a half keeps the member from the code of `proto`.
inline bool halfDenies(const ObjClass* cls, uint32_t half, const FunctionProto* proto) {
    const uint32_t count = halfCount(half);
    if (count == 0) return false;
    const uint16_t* allowed = halfRun(cls, half);
    for (uint16_t written : proto->classIds)
        for (uint32_t i = 0; i < count; i++)
            if (allowed[i] == written) return false;
    return true;
}

inline bool writtenIn(const FunctionProto* proto, const ObjClass* cls) {
    for (uint16_t written : proto->classIds)
        if (written == cls->number) return true;
    return false;
}

}  // namespace

// Gives one half of the mark of `name` a run of numbers, or none: open.
void VM::setAccess(ObjClass* cls, Table& marks, ObjString* name, bool write, const std::vector<uint16_t>& run, int kind) {
    uint32_t half = 0;
    if (!run.empty()) {
        if (run.size() > 0xFF || cls->accessIds.size() + run.size() > 0xFFFFF)
            throw runtimeError("class " + cls->name->chars + " has too many private, protected and internal members");
        half = static_cast<uint32_t>(run.size()) | static_cast<uint32_t>(kind) << 8 |
               static_cast<uint32_t>(cls->accessIds.size()) << 12;
        cls->accessIds.insert(cls->accessIds.end(), run.begin(), run.end());
        name->restricted |= (write ? 2 : 1) | (kind == kAccessPrivate ? 4 : 0);
        if (&marks == &cls->accessMarks) cls->accessBits |= 1ull << (name->hash & 63);
    }
    const Value* before = marks.find(name);
    uint64_t mark = before != nullptr ? static_cast<uint64_t>(before->as.i) : 0;
    mark = write ? (mark & 0xFFFFFFFFull) | static_cast<uint64_t>(half) << 32 : (mark & ~0xFFFFFFFFull) | half;
    marks.set(name, Value::integer(static_cast<int64_t>(mark)));
}

// What a class says of a member it declares: the access word in front of it, for the code
// with the number `id`.
void VM::declareAccess(ObjClass* cls, Table& marks, ObjString* name, int id, int kind, bool write) {
    const Value* before = marks.find(name);
    const uint32_t half = before != nullptr ? halfOf(*before, write) : 0;
    if (kind == kAccessOpen || kind == kAccessAsAbove) {
        // `public override` opens it. `override` alone leaves it as it was above, except
        // that what was private above is a new, open member here.
        if (halfCount(half) != 0 && (kind == kAccessOpen || halfKind(half) == kAccessPrivate)) setAccess(cls, marks, name, write, {}, 0);
        return;
    }
    // Declared again by a subclass, it is the subclass's too.
    std::vector<uint16_t> run(halfRun(cls, half), halfRun(cls, half) + halfCount(half));
    if (std::find(run.begin(), run.end(), static_cast<uint16_t>(id)) == run.end()) run.push_back(static_cast<uint16_t>(id));
    setAccess(cls, marks, name, write, run, kind);
}

// A class that has just been given its superclass: what is protected above is its own code's
// to use as well, on its own objects, and so are the protected static members above.
void VM::joinProtected(ObjClass* subclass) {
    auto join = [&](ObjClass* owner, Table& marks) {
        std::vector<ObjString*> names;
        for (uint32_t at = 0; at < marks.capacity(); at++)
            if (marks.entries()[at].key != nullptr) names.push_back(marks.entries()[at].key);
        for (ObjString* name : names) {
            for (bool write : {false, true}) {
                const uint32_t half = halfOf(*marks.find(name), write);
                if (halfCount(half) == 0 || halfKind(half) != kAccessProtected) continue;
                std::vector<uint16_t> run(halfRun(owner, half), halfRun(owner, half) + halfCount(half));
                if (std::find(run.begin(), run.end(), static_cast<uint16_t>(subclass->number)) != run.end()) continue;
                run.push_back(static_cast<uint16_t>(subclass->number));
                // A class above is everyone's: a core of a parallel section leaves it alone.
                if (owner != subclass && coreId_ != 0)
                    throw notOwned("class " + owner->name->chars + " has protected static members, and a subclass of it is declared for the whole program");
                setAccess(owner, marks, name, write, run, kAccessProtected);
            }
        }
    };
    if (subclass->number == 0) return;
    join(subclass, subclass->accessMarks);
    for (ObjClass* above = subclass->super; above != nullptr; above = above->super) join(above, above->staticAccessMarks);
}

// Whether the code of `proto` may not use the member `name` of `target` (an object, or a
// class for its static members), or may not assign to it. This is only asked for a name that
// some class restricts (see CHECK_ACCESS).
bool VM::accessDenied(Value target, const ObjString* name, const FunctionProto* proto, bool write) const {
    if (!target.isObj()) return false;
    const ObjClass* cls;
    const Table* marks;
    if (target.as.obj->type == ObjType::Instance) {
        cls = asInstance(target)->cls;
        marks = &cls->accessMarks;
        // Code written in the object's own class may use all that the class has, or all but
        // what a class above keeps private: then a name that is private nowhere is its to use.
        if ((cls->accessBits >> (name->hash & 63) & 1) == 0) return false;
        if ((cls->accessAllOwn || (cls->accessSharedOwn && (name->restricted & 4) == 0)) && writtenIn(proto, cls)) return false;
    } else if (target.as.obj->type == ObjType::Class) {
        cls = asClass(target);
        marks = &cls->staticAccessMarks;
        if (marks->size() == 0 || writtenIn(proto, cls)) return false;
    } else {
        return false;
    }
    const Value* mark = marks->find(name);
    if (mark == nullptr) return false;
    return halfDenies(cls, halfOf(*mark, false), proto) || (write && halfDenies(cls, halfOf(*mark, true), proto));
}

// What is said of a member used by code that may not.
AstraThrow VM::accessError(Value target, const ObjString* name, const FunctionProto* proto) {
    const bool isStatic = target.isObjType(ObjType::Class);
    const ObjClass* cls = isStatic ? asClass(target) : asInstance(target)->cls;
    const Value* mark = (isStatic ? cls->staticAccessMarks : cls->accessMarks).find(name);
    if (mark == nullptr) return runtimeError("'" + name->chars + "' cannot be used here");
    const bool write = !halfDenies(cls, halfOf(*mark, false), proto);
    const uint32_t half = halfOf(*mark, write);
    const int kind = halfKind(half);

    // The class that said so: the highest one that has the member marked as this one does. A
    // protected member's mark grows by a number with every subclass, so its start is compared.
    const ObjClass* declaring = cls;
    auto same = [&](const ObjClass* above) {
        const Value* other = above->accessMarks.find(name);
        const uint32_t theirs = other != nullptr ? halfOf(*other, write) : 0;
        if (halfCount(theirs) == 0 || halfKind(theirs) != kind) return false;
        if (kind == kAccessProtected) return halfRun(above, theirs)[0] == halfRun(cls, half)[0];
        return halfCount(theirs) == halfCount(half) &&
               std::equal(halfRun(cls, half), halfRun(cls, half) + halfCount(half), halfRun(above, theirs));
    };
    while (!isStatic && declaring->super != nullptr && same(declaring->super)) declaring = declaring->super;

    const char* const word = kind == kAccessProtected ? "protected" : kind == kAccessInternal ? "internal" : "private";
    const char* const who = kind == kAccessProtected ? "only code written in that class or in a subclass of it can "
                            : kind == kAccessInternal ? "only code of the library it belongs to can "
                            : "only code written in that class can ";
    if (write)
        return runtimeError("'" + name->chars + "' cannot be assigned here: its setter is " + word + " in class " +
                            declaring->name->chars + ", so " + who + "assign to it");
    return runtimeError("'" + name->chars + "' is " + word + " in class " + declaring->name->chars + ": " + who + "use it");
}

Value VM::implicitReceiver(const CallFrame* frame, const Value* base, const ObjString* name) const {
    const FunctionProto* proto = frame->closure->proto;
    auto has = [&](Value candidate) {
        return hasMember(candidate, name) && !(name->restricted != 0 && accessDenied(candidate, name, proto, false));
    };
    if (has(base[0])) return base[0];
    for (uint16_t upvalue : proto->outerReceivers) {
        Value outer = *frame->closure->upvalues[upvalue]->location;
        if (has(outer)) return outer;
    }
    return Value::undefined();
}

// A bare name that is neither a member the code may use nor a variable: said plainly when
// it is a member of the object the code runs on that the code may not use.
AstraThrow VM::undefinedName(const CallFrame* frame, const Value* base, const ObjString* name, size_t global) {
    const FunctionProto* proto = frame->closure->proto;
    auto kept = [&](Value candidate) { return hasMember(candidate, name) && accessDenied(candidate, name, proto, false); };
    if (kept(base[0])) return accessError(base[0], name, proto);
    for (uint16_t upvalue : proto->outerReceivers) {
        Value outer = *frame->closure->upvalues[upvalue]->location;
        if (kept(outer)) return accessError(outer, name, proto);
    }
    return runtimeError("undefined variable '" + program_->globalNames[global] + "'");
}

// Runs a method that takes no arguments on `receiver` and returns its result.
Value VM::callWithReceiver(Value receiver, ObjClosure* method) {
    if (nativeDepth_ >= kMaxNativeDepth || stackEnd_ - sp_ < 1) throw runtimeError("stack overflow");

    NativeCallGuard guard(nativeDepth_, nativeBase_);

    int base = frameCount_;
    *sp_++ = receiver;
    callClosure(method, 0);
    run(base);
    return *--sp_;
}

// ---- Errors ----

AstraThrow VM::runtimeError(const std::string& message) {
    AstraThrow t;
    if (errorClass_ == nullptr) {
        t.value = str(message);
    } else {
        ObjInstance* error = alloc<ObjInstance>(errorClass_);
        error->fields.set(names.message, str(message));
        t.value = Value::object(error);
    }
    return t;
}

AstraThrow VM::errorOf(const char* className, const std::string& message) {
    // The first global of that name is the standard library's, which is loaded first.
    for (size_t i = 0; program_ != nullptr && i < globals_.size(); i++) {
        if (!globals_[i].isObjType(ObjType::Class) || program_->globalNames[i] != className) continue;
        ObjInstance* error = alloc<ObjInstance>(asClass(globals_[i]));
        error->fields.set(names.message, str(message));
        AstraThrow thrown;
        thrown.value = Value::object(error);
        return thrown;
    }
    return runtimeError(message);
}

// What the program is told when memory runs out: an OutOfMemoryError, which it can catch.
AstraThrow VM::outOfMemory() {
    return errorOf("OutOfMemoryError", "out of memory");
}

std::vector<std::string> VM::captureTrace() const {
    std::vector<std::string> trace;
    for (int i = frameCount_ - 1; i >= 0; i--) {
        if (frameCount_ > 14 && i == frameCount_ - 11) {
            trace.push_back("... " + std::to_string(i - 1) + " more calls ...");
            i = 1;
        }
        const CallFrame& f = frames_[static_cast<size_t>(i)];
        const FunctionProto* p = f.closure->proto;
        size_t offset = static_cast<size_t>(f.ip - p->code.data());
        if (offset > 0) offset--;
        trace.push_back("at " + p->name + " (" + p->source + ":" + std::to_string(p->lines[offset]) + ")");
    }
    return trace;
}

// Transfers control to the innermost catch block, if it belongs to the run() that is asking.
bool VM::tryHandle(Value exception, int baseFrame) {
    if (handlers_.empty()) return false;
    Handler h = handlers_.back();
    if (h.frameIndex < baseFrame) return false;  // belongs to an outer run()

    handlers_.pop_back();
    closeUpvalues(h.stackTop);
    frameCount_ = h.frameIndex + 1;
    sp_ = h.stackTop;
    *sp_++ = exception;
    frames_[static_cast<size_t>(h.frameIndex)].ip = h.ip;
    return true;
}

void VM::unwindTo(int baseFrame) {
    Value* base = frames_[static_cast<size_t>(baseFrame)].base;
    closeUpvalues(base);
    sp_ = base;
    frameCount_ = baseFrame;
}

std::string VM::typeName(Value v) const {
    switch (v.kind) {
        case ValueKind::Null: return "null";
        case ValueKind::Bool: return "bool";
        case ValueKind::Int: return "int";
        case ValueKind::Float: return "float";
        case ValueKind::Undefined: return "undefined";
        case ValueKind::Obj: break;
    }
    switch (v.as.obj->type) {
        case ObjType::String: return "string";
        case ObjType::List: return "List";
        case ObjType::Map: return "Map";
        case ObjType::Range: return "Range";
        case ObjType::Bytes: return "Bytes";
        case ObjType::Closure:
        case ObjType::Native:
        case ObjType::BoundMethod:
        case ObjType::BoundNative: return "Function";
        case ObjType::Class: return "Class";
        case ObjType::Instance: return asInstance(v)->cls->name->chars;
        case ObjType::Fiber: return "flow";
        case ObjType::Resource: return static_cast<const ObjResource*>(v.as.obj)->kind;
        case ObjType::Upvalue:
        case ObjType::Iterator: return "internal";
    }
    return "unknown";
}

int64_t VM::toInt(Value v, const char* what) {
    if (!v.isInt()) throw runtimeError(std::string(what) + " must be an int, got " + typeName(v));
    return v.as.i;
}

double VM::toNumber(Value v, const char* what) {
    if (!v.isNumber()) throw runtimeError(std::string(what) + " must be a number, got " + typeName(v));
    return v.toDouble();
}

const std::string& VM::toStr(Value v, const char* what) {
    if (!v.isString()) throw runtimeError(std::string(what) + " must be a string, got " + typeName(v));
    return asString(v)->chars;
}

// ---- Calls ----

void VM::checkArity(const std::string& name, int minArgs, int maxArgs, int argc) {
    if (argc >= minArgs && (maxArgs < 0 || argc <= maxArgs)) return;
    std::string expected = maxArgs < 0 ? "at least " + std::to_string(minArgs)
                           : minArgs == maxArgs ? std::to_string(minArgs)
                           : std::to_string(minArgs) + " to " + std::to_string(maxArgs);
    throw runtimeError(name + "() expects " + expected + " argument(s) but got " + std::to_string(argc));
}

// A bare name was passed where nothing says which enum it could belong to.
AstraThrow VM::unsettledName(Value pending) {
    pendingEnums_--;
    return runtimeError("undefined variable '" + static_cast<ObjString*>(pending.as.obj)->chars + "'");
}

void VM::rejectPendingNames(const Value* args, int argc) {
    for (int i = 0; i < argc; i++)
        if (args[i].kind == ValueKind::EnumName) throw unsettledName(args[i]);
}

// The name has reached a parameter declared with the class `type`: it is the entry of that
// name if the class is an enum, and an error otherwise.
Value VM::settleEnumName(Value pending, const ObjClass* type) {
    if (type != nullptr && type->staticInit != nullptr) ensureClassInit(const_cast<ObjClass*>(type));
    if (type == nullptr || !type->isEnum) throw unsettledName(pending);
    pendingEnums_--;
    const ObjString* name = static_cast<ObjString*>(pending.as.obj);
    const Value* entry = type->statics.find(name);
    if (entry == nullptr || !entry->isObjType(ObjType::Instance) || asInstance(*entry)->cls != type)
        throw runtimeError("'" + name->chars + "' is not an entry of enum " + type->name->chars);
    return *entry;
}

// For a function with a `vararg` parameter: gathers the arguments from that parameter's
// position on into a list, leaving the parameters after it to their defaults. Returns the
// new argument count.
int VM::packVarargs(const FunctionProto* proto, int argc) {
    const int at = proto->varargIndex;
    if (argc < proto->requiredArgs()) checkArity(proto->name, proto->requiredArgs(), -1, argc);
    if (stackEnd_ - (sp_ - argc - 1) < proto->maxStack) throw runtimeError("stack overflow");

    for (; argc < at; argc++) *sp_++ = Value::undefined();
    const int extra = argc - at;
    if (pendingEnums_ > 0) rejectPendingNames(sp_ - extra, extra);
    ObjList* list = alloc<ObjList>();
    list->items.assign(sp_ - extra, sp_);
    account(static_cast<size_t>(extra) * sizeof(Value));
    sp_ -= extra;
    *sp_++ = Value::object(list);
    return at + 1;
}

// How well an argument fits a parameter of a form: -1 not at all, 0 a parameter without a
// type, more the narrower the type is (a class nearer the object's own, more still).
int VM::paramFit(Value v, uint8_t type, const ObjString* className) const {
    if (type == 0) return 0;
    if ((type & kTypeMask) != kTypeInstance) {
        if (!matchesType(v, type, nullptr)) return -1;
        return (type & kTypeMask) == kTypeNumber ? 1 : 2;
    }
    if (v.isNull()) return (type & kTypeNullable) != 0 ? 1 : -1;
    if (!v.isObjType(ObjType::Instance) || className == nullptr) return -1;
    // The class is known by its name, as written: the last part of it.
    auto named = [&](const ObjClass* c) {
        const std::string& n = c->name->chars;
        const std::string& want = className->chars;
        return n == want || (n.size() > want.size() && n[n.size() - want.size() - 1] == '.' &&
                             n.compare(n.size() - want.size(), want.size(), want) == 0);
    };
    int distance = 0;
    for (const ObjClass* c = asInstance(v)->cls; c != nullptr; c = c->super, distance++) {
        if (named(c)) return 2 + std::max(0, 64 - distance);
        for (const ObjClass* i : c->interfaces)
            if (named(i)) return 2 + std::max(0, 63 - distance);
    }
    return -1;
}

// Whether a form takes a call's arguments: as many as there are, each of a type its parameter
// takes. `pairs` holds a named call's (name, value) pairs; the empty name is a lambda written
// after the call, which goes where callNamed puts it. `score` says how narrowly the types fit.
bool VM::formTakes(const FunctionProto* proto, const Value* args, int positional, const Value* pairs, int named,
                   int& score, int& defaultsLeft) const {
    const int arity = proto->arity;
    const int vararg = proto->varargIndex;
    const int required = proto->requiredArgs();
    if (vararg < 0 && positional > arity) return false;
    const bool typed = proto->paramTypes.size() == static_cast<size_t>(arity);
    auto fit = [&](int param, Value v) {
        if (!typed || param == vararg) return 0;
        const int f = paramFit(v, proto->paramTypes[static_cast<size_t>(param)], proto->paramClasses[static_cast<size_t>(param)]);
        // A function written in Astra, given for a parameter of a function type: it has to
        // take as many arguments as the type gives.
        const size_t counted = proto->paramCounts.size() > static_cast<size_t>(param) ? proto->paramCounts[static_cast<size_t>(param)] : 0;
        if (f >= 0 && counted > 0) {
            const FunctionProto* given = v.isObjType(ObjType::Closure)       ? asClosure(v)->proto
                                         : v.isObjType(ObjType::BoundMethod) ? asBoundMethod(v)->method->proto
                                                                             : nullptr;
            const int wanted = static_cast<int>(counted) - 1;
            if (given != nullptr && !given->chooser && given->varargIndex < 0 && (wanted < given->requiredArgs() || wanted > given->arity))
                return -1;
        }
        return f;
    };
    uint64_t given = 0;  // the parameters given a value (the first 64)
    auto give = [&](int param) {
        if (param < 64) given |= uint64_t{1} << param;
    };
    auto isGiven = [&](int param) { return param < 64 && ((given >> param) & 1) != 0; };
    score = 0;
    const int direct = vararg >= 0 && positional > vararg ? vararg : positional;
    for (int i = 0; i < direct; i++) {
        const int f = fit(i, args[i]);
        if (f < 0) return false;
        score += f;
        give(i);
    }
    if (vararg >= 0 && positional > vararg) give(vararg);
    for (int k = 0; k < named; k++) {
        const ObjString* name = asString(pairs[2 * k]);
        const Value value = pairs[2 * k + 1];
        int index = -1;
        if (name->chars.empty()) {
            if (arity == 0) return false;
            index = arity - 1;
        } else {
            for (int p = 0; p < arity && index < 0; p++)
                if (proto->paramNames.size() == static_cast<size_t>(arity) && proto->paramNames[static_cast<size_t>(p)] == name) index = p;
        }
        if (index < 0 || isGiven(index)) return false;
        const int f = fit(index, value);
        if (f < 0) return false;
        score += f;
        give(index);
    }
    defaultsLeft = 0;
    for (int p = 0; p < arity; p++) {
        if (isGiven(p) || p == vararg || p >= 64) continue;
        if (p < required) return false;
        defaultsLeft++;
    }
    return true;
}

// The form of a name with several (D56) that a call means, when the compiler could not tell
// (an argument whose type was not known, or code compiled on its own): of the forms that take
// the arguments, the one whose parameters' types fit them most narrowly; between two as
// narrow, one without a vararg, then one that leaves fewer parameters to their defaults.
ObjClosure* VM::chooseForm(const ObjForms* set, const Value* args, int positional, const Value* pairs, int named) {
    ObjClosure* best = nullptr;
    int bestScore = -1, bestDefaults = 0;
    bool bestVararg = false, tie = false;
    for (ObjClosure* form : set->forms) {
        int score = 0, defaults = 0;
        if (!formTakes(form->proto, args, positional, pairs, named, score, defaults)) continue;
        const bool vararg = form->proto->varargIndex >= 0;
        bool better = best == nullptr || score > bestScore;
        bool same = !better && score == bestScore;
        if (same && vararg != bestVararg) {
            better = !vararg;
            same = false;
        }
        if (same && defaults != bestDefaults) {
            better = defaults < bestDefaults;
            same = false;
        }
        if (better) {
            best = form;
            bestScore = score;
            bestDefaults = defaults;
            bestVararg = vararg;
            tie = false;
        } else if (same) {
            tie = true;
        }
    }
    const std::string& name = set->proto->name;
    auto formsText = [&](const std::vector<ObjClosure*>& forms) {
        std::string text;
        for (size_t i = 0; i < forms.size(); i++) {
            if (i > 0) text += i + 1 == forms.size() ? " and " : ", ";
            text += name + forms[i]->proto->signature;
        }
        return text;
    };
    if (best == nullptr) {
        std::string given;
        for (int i = 0; i < positional; i++) given += (given.empty() ? "" : ", ") + typeName(args[i]);
        for (int k = 0; k < named; k++) {
            const std::string& n = asString(pairs[2 * k])->chars;
            given += (given.empty() ? "" : ", ") + (n.empty() ? std::string() : n + " = ") + typeName(pairs[2 * k + 1]);
        }
        throw runtimeError("no form of " + name + "() takes " + (given.empty() ? std::string("no arguments") : "(" + given + ")") +
                           "; there are " + formsText(set->forms));
    }
    if (tie) {
        // Every form that fits as well as the best, in the order they were written.
        std::vector<ObjClosure*> tied;
        for (ObjClosure* form : set->forms) {
            int score = 0, defaults = 0;
            if (formTakes(form->proto, args, positional, pairs, named, score, defaults) && score == bestScore &&
                defaults == bestDefaults && (form->proto->varargIndex >= 0) == bestVararg)
                tied.push_back(form);
        }
        throw runtimeError("this call fits more than one form of " + name + "(): " + formsText(tied) +
                           "; write the types of what is passed, so that the form is chosen when compiling");
    }
    return best;
}

void VM::callClosure(ObjClosure* closure, int argc, bool packed) {
    if (closure->proto->chooser) closure = chooseForm(static_cast<const ObjForms*>(closure), sp_ - argc, argc, nullptr, 0);
    const FunctionProto* proto = closure->proto;
    if (proto->varargIndex >= 0 && !packed) argc = packVarargs(proto, argc);
    if (argc != proto->arity && (argc > proto->arity || argc < proto->requiredArgs()))
        checkArity(proto->name, proto->requiredArgs(), proto->arity, argc);

    Value* base = sp_ - argc - 1;
    if (frameCount_ == maxFrames_ || stackEnd_ - base < proto->maxStack) throw runtimeError("stack overflow");

    if (pendingEnums_ > 0) {
        // A bare name may only arrive at a parameter that could make an enum entry of it.
        for (int i = 0; i < argc; i++)
            if (base[i + 1].kind == ValueKind::EnumName && (i >= 64 || ((proto->classParams >> i) & 1) == 0))
                throw unsettledName(base[i + 1]);
    }

    // Arguments that were left out are marked, and the function fills in their defaults.
    for (int missing = argc; missing < proto->arity; missing++) *sp_++ = Value::undefined();

    CallFrame& frame = frames_[static_cast<size_t>(frameCount_++)];
    frame.dropResult = false;
    frame.closure = closure;
    frame.ip = proto->code.data();
    frame.base = base;
}

// A call whose last argument is a lambda written after the call, as in
// `items.joinToString(", ") { it.name }`. The lambda belongs to the last parameter, as in
// Kotlin, so the parameters between the other arguments and it are left to their defaults; a
// function that takes a lambda or not has a form for each (D56).
void VM::callTrailing(ObjClosure* closure, int argc) {
    if (closure->proto->chooser) {
        if (emptyName_ == nullptr) emptyName_ = intern("");
        const Value lambda[2] = {Value::object(emptyName_), sp_[-1]};
        closure = chooseForm(static_cast<const ObjForms*>(closure), sp_ - argc, argc - 1, lambda, 1);
    }
    const FunctionProto* proto = closure->proto;
    const int arity = proto->arity;
    const int vararg = proto->varargIndex;
    // Nothing to move: the lambda is already where the last parameter is, there are too many
    // arguments (reported as usual), or the last parameter gathers whatever is left.
    if (vararg == arity - 1 || (vararg < 0 && argc >= arity)) {
        callClosure(closure, argc);
        return;
    }
    if (stackEnd_ - sp_ < arity + 1) throw runtimeError("stack overflow");

    int given = argc - 1;
    const int target = arity - 1;
    if (given < proto->requiredArgs()) {
        // The lambda may pass over parameters that have a default, and no others.
        if (proto->paramNames.size() != static_cast<size_t>(arity)) checkArity(proto->name, proto->requiredArgs(), arity, argc);
        throw runtimeError(proto->name + "() is missing argument '" + proto->paramNames[static_cast<size_t>(given)]->chars + "'");
    }
    if (target == given && vararg < 0) {
        callClosure(closure, argc);  // already in its place; what follows takes its default
        return;
    }

    TempRoot lambda(*this, sp_[-1]);
    sp_--;
    bool packed = false;
    if (vararg >= 0) {
        for (; given < vararg; given++) *sp_++ = Value::undefined();
        const int extra = given - vararg;
        if (pendingEnums_ > 0) rejectPendingNames(sp_ - extra, extra);
        ObjList* list = alloc<ObjList>();
        list->items.assign(sp_ - extra, sp_);
        account(static_cast<size_t>(extra) * sizeof(Value));
        sp_ -= extra;
        *sp_++ = Value::object(list);
        given = vararg + 1;
        packed = true;
    }
    for (; given < target; given++) *sp_++ = Value::undefined();
    *sp_++ = lambda.get();
    callClosure(closure, arity, packed);
}

// callValue for such a call. Only a function written in Astra has parameters to look at; to
// anything else the lambda is simply the last argument.
bool VM::callValueTrailing(Value callee, int argc) {
    if (callee.isObjType(ObjType::Closure)) {
        callTrailing(asClosure(callee), argc);
        return true;
    }
    if (callee.isObjType(ObjType::BoundMethod)) {
        ObjBoundMethod* bound = asBoundMethod(callee);
        sp_[-argc - 1] = bound->receiver;
        callTrailing(bound->method, argc);
        return true;
    }
    return callValue(callee, argc);
}

// Calls the value sitting below `argc` arguments on the stack. Returns true if a new frame
// was pushed (the caller must then keep interpreting), false if the call already completed
// and left its result on the stack.
bool VM::callValue(Value callee, int argc, bool packed) {
    if (callee.isObj()) {
        switch (callee.as.obj->type) {
            case ObjType::Closure:
                callClosure(asClosure(callee), argc, packed);
                return true;

            case ObjType::Native: {
                ObjNative* native = asNative(callee);
                checkArity(native->name, native->minArgs, native->maxArgs, argc);
                if (pendingEnums_ > 0) rejectPendingNames(sp_ - argc, argc);
                if (coreId_ != 0) {
                    if (!native->parallelSafe)
                        throw runtimeError(native->name + "() cannot be used in a parallel section: nothing waits there, and what the " +
                                           "whole program has one of is left alone");
                    guardNative(sp_ - argc, argc);
                }
                nativeBase_ = sp_ - argc - 1;
                Value result = native->fn(*this, argc, sp_ - argc);
                if (result.kind == ValueKind::Undefined) return true;  // it set a call going in its place (tailCallOn)
                sp_ -= argc + 1;
                *sp_++ = result;
                return false;
            }

            case ObjType::Class: {
                ObjClass* cls = asClass(callee);
                if (cls->staticInit != nullptr) ensureClassInit(cls);  // an enum has its entries by now
                if (cls->isAbstract)
                    throw runtimeError(cls->name->chars + " is abstract and cannot be created; create a class that extends it");
                if (cls->isInterface)
                    throw runtimeError(cls->name->chars + " is an interface and cannot be created; create a class that implements it");
                if (cls->enumSealed)
                    throw runtimeError("enum " + cls->name->chars + " has only the entries it declares; " + cls->name->chars +
                                       "() cannot make another");
                ObjInstance* instance = alloc<ObjInstance>(cls);
                account(96);  // rough cost of the field table
                sp_[-argc - 1] = Value::object(instance);
                instance->constructing = true;
                if (cls->nextEntry != nullptr) {
                    // An enum entry: it knows its name before its init runs.
                    nameEntry(instance, cls->nextEntry, cls->nextOrdinal);
                    cls->nextEntry = nullptr;
                }
                // Declared fields get their initial values before the initializer runs.
                for (size_t i = 0; i < cls->fieldInits.size(); i++) callWithReceiver(Value::object(instance), cls->fieldInits[i]);
                if (cls->init != nullptr) {
                    callClosure(cls->init, argc, packed);
                    return true;
                }
                instance->constructing = false;
                if (argc != 0)
                    throw runtimeError(cls->name->chars + "() takes no arguments but got " + std::to_string(argc));
                return false;
            }

            case ObjType::BoundMethod: {
                ObjBoundMethod* bound = asBoundMethod(callee);
                sp_[-argc - 1] = bound->receiver;
                callClosure(bound->method, argc, packed);
                return true;
            }

            case ObjType::BoundNative: {
                ObjBoundNative* bound = asBoundNative(callee);
                const NativeMethod* method = bound->method;
                checkArity(method->name, method->minArgs, method->maxArgs, argc);
                if (pendingEnums_ > 0) rejectPendingNames(sp_ - argc, argc);
                if (coreId_ != 0) {
                    guardRead(bound->receiver);
                    if (method->mutates && bound->receiver.isObj()) guardWrite(bound->receiver.as.obj);
                    guardNative(sp_ - argc, argc);
                }
                nativeBase_ = sp_ - argc - 1;
                Value result = method->fn(*this, bound->receiver, argc, sp_ - argc);
                sp_ -= argc + 1;
                *sp_++ = result;
                return false;
            }

            default:
                break;
        }
    }
    throw runtimeError("cannot call a value of type " + typeName(callee));
}

// A call with arguments passed by name. The stack holds the callee, the positional
// arguments, then a (name, value) pair for each named one. They are rearranged into the
// callee's parameter order, leaving the "not passed" mark for parameters that have a default,
// and the call then proceeds as usual.
bool VM::callNamed(int positional, int named) {
    Value* first = sp_ - positional - 2 * named;
    Value callee = first[-1];

    // A lambda written after a call to a built-in function is simply its last argument.
    if (named == 1 && sp_[-2].isString() && asString(sp_[-2])->chars.empty() &&
        (callee.isObjType(ObjType::Native) || callee.isObjType(ObjType::BoundNative))) {
        sp_[-2] = sp_[-1];
        sp_--;
        return callValue(callee, positional + 1);
    }

    // The function whose parameter names the arguments are matched against.
    const FunctionProto* proto = nullptr;
    auto display = [&]() -> std::string {
        return callee.isObjType(ObjType::Class) ? asClass(callee)->name->chars : proto->name;
    };
    if (callee.isObj()) {
        switch (callee.as.obj->type) {
            case ObjType::Closure:
                if (asClosure(callee)->proto->chooser) {
                    callee = Value::object(chooseForm(static_cast<const ObjForms*>(asClosure(callee)), first, positional,
                                                      first + positional, named));
                    first[-1] = callee;
                }
                proto = asClosure(callee)->proto;
                break;
            case ObjType::BoundMethod:
                if (asBoundMethod(callee)->method->proto->chooser) {
                    // The form is called on the object as a method is: the object in slot 0.
                    first[-1] = asBoundMethod(callee)->receiver;
                    callee = Value::object(chooseForm(static_cast<const ObjForms*>(asBoundMethod(callee)->method), first,
                                                      positional, first + positional, named));
                }
                proto = callee.isObjType(ObjType::Closure) ? asClosure(callee)->proto : asBoundMethod(callee)->method->proto;
                break;
            case ObjType::Class: {
                ObjClass* cls = asClass(callee);
                if (cls->init == nullptr) throw runtimeError(cls->name->chars + "() takes no arguments");
                proto = cls->init->proto;
                break;
            }
            case ObjType::Native:
                if (!asNative(callee)->params.empty()) return callNativeNamed(positional, named);
                throw runtimeError("built-in function " + asNative(callee)->name + "() does not take named arguments");
            case ObjType::BoundNative:
                if (!asBoundNative(callee)->method->params.empty()) return callNativeNamed(positional, named);
                throw runtimeError("built-in method " + asBoundNative(callee)->method->name +
                                   "() does not take named arguments");
            default:
                break;
        }
    }
    if (proto == nullptr) throw runtimeError("cannot call a value of type " + typeName(callee));

    int arity = proto->arity;
    if (proto->paramNames.size() != static_cast<size_t>(arity))
        throw runtimeError(display() + "() does not take named arguments");
    const int vararg = proto->varargIndex;
    if (positional > arity && vararg < 0) checkArity(display(), proto->requiredArgs(), arity, positional + named);

    // The common case of a lambda after the call: every parameter before the last was given,
    // so the lambda is simply the next argument.
    if (named == 1 && vararg < 0 && positional + 1 == arity && asString(sp_[-2])->chars.empty()) {
        sp_[-2] = sp_[-1];
        sp_--;
        return callValue(callee, positional + 1);
    }

    // Scratch space kept between calls. It is emptied back onto the stack before the callee
    // runs, so a named call made from inside the callee can use it again.
    std::vector<Value>& arranged = namedScratch_;
    arranged.resize(static_cast<size_t>(arity));
    const int direct = vararg >= 0 && positional > vararg ? vararg : positional;
    for (int i = 0; i < arity; i++) arranged[static_cast<size_t>(i)] = i < direct ? first[i] : Value::undefined();
    if (vararg >= 0 && positional > vararg) {
        // The positional arguments from the vararg parameter on are its list.
        ObjList* list = alloc<ObjList>();
        list->items.assign(first + vararg, first + positional);
        account(static_cast<size_t>(positional - vararg) * sizeof(Value));
        arranged[static_cast<size_t>(vararg)] = Value::object(list);
    }

    const Value* pair = first + positional;
    for (int i = 0; i < named; i++, pair += 2) {
        if (!pair[0].isString()) throw runtimeError("invalid call");
        const ObjString* name = asString(pair[0]);  // interned, like the parameter names

        int index = -1;
        if (name->chars.empty()) {
            // The lambda written after the call: it is the last parameter (see callTrailing).
            if (arity == 0) throw runtimeError(display() + "() takes no arguments, so no lambda can follow the call");
            index = arity - 1;
            name = proto->paramNames[static_cast<size_t>(index)];
        }
        for (int p = 0; index < 0 && p < arity; p++) {
            if (proto->paramNames[static_cast<size_t>(p)] == name) index = p;
        }
        if (index < 0) throw runtimeError(display() + "() has no parameter named '" + name->chars + "'");
        if (arranged[static_cast<size_t>(index)].kind != ValueKind::Undefined)
            throw runtimeError("argument '" + name->chars + "' of " + display() + "() is given more than once");
        arranged[static_cast<size_t>(index)] = pair[1];
    }

    if (vararg >= 0) {
        Value& gathered = arranged[static_cast<size_t>(vararg)];
        if (gathered.kind == ValueKind::Undefined) {
            gathered = Value::object(alloc<ObjList>());
        } else if (!gathered.isObjType(ObjType::List)) {
            throw runtimeError("argument '" + proto->paramNames[static_cast<size_t>(vararg)]->chars + "' of " + display() +
                               "() is a vararg parameter: by name it takes a list, not a value of type " + typeName(gathered));
        }
    }

    // Only parameters with a default value may be left out.
    for (int i = 0; i < proto->requiredArgs(); i++)
        if (arranged[static_cast<size_t>(i)].kind == ValueKind::Undefined)
            throw runtimeError(display() + "() is missing argument '" + proto->paramNames[static_cast<size_t>(i)]->chars + "'");

    if (stackEnd_ - first < arity) throw runtimeError("stack overflow");
    for (int i = 0; i < arity; i++) first[i] = arranged[static_cast<size_t>(i)];
    sp_ = first + arity;
    return callValue(callee, arity, vararg >= 0);
}

// A built-in method called with arguments by name, as in text.contains("a", ignoreCase = true).
// Only the methods that say what their parameters are called take them. A parameter that is
// passed over on the way to a named one is given null, which such a method reads as left out.
bool VM::callNativeNamed(int positional, int named) {
    Value* first = sp_ - positional - 2 * named;
    Value callee = first[-1];
    // A built-in method of a value, or a built-in function: what it is called and takes.
    struct Native {
        const std::vector<std::string>& params;
        const std::string& name;
        int minArgs, maxArgs;
    };
    const Native native = callee.isObjType(ObjType::BoundNative)
                              ? Native{asBoundNative(callee)->method->params, asBoundNative(callee)->method->name,
                                       asBoundNative(callee)->method->minArgs, asBoundNative(callee)->method->maxArgs}
                              : Native{asNative(callee)->params, asNative(callee)->name, asNative(callee)->minArgs,
                                       asNative(callee)->maxArgs};
    const Native* method = &native;
    const int count = static_cast<int>(method->params.size());
    if (positional > count) checkArity(method->name, method->minArgs, method->maxArgs, positional + named);

    std::vector<Value>& arranged = namedScratch_;
    arranged.assign(static_cast<size_t>(count), Value::undefined());
    for (int i = 0; i < positional; i++) arranged[static_cast<size_t>(i)] = first[i];
    int given = positional;
    const Value* pair = first + positional;
    for (int i = 0; i < named; i++, pair += 2) {
        if (!pair[0].isString()) throw runtimeError("invalid call");
        const std::string& name = asString(pair[0])->chars;
        int index = name.empty() ? count - 1 : -1;  // a lambda after the call is the last parameter
        for (int p = 0; index < 0 && p < count; p++)
            if (method->params[static_cast<size_t>(p)] == name) index = p;
        if (index < 0) throw runtimeError(method->name + "() has no parameter named '" + name + "'");
        if (arranged[static_cast<size_t>(index)].kind != ValueKind::Undefined)
            throw runtimeError("argument '" + method->params[static_cast<size_t>(index)] + "' of " + method->name +
                               "() is given more than once");
        arranged[static_cast<size_t>(index)] = pair[1];
        if (index + 1 > given) given = index + 1;
    }

    if (stackEnd_ - first < given) throw runtimeError("stack overflow");
    for (int i = 0; i < given; i++) {
        const Value& value = arranged[static_cast<size_t>(i)];
        first[i] = value.kind == ValueKind::Undefined ? Value::null() : value;
    }
    sp_ = first + given;
    return callValue(callee, given);
}

// A call whose positional arguments were gathered into a list because some were written
// `*list`. The stack holds the callee, that list, then a (name, value) pair for each named
// argument. The list is laid out as ordinary arguments and the call proceeds as usual.
bool VM::callSpread(int named) {
    Value* slot = sp_ - 2 * named - 1;
    ObjList* list = asList(*slot);
    const int count = static_cast<int>(list->items.size());
    if (stackEnd_ - sp_ < count + 1) throw runtimeError("stack overflow");

    std::copy_backward(slot + 1, sp_, sp_ + count - 1);
    std::copy(list->items.begin(), list->items.end(), slot);
    sp_ += count - 1;
    if (named > 0) return callNamed(count, named);
    return callValue(sp_[-count - 1], count);
}

const NativeMethod* VM::findNativeMethod(Value receiver, const ObjString* name) const {
    BuiltinType type;
    if (receiver.isNumber()) {
        type = BuiltinType::Number;
    } else if (!receiver.isObj()) {
        return nullptr;
    } else {
        switch (receiver.as.obj->type) {
            case ObjType::String: type = BuiltinType::String; break;
            case ObjType::List: type = BuiltinType::List; break;
            case ObjType::Map: type = BuiltinType::Map; break;
            case ObjType::Range: type = BuiltinType::Range; break;
            case ObjType::Bytes: type = BuiltinType::Bytes; break;
            default: return nullptr;
        }
    }
    const auto& table = methods_[static_cast<int>(type)];
    auto it = table.find(name);
    return it == table.end() ? nullptr : &it->second;
}

// Calls method `name` on the receiver sitting below `argc` arguments. Same result as callValue.
bool VM::invoke(ObjString* name, int argc, bool trailing) {
    Value receiver = sp_[-argc - 1];
    guardRead(receiver);

    if (receiver.isObj()) {
        if (receiver.as.obj->type == ObjType::Instance) {
            ObjInstance* instance = asInstance(receiver);
            // A call goes to the method of that name. A field may share the name: reading it
            // gives the field, calling it runs the method.
            if (const Value* method = instance->cls->methods.find(name)) {
                if (trailing) callTrailing(asClosure(*method), argc);
                else callClosure(asClosure(*method), argc);
                return true;
            }
            if (const Value* field = instance->fields.find(name)) {
                sp_[-argc - 1] = *field;
                return trailing ? callValueTrailing(*field, argc) : callValue(*field, argc);
            }
            if (instance->cls->hasAccessors) {
                // obj.property(args): call whatever the getter of the property returns.
                if (const Value* getter = instance->cls->getters.find(name)) {
                    Value callee = callWithReceiver(receiver, asClosure(*getter));
                    sp_[-argc - 1] = callee;
                    return trailing ? callValueTrailing(callee, argc) : callValue(callee, argc);
                }
            }
            if (const Value* extension = findExtension(receiver, name)) {
                if (trailing) callTrailing(asClosure(*extension), argc);
                else callClosure(asClosure(*extension), argc);
                return true;
            }
            if (const int done = scopeFunction(name, argc)) return done == 2;
            throw runtimeError("undefined method '" + name->chars + "' on " + instance->cls->name->chars);
        }

        if (receiver.as.obj->type == ObjType::Class) {
            ObjClass* cls = asClass(receiver);
            if (cls->staticInit != nullptr || (cls->super != nullptr && cls->super->staticInit != nullptr)) ensureClassInit(cls);
            if (const Value* member = cls->findStatic(name)) {
                sp_[-argc - 1] = *member;
                return trailing ? callValueTrailing(*member, argc) : callValue(*member, argc);
            }
            throw runtimeError("undefined static method '" + name->chars + "' on class " + cls->name->chars);
        }
    }

    const NativeMethod* method = findNativeMethod(receiver, name);
    if (method == nullptr) {
        if (const Value* extension = findExtension(receiver, name)) {
            if (trailing) callTrailing(asClosure(*extension), argc);
            else callClosure(asClosure(*extension), argc);
            return true;
        }
        if (const int done = scopeFunction(name, argc)) return done == 2;
        throw runtimeError("undefined method '" + name->chars + "' on a value of type " + typeName(receiver));
    }
    checkArity(method->name, method->minArgs, method->maxArgs, argc);
    if (pendingEnums_ > 0) rejectPendingNames(sp_ - argc, argc);
    if (coreId_ != 0) {
        if (method->mutates && receiver.isObj()) guardWrite(receiver.as.obj);
        guardNative(sp_ - argc, argc);
    }
    nativeBase_ = sp_ - argc - 1;
    Value result = method->fn(*this, receiver, argc, sp_ - argc);
    sp_ -= argc + 1;
    *sp_++ = result;
    return false;
}

Value VM::tailCallOn(Value receiver, Value function) {
    if (!function.isObjType(ObjType::Closure)) throw runtimeError("expected a function written in Astra, got a value of type " + typeName(function));
    // The built-in's own call goes: the receiver takes the place it was called at, and the
    // function is called there with nothing else.
    TempRoot keep(*this, function);
    sp_ = nativeBase_;
    *sp_++ = receiver;
    callClosure(asClosure(function), 0);
    return Value::undefined();
}

// x.let { }, x.also { }, x.apply { } and x.run { } on any value that has no method of its own
// by that name. `let` and `also` hand the value to the block as its argument; `apply` and
// `run` run the block on the value. `also` and `apply` give the value back, the others the
// block's result. (Written with a lambda, let and also are expanded in place by the compiler
// and never get here.)
//
// The block is called as part of the program, not from inside this function, so that a flow
// can be set aside in it. Returns 0 if `name` is not one of these, 1 if the result is on the
// stack, and 2 if a call has been set going.
int VM::scopeFunction(ObjString* name, int argc) {
    if (argc != 1) return 0;
    const std::string& n = name->chars;
    const bool onReceiver = n == "apply" || n == "run";
    if (!onReceiver && n != "let" && n != "also") return 0;

    const Value receiver = sp_[-2], block = sp_[-1];
    if (n == "let") {
        // block(receiver), in the place of the two.
        sp_[-2] = block;
        sp_[-1] = receiver;
        return callValue(block, 1) ? 2 : 1;
    }
    if (!block.isObjType(ObjType::Closure)) {
        // Something that is not a function written in Astra is simply called.
        Value result = n == "also" ? call(block, 1, &receiver) : call(block, 0, nullptr);
        if (n != "run") result = receiver;
        sp_ -= 2;
        *sp_++ = result;
        return 1;
    }
    if (n == "run") {
        // The block runs on the receiver, and its result takes the receiver's place.
        sp_--;
        callClosure(asClosure(block), 0);
        return 2;
    }
    if (n == "apply") {
        // The same, on a second copy of the receiver, and the block's result is dropped.
        sp_[-1] = receiver;
        callClosure(asClosure(block), 0);
    } else {
        // also: block(receiver), and the result is dropped.
        if (stackEnd_ - sp_ < 1) throw runtimeError("stack overflow");
        *sp_++ = receiver;
        callClosure(asClosure(block), 1);
    }
    frames_[static_cast<size_t>(frameCount_ - 1)].dropResult = true;
    return 2;
}

// What == means: numbers and strings by value; lists, maps and ranges by what they hold,
// compared the same way all the way down; an object by its own `==` operator if its class
// has one, and otherwise only to itself. A structure nested deeper than a comparison can
// follow (one that contains itself, for instance) is equal only to itself from there on.
bool VM::equals(Value a, Value b, int depth) {
    if (!a.isObj() || !b.isObj()) return valuesEqual(a, b);
    if (a.as.obj == b.as.obj) return true;

    // In a parallel section another core's item is not looked into. Telling two objects apart
    // by which they are needs no looking, so only the cases below that do look ask.
    if (a.isObjType(ObjType::Instance)) {
        const Value* method = asInstance(a)->cls->methods.find(names.opEq);
        if (method == nullptr) {
            // Two objects of one data class are equal when their header fields are.
            ObjInstance* x = asInstance(a);
            if (!x->cls->isData || !b.isObjType(ObjType::Instance) || asInstance(b)->cls != x->cls) return false;
            if (depth >= kMaxEqualsDepth) return false;
            guardRead(a.as.obj);
            guardRead(b.as.obj);
            ObjInstance* y = asInstance(b);
            for (ObjString* part : x->cls->components) {
                const Value* left = x->fields.find(part);
                const Value* right = y->fields.find(part);
                if (!equals(left != nullptr ? *left : Value::null(), right != nullptr ? *right : Value::null(), depth + 1))
                    return false;
            }
            return true;
        }
        TempRoot bound(*this, Value::object(alloc<ObjBoundMethod>(a, asClosure(*method))));
        return call(bound.get(), 1, &b).truthy();
    }
    if (a.as.obj->type != b.as.obj->type) return false;
    if (depth >= kMaxEqualsDepth) return false;
    guardRead(a.as.obj);
    guardRead(b.as.obj);

    switch (a.as.obj->type) {
        case ObjType::Bytes:
            return asBytes(a)->data == asBytes(b)->data;
        case ObjType::List: {
            const std::vector<Value>& x = asList(a)->items;
            const std::vector<Value>& y = asList(b)->items;
            if (x.size() != y.size()) return false;
            // An element's own == may change either list, so the sizes are looked at again.
            for (size_t i = 0; i < x.size() && i < y.size(); i++)
                if (!equals(x[i], y[i], depth + 1)) return false;
            return x.size() == y.size();
        }
        case ObjType::Map: {
            const ValueMap& x = asMap(a)->map;
            const ValueMap& y = asMap(b)->map;
            if (x.size() != y.size()) return false;
            for (size_t i = 0; i < x.entries().size(); i++) {
                if (!x.entries()[i].live) continue;
                const Value key = x.entries()[i].key, value = x.entries()[i].value;
                const Value* other = y.find(key);
                if (other == nullptr) return false;
                const Value otherValue = *other;
                if (!equals(value, otherValue, depth + 1)) return false;
            }
            return true;
        }
        case ObjType::Range: {
            const ObjRange* x = asRange(a);
            const ObjRange* y = asRange(b);
            return x->start == y->start && x->end == y->end && x->step == y->step && x->inclusive == y->inclusive;
        }
        default:
            return valuesEqual(a, b);
    }
}

// Calls an overloaded operator; the receiver and its operands are already on the stack in
// call layout, so this always pushes a frame.
void VM::invokeOperator(Value receiver, ObjString* name, int argc) {
    ObjInstance* instance = asInstance(receiver);
    const Value* method = instance->cls->methods.find(name);
    if (method == nullptr) {
        const std::string symbol = name == names.opNeg ? "-" : name == names.opBitNot ? "~" : name->chars;
        throw runtimeError(instance->cls->name->chars + " does not define operator '" + symbol + "'");
    }
    callClosure(asClosure(*method), argc);
}

// ---- Properties and indexing ----

Value VM::getProperty(Value obj, ObjString* name) {
    if (obj.isObj()) {
        guardRead(obj.as.obj);
        switch (obj.as.obj->type) {
            case ObjType::Instance: {
                ObjInstance* instance = asInstance(obj);
                if (const Value* field = instance->fields.find(name)) return *field;
                if (const Value* method = instance->cls->methods.find(name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*method)));
                if (instance->cls->setters.find(name) != nullptr)
                    throw runtimeError("property '" + name->chars + "' of " + instance->cls->name->chars +
                                       " cannot be read: it has no getter");
                if (const Value* extension = findExtension(obj, name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*extension)));
                throw runtimeError("undefined property '" + name->chars + "' on " + instance->cls->name->chars);
            }
            case ObjType::Class: {
                ObjClass* cls = asClass(obj);
                if (cls->staticInit != nullptr || (cls->super != nullptr && cls->super->staticInit != nullptr)) ensureClassInit(cls);
                if (const Value* member = cls->findStatic(name)) return *member;
                for (const ObjClass* k = cls; k != nullptr; k = k->super) {
                    if (const Value* getter = k->staticGetters.find(name)) return call(*getter, 0, nullptr);
                    if (k->staticSetters.find(name) != nullptr)
                        throw runtimeError("static property '" + name->chars + "' of " + k->name->chars +
                                           " cannot be read: it has no getter");
                }
                if (name == names.className) return Value::object(cls->name);  // `name` is left free for a member
                throw runtimeError("undefined static member '" + name->chars + "' on class " + cls->name->chars);
            }
            case ObjType::String:
                if (name == names.length || name == names.size) return Value::integer(static_cast<int64_t>(asString(obj)->length));
                if (name == names.indices)
                    return Value::object(alloc<ObjRange>(int64_t{0}, static_cast<int64_t>(asString(obj)->length), int64_t{1}, false));
                if (name == names.lastIndex) return Value::integer(static_cast<int64_t>(asString(obj)->length) - 1);
                if (name == names.code && asString(obj)->length > 0) {
                    // The code point of a character (the first, if there are more).
                    std::string_view first = asString(obj)->character(0);
                    uint32_t cp = static_cast<unsigned char>(first[0]);
                    if (first.size() == 2) cp &= 0x1F;
                    else if (first.size() == 3) cp &= 0x0F;
                    else if (first.size() >= 4) cp &= 0x07;
                    for (size_t i = 1; i < first.size(); i++) cp = (cp << 6) | (static_cast<unsigned char>(first[i]) & 0x3F);
                    return Value::integer(cp);
                }
                break;
            case ObjType::List:
                if (name == names.length || name == names.size) return Value::integer(static_cast<int64_t>(asList(obj)->items.size()));
                if (name == names.indices)
                    return Value::object(alloc<ObjRange>(int64_t{0}, static_cast<int64_t>(asList(obj)->items.size()), int64_t{1}, false));
                if (name == names.lastIndex) return Value::integer(static_cast<int64_t>(asList(obj)->items.size()) - 1);
                break;
            case ObjType::Map:
                if (name == names.length || name == names.size) return Value::integer(static_cast<int64_t>(asMap(obj)->map.size()));
                if (name == names.keys) return mapKeys(asMap(obj));
                if (name == names.values) return mapValues(asMap(obj));
                if (name == names.entries) return mapEntries(asMap(obj));
                break;
            case ObjType::Range: {
                ObjRange* range = asRange(obj);
                if (name == names.start) return Value::integer(range->start);
                if (name == names.end) return Value::integer(range->end);
                if (name == names.step) return Value::integer(range->step);
                if (name == names.length || name == names.size) return Value::integer(range->count());
                // The numbers it starts and finishes on, when it holds any.
                if (name == names.first && range->count() > 0) return Value::integer(range->start);
                if (name == names.last && range->count() > 0)
                    return Value::integer(range->start + (range->count() - 1) * range->step);
                break;
            }
            case ObjType::Bytes:
                if (name == names.length || name == names.size) return Value::integer(static_cast<int64_t>(asBytes(obj)->data.size()));
                break;
            default:
                break;
        }
    }

    if (obj.isInt()) {
        if (name == names.absoluteValue) return Value::integer(obj.as.i < 0 ? -obj.as.i : obj.as.i);
        if (name == names.sign) return Value::integer((obj.as.i > 0) - (obj.as.i < 0));
    } else if (obj.isFloat()) {
        if (name == names.absoluteValue) return Value::number(std::fabs(obj.as.d));
        if (name == names.sign) return Value::number(obj.as.d > 0 ? 1.0 : obj.as.d < 0 ? -1.0 : obj.as.d);
    }

    if (const NativeMethod* method = findNativeMethod(obj, name))
        return Value::object(alloc<ObjBoundNative>(obj, method));
    if (const Value* extension = findExtension(obj, name))
        return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*extension)));
    throw runtimeError("undefined property '" + name->chars + "' on a value of type " + typeName(obj));
}

// Called when initializer `init` returns: the class it belongs to may have declared fields
// with no initial value, which the initializer was obliged to assign.
void VM::checkRequiredFields(ObjInstance* instance, const ObjClosure* init) {
    // The owner is the highest class in the chain using this initializer; subclasses that
    // merely inherit it cannot have required fields of their own.
    const ObjClass* owner = nullptr;
    for (const ObjClass* k = instance->cls; k != nullptr; k = k->super)
        if (k->init == init) owner = k;
    if (owner == nullptr) return;

    for (ObjString* name : owner->ownRequired)
        if (instance->fields.find(name) == nullptr)
            throw runtimeError(owner->name->chars + ".init() must set field '" + name->chars + "'");
}

// Enforces the field declarations of a class that has any (its own or inherited): only
// declared fields exist, a val can be assigned only once, and a typed field only takes
// values of its type.
void VM::checkFieldAssignment(ObjInstance* instance, ObjString* stored, Value value) {
    ObjClass* cls = instance->cls;
    // A property with accessors keeps its value under "field:" and its name; messages say the name.
    struct Shown {
        std::string chars;
    };
    const Shown shown{stored->chars.rfind("field:", 0) == 0 ? stored->chars.substr(6) : stored->chars};
    const Shown* const name = &shown;
    const Value* index = cls->declaredIndex.find(stored);
    if (index == nullptr)
        throw runtimeError(cls->name->chars + " has no field '" + name->chars +
                           "'; fields must be declared in the class with val or var");
    size_t at = static_cast<size_t>(index->as.i);

    if (cls->declared[at].isVal && instance->fields.find(stored) != nullptr &&
        !(cls->declared[at].overrides && instance->constructing))
        throw runtimeError("cannot assign to val field '" + name->chars + "' of " + cls->name->chars);

    uint8_t type = cls->declared[at].type;
    if (type == 0) return;
    if (value.isNull() && (type & kTypeNullable) != 0) return;

    if ((type & kTypeMask) == kTypeInstance && cls->declared[at].typeClass == nullptr) {
        // First use of a class-typed field: find out which class its type names.
        TempRoot keepAlive(*this, value);
        Value resolved = call(Value::object(cls->declared[at].typeThunk), 0, nullptr);
        if (!resolved.isObjType(ObjType::Class))
            throw runtimeError("a type must be a class or a built-in type, not a value of type " + typeName(resolved));
        // (In a parallel section several cores may find this out at once: they all find the same class.)
        cls->declared[at].typeClass = asClass(resolved);
    }

    const ObjClass* typeClass = cls->declared[at].typeClass;
    if (!matchesType(value, type, typeClass))
        throw runtimeError("cannot assign a value of type " + typeName(value) + " to field '" + name->chars + "' of " +
                           cls->name->chars + ", which is declared as " + declaredTypeText(type, typeClass));
}

// `super.name(...)`: the method of the superclass, or failing that the one that comes with an
// interface the object's class implements (a class with only interfaces has no superclass).
const Value* VM::superMethod(Value super, Value self, const ObjString* name) const {
    if (super.isObjType(ObjType::Class)) {
        if (const Value* method = asClass(super)->methods.find(name)) return method;
    }
    if (self.isObjType(ObjType::Instance)) {
        for (const ObjClass* implemented : asInstance(self)->cls->interfaces)
            if (const Value* method = implemented->methods.find(name)) return method;
    }
    return nullptr;
}

// Gives an enum entry its name and its place in the list. A getter the enum wrote for `name`
// or `ordinal` stands in for the field; the real ones are kept under names of their own.
void VM::nameEntry(ObjInstance* entry, ObjString* name, int ordinal) {
    const ObjClass* cls = entry->cls;
    entry->fields.set(names.entryName, Value::object(name));
    entry->fields.set(names.entryOrdinal, Value::integer(ordinal));
    if (cls->getters.find(names.name) == nullptr) entry->fields.set(names.name, Value::object(name));
    if (cls->getters.find(names.ordinal) == nullptr) entry->fields.set(names.ordinal, Value::integer(ordinal));
}

Value VM::makeEntry(Value key, Value value) {
    if (mapEntryClass_ == nullptr) {
        ObjList* pair = alloc<ObjList>();
        pair->items = {key, value};
        return Value::object(pair);
    }
    ObjInstance* entry = alloc<ObjInstance>(mapEntryClass_);
    entry->fields.set(names.key, key);
    entry->fields.set(names.value, value);
    account(96);
    return Value::object(entry);
}

Value VM::mapEntries(ObjMap* map) {
    ObjList* list = alloc<ObjList>();
    TempRoot keep(*this, Value::object(list));
    for (const ValueMap::Entry& entry : map->map.entries())
        if (entry.live) list->items.push_back(makeEntry(entry.key, entry.value));
    account(list->items.size() * sizeof(Value));
    return Value::object(list);
}

Value VM::mapKeys(ObjMap* map) {
    ObjList* list = alloc<ObjList>();
    for (const ValueMap::Entry& entry : map->map.entries())
        if (entry.live) list->items.push_back(entry.key);
    account(list->items.size() * sizeof(Value));
    return Value::object(list);
}

Value VM::mapValues(ObjMap* map) {
    ObjList* list = alloc<ObjList>();
    for (const ValueMap::Entry& entry : map->map.entries())
        if (entry.live) list->items.push_back(entry.value);
    account(list->items.size() * sizeof(Value));
    return Value::object(list);
}

// Runs what works out a class's static values, if it has not run yet, superclasses first.
void VM::ensureClassInit(ObjClass* cls) {
    if (cls->super != nullptr && cls->super->staticInit != nullptr) ensureClassInit(cls->super);
    ObjClosure* init = cls->staticInit;
    if (init == nullptr) return;
    // Working out a class's static values changes the class, which every core sees.
    if (cls->core != coreId_)
        throw notOwned("class " + cls->name->chars + " is used for the first time here, and its static values are worked out on " +
                       "first use; use it once before the section");
    cls->staticInit = nullptr;  // before running it: what it works out may be asked for meanwhile
    call(Value::object(init), 0, nullptr);
}

// A value for a declared static field has to fit the field's type.
void VM::checkStaticType(ObjClass* owner, FieldDecl& field, Value value) {
    const uint8_t type = field.type;
    if (type == 0) return;
    if (value.isNull() && (type & kTypeNullable) != 0) return;

    if ((type & kTypeMask) == kTypeInstance && field.typeClass == nullptr) {
        TempRoot keepAlive(*this, value);
        Value resolved = call(Value::object(field.typeThunk), 0, nullptr);
        if (!resolved.isObjType(ObjType::Class))
            throw runtimeError("a type must be a class or a built-in type, not a value of type " + typeName(resolved));
        field.typeClass = asClass(resolved);
    }
    if (!matchesType(value, type, field.typeClass))
        throw runtimeError("cannot assign a value of type " + typeName(value) + " to static field '" + field.name->chars +
                           "' of " + owner->name->chars + ", which is declared as " + declaredTypeText(type, field.typeClass));
}

// `Class.name = value`. A declared static field keeps its rules (a val stays as it is, a type
// is enforced), a static property runs its setter, and any other name is simply stored.
void VM::assignStatic(ObjClass* cls, ObjString* name, Value value) {
    guardWrite(cls);
    if (cls->staticInit != nullptr || (cls->super != nullptr && cls->super->staticInit != nullptr)) {
        TempRoot keepAlive(*this, value);
        ensureClassInit(cls);
    }
    for (ObjClass* k = cls; k != nullptr; k = k->super) {
        if (const Value* index = k->staticIndex.find(name)) {
            FieldDecl& field = k->staticDeclared[static_cast<size_t>(index->as.i)];
            if (field.isVal) throw runtimeError("cannot assign to static val '" + name->chars + "' of " + k->name->chars);
            checkStaticType(k, field, value);
            k->statics.set(name, value);
            return;
        }
        if (const Value* setter = k->staticSetters.find(name)) {
            call(*setter, 1, &value);
            return;
        }
        if (k->staticGetters.find(name) != nullptr)
            throw runtimeError("static property '" + name->chars + "' of " + k->name->chars +
                               " cannot be assigned: it has no setter");
    }
    cls->statics.set(name, value);
}

void VM::setProperty(Value obj, ObjString* name, Value value) {
    if (obj.isObjType(ObjType::Instance)) {
        ObjInstance* instance = asInstance(obj);
        guardWrite(instance);
        if (instance->cls->isEnum && (name == names.name || name == names.ordinal))
            throw runtimeError("cannot assign to '" + name->chars + "' of an entry of enum " + instance->cls->name->chars);
        checkFieldAssignment(instance, name, value);  // every field is declared (R24)
        instance->fields.set(name, value);
    } else if (obj.isObjType(ObjType::Class)) {
        assignStatic(asClass(obj), name, value);
    } else {
        throw runtimeError("cannot set property '" + name->chars + "' on a value of type " + typeName(obj));
    }
}

namespace {

// Resolves a possibly negative index against a length; returns false if out of range.
bool resolveIndex(int64_t index, size_t length, size_t& out) {
    int64_t n = static_cast<int64_t>(length);
    if (index < 0) index += n;
    if (index < 0 || index >= n) return false;
    out = static_cast<size_t>(index);
    return true;
}

}  // namespace

Value VM::getIndex(Value obj, Value index) {
    if (obj.isObj()) {
        guardRead(obj.as.obj);
        switch (obj.as.obj->type) {
            case ObjType::List: {
                const std::vector<Value>& items = asList(obj)->items;
                size_t i;
                if (!resolveIndex(toInt(index, "list index"), items.size(), i))
                    throw runtimeError("list index " + std::to_string(index.as.i) + " is out of range (length " +
                                       std::to_string(items.size()) + ")");
                return items[i];
            }
            case ObjType::String: {
                // One character, wherever it is: positions count characters, not bytes.
                const ObjString* text = asString(obj);
                size_t i;
                if (!resolveIndex(toInt(index, "string index"), text->length, i))
                    throw runtimeError("string index " + std::to_string(index.as.i) + " is out of range (length " +
                                       std::to_string(text->length) + ")");
                return str(std::string(text->character(i)));
            }
            case ObjType::Map: {
                const Value* found = asMap(obj)->map.find(index);
                return found != nullptr ? *found : Value::null();
            }
            case ObjType::Bytes: {
                const std::vector<uint8_t>& data = asBytes(obj)->data;
                size_t i;
                if (!resolveIndex(toInt(index, "bytes index"), data.size(), i))
                    throw runtimeError("bytes index " + std::to_string(index.as.i) + " is out of range (size " +
                                       std::to_string(data.size()) + ")");
                return Value::integer(data[i]);
            }
            default:
                break;
        }
    }
    throw runtimeError("cannot index a value of type " + typeName(obj));
}

void VM::setIndex(Value obj, Value index, Value value) {
    if (obj.isObjType(ObjType::List)) {
        guardWrite(obj.as.obj);
        std::vector<Value>& items = asList(obj)->items;
        size_t i;
        if (!resolveIndex(toInt(index, "list index"), items.size(), i))
            throw runtimeError("list index " + std::to_string(index.as.i) + " is out of range (length " +
                               std::to_string(items.size()) + ")");
        items[i] = value;
    } else if (obj.isObjType(ObjType::Map)) {
        guardWrite(obj.as.obj);
        asMap(obj)->map.set(index, value);
        account(64);
    } else if (obj.isObjType(ObjType::Bytes)) {
        std::vector<uint8_t>& data = asBytes(obj)->data;
        size_t i;
        if (!resolveIndex(toInt(index, "bytes index"), data.size(), i))
            throw runtimeError("bytes index " + std::to_string(index.as.i) + " is out of range (size " +
                               std::to_string(data.size()) + ")");
        if (!value.isInt() || value.as.i < 0 || value.as.i > 255)
            throw runtimeError("a byte is a whole number from 0 to 255, not " + stringify(value));
        // In a parallel section over the parts of a buffer, a core writes its own part of it.
        if (obj.as.obj->core != coreId_ && !(obj.as.obj == bandBuffer_ && i >= bandFrom_ && i < bandTo_)) {
            if (obj.as.obj == bandBuffer_)
                throw notOwned("position " + std::to_string(i) + " is outside this core's part of the buffer, which is " +
                               std::to_string(bandFrom_) + " to " + std::to_string(bandTo_ - 1));
            throw notOwned(obj.as.obj);
        }
        data[i] = static_cast<uint8_t>(value.as.i);
    } else {
        throw runtimeError("cannot assign to an index of a value of type " + typeName(obj));
    }
}

// ---- Operators (the cases the interpreter loop does not handle inline) ----

bool VM::binarySlow(Op op) {
    Value a = sp_[-2], b = sp_[-1];
    guardRead(a);
    guardRead(b);
    ObjString* symbol = opNames_[static_cast<int>(op)];

    if (a.isObjType(ObjType::Instance)) {
        invokeOperator(a, symbol, 1);
        return true;
    }

    if (op == Op::Add) {
        if (a.isString() || b.isString()) {
            std::string joined;
            if (a.isString() && b.isString()) {
                joined.reserve(asString(a)->chars.size() + asString(b)->chars.size());
                joined += asString(a)->chars;
                joined += asString(b)->chars;
            } else {
                stringifyInto(joined, a, false, 0);
                stringifyInto(joined, b, false, 0);
            }
            sp_[-2] = str(std::move(joined));
            sp_--;
            return false;
        }
        if (a.isObjType(ObjType::Bytes) && b.isObjType(ObjType::Bytes)) {
            ObjBytes* joined = alloc<ObjBytes>();
            joined->data = asBytes(a)->data;
            joined->data.insert(joined->data.end(), asBytes(b)->data.begin(), asBytes(b)->data.end());
            account(joined->data.size());
            sp_[-2] = Value::object(joined);
            sp_--;
            return false;
        }
        if (a.isObjType(ObjType::List) && b.isObjType(ObjType::List)) {
            ObjList* joined = alloc<ObjList>();
            const std::vector<Value>& left = asList(a)->items;
            const std::vector<Value>& right = asList(b)->items;
            joined->items.reserve(left.size() + right.size());
            joined->items.insert(joined->items.end(), left.begin(), left.end());
            joined->items.insert(joined->items.end(), right.begin(), right.end());
            account(joined->items.size() * sizeof(Value));
            sp_[-2] = Value::object(joined);
            sp_--;
            return false;
        }
    }

    throw runtimeError("unsupported operand types for '" + symbol->chars + "': " + typeName(a) + " and " + typeName(b));
}

bool VM::compareSlow(Op op) {
    Value a = sp_[-2], b = sp_[-1];
    guardRead(a);
    guardRead(b);
    ObjString* symbol = opNames_[static_cast<int>(op)];

    if (a.isObjType(ObjType::Instance)) {
        invokeOperator(a, symbol, 1);
        return true;
    }

    if (a.isString() && b.isString()) {
        int order = asString(a)->chars.compare(asString(b)->chars);
        bool result = op == Op::Lt ? order < 0 : op == Op::Le ? order <= 0 : op == Op::Gt ? order > 0 : order >= 0;
        sp_[-2] = Value::boolean(result);
        sp_--;
        return false;
    }

    throw runtimeError("cannot compare " + typeName(a) + " and " + typeName(b) + " with '" + symbol->chars + "'");
}

// ---- Declared variable types ----

bool VM::matchesType(Value v, uint8_t type, const ObjClass* cls) const {
    if (v.isNull()) return (type & kTypeNullable) != 0;

    switch (type & kTypeMask) {
        case kTypeInt: return v.isInt();
        case kTypeFloat: return v.isFloat();
        case kTypeNumber: return v.isNumber();
        case kTypeBool: return v.isBool();
        case kTypeString: return v.isString();
        case kTypeList: return v.isObjType(ObjType::List);
        case kTypeMap: return v.isObjType(ObjType::Map);
        case kTypeRange: return v.isObjType(ObjType::Range);
        case kTypeBytes: return v.isObjType(ObjType::Bytes);
        case kTypeFunction:
            return v.isObjType(ObjType::Closure) || v.isObjType(ObjType::Native) ||
                   v.isObjType(ObjType::BoundMethod) || v.isObjType(ObjType::BoundNative);
        case kTypeInstance: return v.isObjType(ObjType::Instance) && asInstance(v)->cls->isSubclassOf(cls);
        default: return false;
    }
}

// For a class type, takes the class off the stack (the compiler pushed it above the value).
ObjClass* VM::popTypeClass(uint8_t type) {
    if ((type & kTypeMask) != kTypeInstance) return nullptr;
    Value cls = *--sp_;
    if (!cls.isObjType(ObjType::Class))
        throw runtimeError("a type must be a class or a built-in type, not a value of type " + typeName(cls));
    return asClass(cls);
}

std::string VM::declaredTypeText(uint8_t type, const ObjClass* cls) const {
    return typeText(type, cls != nullptr ? cls->name->chars : std::string());
}

AstraThrow VM::typeMismatch(const std::string& variable, uint8_t type, const ObjClass* cls, Value v) {
    return runtimeError("cannot assign a value of type " + typeName(v) + " to '" + variable + "', which is declared as " +
                        declaredTypeText(type, cls));
}

// a << b and a >> b on ints. Returns true if an overloaded operator was called instead.
bool VM::shift(Op op) {
    Value a = sp_[-2], b = sp_[-1];
    if (!a.isInt() || !b.isInt()) return binarySlow(op);

    int64_t count = b.as.i;
    if (count < 0) throw runtimeError("cannot shift by a negative amount (" + std::to_string(count) + ")");

    int64_t result;
    if (op == Op::Shl) {
        // Bits shifted past the top are lost; a count of 64 or more leaves nothing.
        result = count >= 64 ? 0 : static_cast<int64_t>(static_cast<uint64_t>(a.as.i) << count);
    } else {
        // The sign is kept: negative numbers stay negative, ending at -1.
        if (count >= 64) result = a.as.i < 0 ? -1 : 0;
        else result = a.as.i < 0 ? ~(~a.as.i >> count) : a.as.i >> count;
    }
    sp_[-2] = Value::integer(result);
    sp_--;
    return false;
}

// a & b, a | b and a ^ b on ints. Returns true if an overloaded operator was called instead.
bool VM::bitwise(Op op) {
    Value a = sp_[-2], b = sp_[-1];
    if (!a.isInt() || !b.isInt()) return binarySlow(op);

    int64_t result = op == Op::BitAnd ? a.as.i & b.as.i : op == Op::BitOr ? a.as.i | b.as.i : a.as.i ^ b.as.i;
    sp_[-2] = Value::integer(result);
    sp_--;
    return false;
}

// ---- Iteration ----

Value VM::makeIterator(Value iterable) {
    guardRead(iterable);
    if (iterable.isObj()) {
        switch (iterable.as.obj->type) {
            case ObjType::List:
            case ObjType::Map:
            case ObjType::String:
            case ObjType::Bytes:
                return Value::object(alloc<ObjIterator>(iterable));
            case ObjType::Range: {
                ObjIterator* it = alloc<ObjIterator>(iterable);
                it->nextValue = asRange(iterable)->start;
                it->remaining = asRange(iterable)->count();
                return Value::object(it);
            }
            case ObjType::Instance: {
                // An object can be looped over if it has an iterator() that gives something
                // that can: a list, say, of what it holds.
                const Value* method = asInstance(iterable)->cls->methods.find(names.iterator);
                if (method == nullptr) break;
                TempRoot bound(*this, Value::object(alloc<ObjBoundMethod>(iterable, asClosure(*method))));
                Value inner = call(bound.get(), 0, nullptr);
                if (inner.isObjType(ObjType::Instance))
                    throw runtimeError("iterator() of " + typeName(iterable) + " has to give a list or another built-in collection");
                TempRoot keep(*this, inner);
                return makeIterator(inner);
            }
            default:
                break;
        }
    }
    throw runtimeError("a value of type " + typeName(iterable) + " is not iterable");
}

bool VM::iteratorNext(ObjIterator* it, Value& out) {
    Value source = it->source;
    switch (source.as.obj->type) {
        case ObjType::List: {
            const std::vector<Value>& items = asList(source)->items;
            if (it->index >= items.size()) return false;
            out = items[it->index++];
            return true;
        }
        case ObjType::Bytes: {
            const std::vector<uint8_t>& data = asBytes(source)->data;
            if (it->index >= data.size()) return false;
            out = Value::integer(data[it->index++]);
            return true;
        }
        case ObjType::Range: {
            if (it->remaining <= 0) return false;
            it->remaining--;
            out = Value::integer(it->nextValue);
            it->nextValue = static_cast<int64_t>(static_cast<uint64_t>(it->nextValue) + static_cast<uint64_t>(asRange(source)->step));
            return true;
        }
        case ObjType::String: {
            // Yields whole UTF-8 characters: a lead byte plus its continuation bytes.
            const std::string& chars = asString(source)->chars;
            if (it->index >= chars.size()) return false;
            size_t begin = it->index++;
            while (it->index < chars.size() && (static_cast<unsigned char>(chars[it->index]) & 0xC0) == 0x80) it->index++;
            out = str(chars.substr(begin, it->index - begin));
            return true;
        }
        case ObjType::Map: {
            const std::vector<ValueMap::Entry>& entries = asMap(source)->map.entries();
            while (it->index < entries.size()) {
                const ValueMap::Entry& entry = entries[it->index++];
                if (entry.live) {
                    out = entry.key;
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

// ---- Upvalues ----

ObjUpvalue* VM::captureUpvalue(Value* slot) {
    // The open list is sorted by stack slot, highest first.
    ObjUpvalue* previous = nullptr;
    ObjUpvalue* upvalue = openUpvalues_;
    while (upvalue != nullptr && upvalue->location > slot) {
        previous = upvalue;
        upvalue = upvalue->nextOpen;
    }
    if (upvalue != nullptr && upvalue->location == slot) return upvalue;

    ObjUpvalue* created = alloc<ObjUpvalue>(slot);
    created->owner = context_->fiber;
    created->nextOpen = upvalue;
    if (previous == nullptr) openUpvalues_ = created;
    else previous->nextOpen = created;
    return created;
}

// Moves every captured variable at or above `last` off the stack and into its upvalue.
void VM::closeUpvalues(Value* last) {
    while (openUpvalues_ != nullptr && openUpvalues_->location >= last) {
        ObjUpvalue* upvalue = openUpvalues_;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        upvalue->owner = nullptr;
        openUpvalues_ = upvalue->nextOpen;
        upvalue->nextOpen = nullptr;
    }
}

// ---- Interpreter loop ----

void VM::run(int baseFrame) {
    CallFrame* frame = nullptr;
    const uint8_t* ip = nullptr;
    const uint8_t* code = nullptr;
    Value* base = nullptr;
    const Value* consts = nullptr;

#define LOAD_FRAME()                                         \
    do {                                                     \
        frame = &frames_[static_cast<size_t>(frameCount_ - 1)]; \
        ip = frame->ip;                                      \
        base = frame->base;                                  \
        code = frame->closure->proto->code.data();           \
        consts = frame->closure->proto->constants.data();    \
    } while (0)

// Must precede anything that can push a frame or re-enter the VM, so the frame can be
// resumed and shows the right line in a stack trace.
#define SAVE_IP() (frame->ip = ip)

// Before a member is reached by a name written after an object (or a class, for a static
// one): the code may not be allowed to use it, or to assign to it. `bits` is what about the
// name matters here: 1 that some class restricts its use, 2 that some class restricts
// assigning to it. A name no class restricts costs the one test. Of the rest, three cases
// are passed without a call: the instructions the compiler writes where it knows the code
// may (GetOwn and the like, for which `checked` is false); an object whose class restricts
// no name like this one (ObjClass::accessBits); and code written in the class of the object
// it works on, when that class's own code may use all its objects have.
#define CHECK_ACCESS(target, bits, write, checked)                                               \
    do {                                                                                         \
        if (name->restricted & (bits)) [[unlikely]] {                                            \
            const Value target_ = (target);                                                      \
            const FunctionProto* const proto_ = frame->closure->proto;                           \
            const bool own_ = !(checked) ||                                                      \
                              (target_.isObjType(ObjType::Instance) &&                           \
                               ((asInstance(target_)->cls->accessBits >> (name->hash & 63) & 1) == 0 || \
                                (asInstance(target_)->cls->number == proto_->ownClass &&         \
                                 (asInstance(target_)->cls->accessAllOwn ||                      \
                                  (asInstance(target_)->cls->accessSharedOwn && (name->restricted & 4) == 0))))); \
            if (!own_ && accessDenied(target_, name, proto_, (write))) {                         \
                SAVE_IP();                                                                       \
                throw accessError(target_, name, proto_);                                        \
            }                                                                                    \
        }                                                                                        \
    } while (0)

#define READ_U8() (*ip++)
#define READ_U16() (ip += 2, static_cast<int>(ip[-2] | (ip[-1] << 8)))
#define READ_I32()                                                                                        \
    (ip += 4, static_cast<int32_t>(static_cast<uint32_t>(ip[-4]) | (static_cast<uint32_t>(ip[-3]) << 8) | \
                                   (static_cast<uint32_t>(ip[-2]) << 16) | (static_cast<uint32_t>(ip[-1]) << 24)))

// Where the collector may run, and where a thread gives way to another.
#define GC_SAFE_POINT()                                                                    \
    do {                                                                                   \
        if (bytesAllocated_ > nextGC_ || attention_.load(std::memory_order_relaxed)) safePoint(); \
    } while (0)

#define ARITHMETIC(op, intExpr, floatExpr)                                \
    {                                                                     \
        Value b = sp_[-1], a = sp_[-2];                                   \
        if (a.isInt() && b.isInt()) {                                     \
            uint64_t x = static_cast<uint64_t>(a.as.i);                   \
            uint64_t y = static_cast<uint64_t>(b.as.i);                   \
            sp_[-2] = Value::integer(static_cast<int64_t>(intExpr));      \
            sp_--;                                                        \
        } else if (a.isNumber() && b.isNumber()) {                        \
            double x = a.toDouble(), y = b.toDouble();                    \
            sp_[-2] = Value::number(floatExpr);                           \
            sp_--;                                                        \
        } else {                                                          \
            SAVE_IP();                                                    \
            if (binarySlow(op)) LOAD_FRAME();                             \
        }                                                                 \
        break;                                                            \
    }

#define COMPARISON(op, cmp)                                               \
    {                                                                     \
        Value b = sp_[-1], a = sp_[-2];                                   \
        if (a.isInt() && b.isInt()) {                                     \
            sp_[-2] = Value::boolean(a.as.i cmp b.as.i);                  \
            sp_--;                                                        \
        } else if (a.isNumber() && b.isNumber()) {                        \
            sp_[-2] = Value::boolean(a.toDouble() cmp b.toDouble());      \
            sp_--;                                                        \
        } else {                                                          \
            SAVE_IP();                                                    \
            if (compareSlow(op)) LOAD_FRAME();                            \
        }                                                                 \
        break;                                                            \
    }

    // An error the program may catch: the innermost handler that takes it gets it, or, with
    // none in this run, it goes on to whoever called. Returns false in that case.
    auto handled = [&](AstraThrow& thrown) {
        frames_[static_cast<size_t>(frameCount_ - 1)].ip = ip;
        if (!thrown.hasTrace && handlers_.empty()) {
            thrown.trace = captureTrace();
            thrown.hasTrace = true;
        }
        if (tryHandle(thrown.value, baseFrame)) return true;
        unwindTo(baseFrame);
        return false;
    };

    for (;;) {
        try {
            LOAD_FRAME();

            for (;;) {
                switch (static_cast<Op>(*ip++)) {
                    case Op::Const:
                        *sp_++ = consts[READ_U16()];
                        break;
                    case Op::Null:
                        *sp_++ = Value::null();
                        break;
                    case Op::True:
                        *sp_++ = Value::boolean(true);
                        break;
                    case Op::False:
                        *sp_++ = Value::boolean(false);
                        break;
                    case Op::Pop:
                        sp_--;
                        break;
                    case Op::Dup:
                        sp_[0] = sp_[-1];
                        sp_++;
                        break;
                    case Op::Dup2:
                        sp_[0] = sp_[-2];
                        sp_[1] = sp_[-1];
                        sp_ += 2;
                        break;

                    case Op::GetLocal:
                        *sp_++ = base[READ_U16()];
                        break;
                    case Op::SetLocal:
                        base[READ_U16()] = sp_[-1];
                        break;

                    case Op::GetGlobal: {
                        int index = READ_U16();
                        Value value = globals_[static_cast<size_t>(index)];
                        if (value.kind == ValueKind::Undefined)
                            throw runtimeError("undefined variable '" + program_->globalNames[static_cast<size_t>(index)] + "'");
                        *sp_++ = value;
                        break;
                    }
                    case Op::SetGlobal: {
                        int index = READ_U16();
                        if (coreId_ != 0) throw notOwned("the variable '" + program_->globalNames[static_cast<size_t>(index)] + "' is shared");
                        Value& slot = globals_[static_cast<size_t>(index)];
                        if (slot.kind == ValueKind::Undefined)
                            throw runtimeError("undefined variable '" + program_->globalNames[static_cast<size_t>(index)] + "'");
                        const TypeConstraint& declared = globalTypes_[static_cast<size_t>(index)];
                        if (declared.type != 0 && !matchesType(sp_[-1], declared.type, declared.cls))
                            throw typeMismatch(program_->globalNames[static_cast<size_t>(index)], declared.type,
                                               declared.cls, sp_[-1]);
                        slot = sp_[-1];
                        break;
                    }
                    case Op::DefineGlobal: {
                        size_t index = static_cast<size_t>(READ_U16());
                        if (coreId_ != 0) throw notOwned("'" + program_->globalNames[index] + "' is declared for the whole program");
                        globals_[index] = *--sp_;
                        globalTypes_[index] = TypeConstraint{};
                        break;
                    }

                    case Op::GetImplicit: {
                        ObjString* name = asString(consts[READ_U16()]);
                        size_t index = static_cast<size_t>(READ_U16());
                        Value receiver = implicitReceiver(frame, base, name);
                        if (receiver.kind == ValueKind::Undefined) {
                            Value value = globals_[index];
                            if (value.kind == ValueKind::Undefined) throw undefinedName(frame, base, name, index);
                            *sp_++ = value;
                            break;
                        }
                        if (!receiver.isObjType(ObjType::Instance)) {
                            // In an extension function on a built-in type: a member of the value.
                            SAVE_IP();
                            *sp_++ = getProperty(receiver, name);
                            break;
                        }
                        ObjInstance* instance = asInstance(receiver);
                        if (const Value* field = instance->fields.find(name)) {
                            *sp_++ = *field;
                            break;
                        }
                        if (instance->cls->hasAccessors) {
                            if (const Value* getter = instance->cls->getters.find(name)) {
                                *sp_++ = receiver;
                                SAVE_IP();
                                callClosure(asClosure(*getter), 0);
                                LOAD_FRAME();
                                break;
                            }
                        }
                        SAVE_IP();
                        *sp_++ = getProperty(receiver, name);  // a method, bound to the object
                        break;
                    }
                    case Op::TryImplicit: {
                        ObjString* name = asString(consts[READ_U16()]);
                        int target = READ_I32();
                        Value receiver = implicitReceiver(frame, base, name);
                        if (receiver.kind == ValueKind::Undefined) break;
                        ip = code + target;
                        if (!receiver.isObjType(ObjType::Instance)) {
                            SAVE_IP();
                            *sp_++ = getProperty(receiver, name);
                            break;
                        }
                        ObjInstance* instance = asInstance(receiver);
                        if (const Value* field = instance->fields.find(name)) {
                            *sp_++ = *field;
                            break;
                        }
                        if (instance->cls->hasAccessors) {
                            if (const Value* getter = instance->cls->getters.find(name)) {
                                *sp_++ = receiver;
                                SAVE_IP();
                                callClosure(asClosure(*getter), 0);
                                LOAD_FRAME();
                                break;
                            }
                        }
                        SAVE_IP();
                        *sp_++ = getProperty(receiver, name);
                        break;
                    }
                    case Op::InvokeImplicit: {
                        ObjString* name = asString(consts[READ_U16()]);
                        size_t index = static_cast<size_t>(READ_U16());
                        int argc = READ_U8();
                        GC_SAFE_POINT();
                        SAVE_IP();
                        Value receiver = implicitReceiver(frame, base, name);
                        if (receiver.kind == ValueKind::Undefined) {
                            Value callee = globals_[index];
                            if (callee.kind == ValueKind::Undefined) throw undefinedName(frame, base, name, index);
                            sp_[-argc - 1] = callee;
                            if (callValue(callee, argc)) LOAD_FRAME();
                        } else {
                            sp_[-argc - 1] = receiver;
                            if (invoke(name, argc)) LOAD_FRAME();
                        }
                        break;
                    }
                    case Op::SetImplicit: {
                        ObjString* name = asString(consts[READ_U16()]);
                        size_t index = static_cast<size_t>(READ_U16());
                        Value receiver = implicitReceiver(frame, base, name);
                        if (receiver.kind == ValueKind::Undefined) {
                            if (coreId_ != 0) throw notOwned("the variable '" + program_->globalNames[index] + "' is shared");
                            Value& slot = globals_[index];
                            if (slot.kind == ValueKind::Undefined) throw undefinedName(frame, base, name, index);
                            const TypeConstraint& declared = globalTypes_[index];
                            if (declared.type != 0 && !matchesType(sp_[-1], declared.type, declared.cls))
                                throw typeMismatch(program_->globalNames[index], declared.type, declared.cls, sp_[-1]);
                            slot = sp_[-1];
                            break;
                        }
                        CHECK_ACCESS(receiver, 2, true, true);  // found, but its setter may be kept back
                        // Lay the object under the value, as an assignment to obj.name has them.
                        Value value = sp_[-1];
                        sp_[-1] = receiver;
                        *sp_++ = value;
                        SAVE_IP();
                        if (!receiver.isObjType(ObjType::Instance)) {
                            setProperty(receiver, name, value);  // reports that it cannot be set
                            break;
                        }
                        ObjClass* cls = asInstance(receiver)->cls;
                        if (cls->hasAccessors) {
                            if (const Value* setter = cls->setters.find(name)) {
                                callClosure(asClosure(*setter), 1);
                                LOAD_FRAME();
                                break;
                            }
                            if (cls->getters.find(name) != nullptr)
                                throw runtimeError("property '" + name->chars + "' of " + cls->name->chars +
                                                   " cannot be assigned: it has no setter");
                        }
                        setProperty(receiver, name, value);
                        sp_[-2] = value;
                        sp_--;
                        break;
                    }
                    case Op::DefineTypedGlobal: {
                        size_t index = static_cast<size_t>(READ_U16());
                        uint8_t type = READ_U8();
                        ObjClass* cls = popTypeClass(type);
                        Value value = *--sp_;
                        if (!matchesType(value, type, cls))
                            throw typeMismatch(program_->globalNames[index], type, cls, value);
                        if (coreId_ != 0) throw notOwned("'" + program_->globalNames[index] + "' is declared for the whole program");
                        globals_[index] = value;
                        globalTypes_[index] = TypeConstraint{type, cls};
                        break;
                    }
                    case Op::CheckParam: {
                        ObjString* parameter = asString(consts[READ_U16()]);
                        uint8_t type = READ_U8();
                        ObjClass* cls = popTypeClass(type);
                        if (sp_[-1].kind == ValueKind::EnumName) sp_[-1] = settleEnumName(sp_[-1], cls);
                        if (!matchesType(sp_[-1], type, cls))
                            throw runtimeError("argument '" + parameter->chars + "' of " + frame->closure->proto->name +
                                               "() must be " + declaredTypeText(type, cls) + ", got " + typeName(sp_[-1]));
                        break;
                    }
                    case Op::CheckReturn: {
                        uint8_t type = READ_U8();
                        ObjClass* cls = popTypeClass(type);
                        if (!matchesType(sp_[-1], type, cls))
                            throw runtimeError(frame->closure->proto->name + "() must return " + declaredTypeText(type, cls) +
                                               ", but returned " + typeName(sp_[-1]));
                        break;
                    }
                    case Op::CheckType: {
                        ObjString* variable = asString(consts[READ_U16()]);
                        uint8_t type = READ_U8();
                        ObjClass* cls = popTypeClass(type);
                        if (!matchesType(sp_[-1], type, cls)) throw typeMismatch(variable->chars, type, cls, sp_[-1]);
                        break;
                    }

                    case Op::GetUpvalue:
                        *sp_++ = *frame->closure->upvalues[static_cast<size_t>(READ_U16())]->location;
                        break;
                    case Op::GetUpvalueChecked: {
                        int index = READ_U16();
                        ObjString* name = asString(consts[READ_U16()]);
                        Value value = *frame->closure->upvalues[static_cast<size_t>(index)]->location;
                        if (value.kind == ValueKind::Undefined)
                            throw runtimeError("'" + name->chars + "' is used before it has a value");
                        *sp_++ = value;
                        break;
                    }
                    case Op::Unset:
                        *sp_++ = Value::undefined();
                        break;
                    case Op::SetUpvalue: {
                        ObjUpvalue* upvalue = frame->closure->upvalues[static_cast<size_t>(READ_U16())];
                        // A variable of the code around the section is every core's to read, and nobody's to change.
                        if (upvalue->core != coreId_) throw notOwned("a variable from outside the section is shared");
                        *upvalue->location = sp_[-1];
                        break;
                    }

                    case Op::GetOwn:
                    case Op::GetProp: {
                        ObjString* name = asString(consts[READ_U16()]);
                        Value obj = sp_[-1];
                        CHECK_ACCESS(obj, 1, false, static_cast<Op>(ip[-3]) != Op::GetOwn);
                        if (obj.isObjType(ObjType::Instance)) {
                            ObjInstance* instance = asInstance(obj);
                            guardRead(instance);
                            if (const Value* field = instance->fields.find(name)) {
                                sp_[-1] = *field;
                                break;
                            }
                            if (instance->cls->hasAccessors) {
                                // A property: its getter runs with the object as the receiver.
                                if (const Value* getter = instance->cls->getters.find(name)) {
                                    SAVE_IP();
                                    callClosure(asClosure(*getter), 0);
                                    LOAD_FRAME();
                                    break;
                                }
                            }
                        }
                        sp_[-1] = getProperty(obj, name);
                        break;
                    }
                    case Op::SetOwn:
                    case Op::SetProp: {
                        ObjString* name = asString(consts[READ_U16()]);
                        Value value = sp_[-1];
                        SAVE_IP();
                        CHECK_ACCESS(sp_[-2], 3, true, static_cast<Op>(ip[-3]) != Op::SetOwn);
                        if (sp_[-2].isObjType(ObjType::Instance) && asInstance(sp_[-2])->cls->hasAccessors) {
                            // A property: the object and the value are already laid out as a call
                            // to the setter, which returns the value it was given.
                            ObjClass* cls = asInstance(sp_[-2])->cls;
                            if (const Value* setter = cls->setters.find(name)) {
                                callClosure(asClosure(*setter), 1);
                                LOAD_FRAME();
                                break;
                            }
                            if (cls->getters.find(name) != nullptr)
                                throw runtimeError("property '" + name->chars + "' of " + cls->name->chars +
                                                   " cannot be assigned: it has no setter");
                        }
                        setProperty(sp_[-2], name, value);
                        sp_[-2] = value;
                        sp_--;
                        break;
                    }

                    case Op::GetIndex: {
                        Value index = sp_[-1], obj = sp_[-2];
                        if (obj.isObjType(ObjType::Instance)) {
                            SAVE_IP();
                            invokeOperator(obj, names.opIndex, 1);
                            LOAD_FRAME();
                            break;
                        }
                        sp_[-2] = getIndex(obj, index);
                        sp_--;
                        break;
                    }
                    case Op::SetIndex: {
                        Value value = sp_[-1], index = sp_[-2], obj = sp_[-3];
                        if (obj.isObjType(ObjType::Instance)) {
                            SAVE_IP();
                            invokeOperator(obj, names.opIndexSet, 2);
                            LOAD_FRAME();
                            break;
                        }
                        setIndex(obj, index, value);
                        sp_[-3] = value;
                        sp_ -= 2;
                        break;
                    }

                    case Op::GetSuper: {
                        ObjString* name = asString(consts[READ_U16()]);
                        Value super = *--sp_;
                        CHECK_ACCESS(sp_[-1], 1, false, true);
                        const Value* method = superMethod(super, sp_[-1], name);
                        if (method == nullptr && super.isNull())
                            throw runtimeError("there is no '" + name->chars + "' for super to mean: this class has no superclass, " +
                                               std::string("and its interfaces come with none"));
                        if (method == nullptr && !super.isObjType(ObjType::Class)) throw runtimeError("superclass must be a class");
                        if (method == nullptr) {
                            // super.name: the superclass's property, run on this object, or a
                            // field it declares, which the object simply has.
                            if (const Value* getter = asClass(super)->getters.find(name)) {
                                SAVE_IP();
                                callClosure(asClosure(*getter), 0);
                                LOAD_FRAME();
                                break;
                            }
                            guardRead(sp_[-1]);
                            const Value* field = sp_[-1].isObjType(ObjType::Instance) ? asInstance(sp_[-1])->fields.find(name) : nullptr;
                            if (field != nullptr && asClass(super)->declaredIndex.find(name) != nullptr) {
                                sp_[-1] = *field;
                                break;
                            }
                            throw runtimeError("undefined method or property '" + name->chars + "' in superclass " +
                                               asClass(super)->name->chars);
                        }
                        sp_[-1] = Value::object(alloc<ObjBoundMethod>(sp_[-1], asClosure(*method)));
                        break;
                    }

                    case Op::Add: ARITHMETIC(Op::Add, x + y, x + y)
                    case Op::Sub: ARITHMETIC(Op::Sub, x - y, x - y)
                    case Op::Mul: ARITHMETIC(Op::Mul, x * y, x * y)

                    case Op::Div: {
                        Value b = sp_[-1], a = sp_[-2];
                        if (a.isInt() && b.isInt()) {
                            if (b.as.i == 0) throw runtimeError("division by zero");
                            // INT64_MIN / -1 overflows; negating with wraparound is the same answer.
                            int64_t result = b.as.i == -1 ? static_cast<int64_t>(0 - static_cast<uint64_t>(a.as.i))
                                                          : a.as.i / b.as.i;
                            sp_[-2] = Value::integer(result);
                            sp_--;
                        } else if (a.isNumber() && b.isNumber()) {
                            sp_[-2] = Value::number(a.toDouble() / b.toDouble());
                            sp_--;
                        } else {
                            SAVE_IP();
                            if (binarySlow(Op::Div)) LOAD_FRAME();
                        }
                        break;
                    }
                    case Op::Mod: {
                        Value b = sp_[-1], a = sp_[-2];
                        if (a.isInt() && b.isInt()) {
                            if (b.as.i == 0) throw runtimeError("division by zero");
                            sp_[-2] = Value::integer(b.as.i == -1 ? 0 : a.as.i % b.as.i);
                            sp_--;
                        } else if (a.isNumber() && b.isNumber()) {
                            sp_[-2] = Value::number(std::fmod(a.toDouble(), b.toDouble()));
                            sp_--;
                        } else {
                            SAVE_IP();
                            if (binarySlow(Op::Mod)) LOAD_FRAME();
                        }
                        break;
                    }

                    case Op::Shl:
                    case Op::Shr:
                        SAVE_IP();
                        if (shift(static_cast<Op>(ip[-1]))) LOAD_FRAME();
                        break;

                    case Op::BitAnd:
                    case Op::BitOr:
                    case Op::BitXor:
                        SAVE_IP();
                        if (bitwise(static_cast<Op>(ip[-1]))) LOAD_FRAME();
                        break;

                    case Op::BitNot: {
                        Value a = sp_[-1];
                        if (a.isInt()) {
                            sp_[-1] = Value::integer(~a.as.i);
                        } else if (a.isObjType(ObjType::Instance)) {
                            SAVE_IP();
                            invokeOperator(a, names.opBitNot, 0);
                            LOAD_FRAME();
                        } else {
                            throw runtimeError("cannot apply '~' to a value of type " + typeName(a));
                        }
                        break;
                    }

                    case Op::Neg: {
                        Value a = sp_[-1];
                        if (a.isInt()) {
                            sp_[-1] = Value::integer(static_cast<int64_t>(0 - static_cast<uint64_t>(a.as.i)));
                        } else if (a.isFloat()) {
                            sp_[-1] = Value::number(-a.as.d);
                        } else if (a.isObjType(ObjType::Instance)) {
                            SAVE_IP();
                            invokeOperator(a, names.opNeg, 0);
                            LOAD_FRAME();
                        } else {
                            throw runtimeError("cannot negate a value of type " + typeName(a));
                        }
                        break;
                    }
                    case Op::Not:
                        sp_[-1] = Value::boolean(!sp_[-1].truthy());
                        break;

                    case Op::Eq: {
                        Value b = sp_[-1], a = sp_[-2];
                        if (a.isObjType(ObjType::Instance) && asInstance(a)->cls->methods.find(names.opEq) != nullptr) {
                            SAVE_IP();
                            invokeOperator(a, names.opEq, 1);
                            LOAD_FRAME();
                            break;
                        }
                        if (a.isObj() && b.isObj() && a.as.obj != b.as.obj && a.as.obj->type != ObjType::String) {
                            // Two lists, maps or ranges: equal if what they hold is equal.
                            SAVE_IP();
                            const bool same = equals(a, b);
                            sp_[-2] = Value::boolean(same);
                            sp_--;
                            break;
                        }
                        sp_[-2] = Value::boolean(valuesEqual(a, b));
                        sp_--;
                        break;
                    }
                    case Op::Identical: {
                        // The very same object. Numbers, booleans and strings have no identity
                        // apart from their value.
                        Value b = sp_[-1], a = sp_[-2];
                        const bool objects = a.isObj() && b.isObj() && !a.isString();
                        sp_[-2] = Value::boolean(objects ? a.as.obj == b.as.obj : valuesEqual(a, b));
                        sp_--;
                        break;
                    }
                    case Op::Lt: COMPARISON(Op::Lt, <)
                    case Op::Le: COMPARISON(Op::Le, <=)
                    case Op::Gt: COMPARISON(Op::Gt, >)
                    case Op::Ge: COMPARISON(Op::Ge, >=)

                    case Op::Is: {
                        Value type = *--sp_;
                        if (!type.isObjType(ObjType::Class)) throw runtimeError("right operand of 'is' must be a class");
                        Value value = sp_[-1];
                        sp_[-1] = Value::boolean(value.isObjType(ObjType::Instance) &&
                                                 asInstance(value)->cls->isSubclassOf(asClass(type)));
                        break;
                    }

                    case Op::Jump:
                        ip = code + READ_I32();
                        break;
                    case Op::Loop:
                        ip = code + READ_I32();
                        GC_SAFE_POINT();
                        break;
                    case Op::JumpIfFalse: {
                        int target = READ_I32();
                        if (!(*--sp_).truthy()) ip = code + target;
                        break;
                    }
                    case Op::And: {
                        int target = READ_I32();
                        if (!sp_[-1].truthy()) ip = code + target;
                        else sp_--;
                        break;
                    }
                    case Op::Or: {
                        int target = READ_I32();
                        if (sp_[-1].truthy()) ip = code + target;
                        else sp_--;
                        break;
                    }

                    case Op::JumpIfNull: {
                        int target = READ_I32();
                        if (sp_[-1].isNull()) ip = code + target;
                        break;
                    }
                    case Op::JumpIfNotNull: {
                        int target = READ_I32();
                        if (!sp_[-1].isNull()) ip = code + target;
                        break;
                    }
                    case Op::AssertNotNull:
                        if (sp_[-1].isNull()) throw runtimeError("'!!' was used on a value that is null");
                        break;
                    case Op::Swap:
                        std::swap(sp_[-1], sp_[-2]);
                        break;
                    case Op::PopUnder: {
                        int count = READ_U16();
                        Value* first = sp_ - 1 - count;
                        closeUpvalues(first);
                        *first = sp_[-1];
                        sp_ = first + 1;
                        break;
                    }
                    case Op::IsType: {
                        uint8_t type = READ_U8();
                        ObjClass* cls = popTypeClass(type);
                        sp_[-1] = Value::boolean(matchesType(sp_[-1], type, cls));
                        break;
                    }
                    case Op::Cast: {
                        uint8_t type = READ_U8();
                        ObjClass* cls = popTypeClass(type);
                        if (!matchesType(sp_[-1], type, cls))
                            throw runtimeError("cannot cast a value of type " + typeName(sp_[-1]) + " to " +
                                               declaredTypeText(type, cls));
                        break;
                    }
                    case Op::CastOrNull: {
                        uint8_t type = READ_U8();
                        ObjClass* cls = popTypeClass(type);
                        if (!matchesType(sp_[-1], type, cls)) sp_[-1] = Value::null();
                        break;
                    }
                    case Op::Settle: {
                        Value top = sp_[-1];
                        Value* target = base + READ_U16();
                        for (Value* p = sp_ - 1; p < target; p++) *p = Value::null();
                        *target = top;
                        sp_ = target + 1;
                        break;
                    }

                    case Op::Call: {
                        int argc = READ_U8();
                        GC_SAFE_POINT();
                        SAVE_IP();
                        if (callValue(sp_[-argc - 1], argc)) LOAD_FRAME();
                        break;
                    }
                    case Op::CallNamed: {
                        int positional = READ_U8();
                        int named = READ_U8();
                        GC_SAFE_POINT();
                        SAVE_IP();
                        if (callNamed(positional, named)) LOAD_FRAME();
                        break;
                    }
                    case Op::Spread: {
                        Value added = *--sp_;
                        if (!added.isObjType(ObjType::List))
                            throw runtimeError("'*' in a call needs a list to spread, not a value of type " + typeName(added));
                        ObjList* list = asList(sp_[-1]);
                        guardRead(added);
                        const std::vector<Value>& items = asList(added)->items;
                        list->items.insert(list->items.end(), items.begin(), items.end());
                        account(items.size() * sizeof(Value));
                        break;
                    }
                    case Op::CallSpread: {
                        int named = READ_U8();
                        GC_SAFE_POINT();
                        SAVE_IP();
                        if (callSpread(named)) LOAD_FRAME();
                        break;
                    }
                    case Op::InvokeOwn:
                    case Op::Invoke: {
                        ObjString* name = asString(consts[READ_U16()]);
                        int argc = READ_U8();
                        GC_SAFE_POINT();
                        SAVE_IP();
                        CHECK_ACCESS(sp_[-argc - 1], 1, false, static_cast<Op>(ip[-4]) != Op::InvokeOwn);
                        if (invoke(name, argc)) LOAD_FRAME();
                        break;
                    }
                    case Op::InvokeOwnTrailing:
                    case Op::InvokeTrailing: {
                        ObjString* name = asString(consts[READ_U16()]);
                        int argc = READ_U8();
                        GC_SAFE_POINT();
                        SAVE_IP();
                        CHECK_ACCESS(sp_[-argc - 1], 1, false, static_cast<Op>(ip[-4]) != Op::InvokeOwnTrailing);
                        if (invoke(name, argc, true)) LOAD_FRAME();
                        break;
                    }
                    case Op::SuperInvoke:
                    case Op::SuperInvokeTrailing: {
                        const bool trailing = static_cast<Op>(ip[-1]) == Op::SuperInvokeTrailing;
                        ObjString* name = asString(consts[READ_U16()]);
                        int argc = READ_U8();
                        GC_SAFE_POINT();
                        Value super = *--sp_;
                        CHECK_ACCESS(sp_[-argc - 1], 1, false, true);
                        const Value* method = superMethod(super, sp_[-argc - 1], name);
                        if (method == nullptr && super.isNull())
                            throw runtimeError("there is no '" + name->chars + "' for super to mean: this class has no superclass, " +
                                               std::string("and its interfaces come with none"));
                        if (method == nullptr && !super.isObjType(ObjType::Class)) throw runtimeError("superclass must be a class");
                        if (method == nullptr)
                            throw runtimeError("undefined method '" + name->chars + "' in superclass " +
                                               asClass(super)->name->chars);
                        SAVE_IP();
                        if (trailing) callTrailing(asClosure(*method), argc);
                        else callClosure(asClosure(*method), argc);
                        LOAD_FRAME();
                        break;
                    }
                    case Op::SuperInit: {
                        // From a generated initializer: run the superclass's init, if it has one.
                        int argc = READ_U8();
                        GC_SAFE_POINT();
                        Value super = *--sp_;
                        if (super.isNull()) {
                            // Everything the class lists after its ':' turned out to be an interface.
                            if (argc != 0) throw runtimeError("there is no superclass to pass arguments to: only a class takes them");
                            break;
                        }
                        if (!super.isObjType(ObjType::Class)) throw runtimeError("superclass must be a class");
                        ObjClosure* init = asClass(super)->init;
                        if (init == nullptr) {
                            if (argc != 0)
                                throw runtimeError(asClass(super)->name->chars + "() takes no arguments but got " +
                                                   std::to_string(argc));
                            break;  // nothing to run; the receiver stays as the result
                        }
                        SAVE_IP();
                        callClosure(init, argc);
                        LOAD_FRAME();
                        break;
                    }

                    case Op::Closure: {
                        const FunctionProto* proto = frame->closure->proto->protos[static_cast<size_t>(READ_U16())];
                        ObjClosure* closure = alloc<ObjClosure>(proto, proto->upvalues.size());
                        account(proto->upvalues.size() * sizeof(ObjUpvalue*));
                        for (size_t i = 0; i < proto->upvalues.size(); i++) {
                            const UpvalueDesc& uv = proto->upvalues[i];
                            closure->upvalues[i] = uv.isLocal ? captureUpvalue(base + uv.index)
                                                              : frame->closure->upvalues[uv.index];
                        }
                        *sp_++ = Value::object(closure);
                        break;
                    }
                    case Op::MakeForms: {
                        const FunctionProto* chooser = frame->closure->proto->protos[static_cast<size_t>(READ_U16())];
                        const int count = READ_U8();
                        ObjForms* set = alloc<ObjForms>(chooser);
                        account(static_cast<size_t>(count) * sizeof(ObjClosure*));
                        for (int i = count; i > 0; i--) {
                            if (!sp_[-i].isObjType(ObjType::Closure)) throw runtimeError("invalid set of forms");
                            set->forms.push_back(asClosure(sp_[-i]));
                        }
                        sp_ -= count;
                        *sp_++ = Value::object(set);
                        break;
                    }
                    case Op::MethodForms: {
                        const FunctionProto* chooser = frame->closure->proto->protos[static_cast<size_t>(READ_U16())];
                        const int count = READ_U8();
                        if (!sp_[-count - 1].isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        ObjClass* cls = asClass(sp_[-count - 1]);
                        ObjString* methodName = intern(chooser->name);
                        std::vector<ObjClosure*> forms;
                        for (int i = count; i > 0; i--) {
                            ObjString* name = asString(sp_[-i]);
                            const Value* method = cls->methods.find(name);
                            if (method == nullptr) {
                                // A form under the method's own name: compiled where the name had one form.
                                const Value* plain = cls->methods.find(methodName);
                                if (plain != nullptr && plain->isObjType(ObjType::Closure) && !asClosure(*plain)->proto->chooser &&
                                    asClosure(*plain)->proto->signature.empty())
                                    method = plain;
                            }
                            if (method == nullptr || !method->isObjType(ObjType::Closure)) {
                                if (cls->isAbstract) continue;  // one left to the classes below
                                throw runtimeError("class " + cls->name->chars + " has no form " + name->chars);
                            }
                            forms.push_back(asClosure(*method));
                            cls->methods.set(name, *method);
                        }
                        sp_ -= count;
                        if (forms.size() == 1) {
                            cls->methods.set(methodName, Value::object(forms[0]));
                        } else if (!forms.empty()) {
                            ObjForms* set = alloc<ObjForms>(chooser);
                            set->forms = std::move(forms);
                            account(set->forms.size() * sizeof(ObjClosure*));
                            cls->methods.set(methodName, Value::object(set));
                        }
                        break;
                    }
                    case Op::StaticForms: {
                        const FunctionProto* chooser = frame->closure->proto->protos[static_cast<size_t>(READ_U16())];
                        const int count = READ_U8();
                        if (!sp_[-count - 1].isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        ObjClass* cls = asClass(sp_[-count - 1]);
                        ObjForms* set = alloc<ObjForms>(chooser);
                        cls->statics.set(intern(chooser->name), Value::object(set));
                        for (int i = count; i > 0; i--) {
                            const Value* method = cls->statics.find(asString(sp_[-i]));
                            if (method == nullptr || !method->isObjType(ObjType::Closure)) throw runtimeError("invalid class declaration");
                            set->forms.push_back(asClosure(*method));
                        }
                        account(set->forms.size() * sizeof(ObjClosure*));
                        sp_ -= count;
                        break;
                    }
                    case Op::GetIndexOr: {
                        const Value start = sp_[-1], key = sp_[-2], map = sp_[-3];
                        if (!map.isObjType(ObjType::Map))
                            throw runtimeError("only a map's index takes a default, not that of a value of type " + typeName(map));
                        guardRead(map);
                        const Value* found = asMap(map)->map.find(key);
                        sp_ -= 2;
                        sp_[-1] = found != nullptr ? *found : start;
                        break;
                    }
                    case Op::CloseUpvalue:
                        closeUpvalues(sp_ - 1);
                        sp_--;
                        break;

                    case Op::ReturnInit:
                        // The object is made once its own class's initializer has returned.
                        if (base[0].isObjType(ObjType::Instance) && asInstance(base[0])->cls->init == frame->closure)
                            asInstance(base[0])->constructing = false;
                        // Slot 0 is the new instance: its required fields must be set by now.
                        if (base[0].isObjType(ObjType::Instance) && asInstance(base[0])->cls->anyRequired) {
                            SAVE_IP();
                            checkRequiredFields(asInstance(base[0]), frame->closure);
                        }
                        [[fallthrough]];
                    case Op::Return: {
                        Value result = sp_[-1];
                        closeUpvalues(base);
                        frameCount_--;
                        // Drop the catch blocks of a function that returned from inside a try.
                        while (!handlers_.empty() && handlers_.back().frameIndex >= frameCount_) handlers_.pop_back();
                        sp_ = base;
                        if (!frame->dropResult) *sp_++ = result;
                        if (frameCount_ == baseFrame) return;
                        LOAD_FRAME();
                        break;
                    }

                    case Op::Class: {
                        ObjString* name = asString(consts[READ_U16()]);
                        *sp_++ = Value::object(alloc<ObjClass>(name));
                        account(256);  // rough cost of the method tables
                        break;
                    }
                    case Op::Inherit: {
                        const int count = READ_U8();
                        Value sub = sp_[-1];
                        Value* types = sp_ - 1 - count;
                        if (!sub.isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        ObjClass* subclass = asClass(sub);

                        // Of the supertypes, at most one is a class: the superclass.
                        ObjClass* superclass = nullptr;
                        for (int i = 0; i < count; i++) {
                            if (!types[i].isObjType(ObjType::Class))
                                throw runtimeError("superclass must be a class, got " + typeName(types[i]));
                            ObjClass* type = asClass(types[i]);
                            if (type->isInterface) continue;
                            if (type->isEnum) throw runtimeError("cannot inherit from enum " + type->name->chars);
                            if (subclass->isInterface)
                                throw runtimeError("interface " + subclass->name->chars + " cannot extend class " + type->name->chars);
                            if (superclass != nullptr)
                                throw runtimeError("class " + subclass->name->chars + " can have only one superclass, but " +
                                                   superclass->name->chars + " and " + type->name->chars + " are both classes");
                            superclass = type;
                        }
                        if (superclass != nullptr) {
                            subclass->super = superclass;
                            subclass->methods.addAll(superclass->methods);
                            subclass->init = superclass->init;
                            subclass->declared = superclass->declared;
                            subclass->declaredIndex.addAll(superclass->declaredIndex);
                            subclass->fieldInits = superclass->fieldInits;
                            subclass->anyRequired = superclass->anyRequired;
                            subclass->getters.addAll(superclass->getters);
                            subclass->setters.addAll(superclass->setters);
                            subclass->hasAccessors = superclass->hasAccessors;
                            subclass->accessMarks.addAll(superclass->accessMarks);
                            subclass->accessIds = superclass->accessIds;
                            subclass->accessBits = superclass->accessBits;
                            subclass->interfaces = superclass->interfaces;
                            subclass->required = superclass->required;  // what an abstract class left unwritten
                        }

                        // An interface gives the methods and properties it has a body for, where
                        // the superclass has none of that name, and asks for the rest.
                        auto implement = [&](ObjClass* type) {
                            for (ObjClass* known : subclass->interfaces)
                                if (known == type) return;
                            subclass->interfaces.push_back(type);
                        };
                        for (int i = 0; i < count; i++) {
                            ObjClass* type = asClass(types[i]);
                            if (!type->isInterface) continue;
                            implement(type);
                            for (ObjClass* extended : type->interfaces) implement(extended);
                            subclass->methods.addMissing(type->methods);
                            subclass->getters.addMissing(type->getters);
                            subclass->setters.addMissing(type->setters);
                            subclass->hasAccessors = subclass->hasAccessors || type->hasAccessors;
                            for (uint32_t at = 0; at < type->accessMarks.capacity(); at++) {
                                // What the interface keeps to itself, the class keeps so too.
                                const Table::Entry& entry = type->accessMarks.entries()[at];
                                if (entry.key == nullptr || subclass->accessMarks.find(entry.key) != nullptr) continue;
                                for (bool write : {false, true}) {
                                    const uint32_t half = halfOf(entry.value, write);
                                    if (halfCount(half) == 0) continue;
                                    setAccess(subclass, subclass->accessMarks, entry.key, write,
                                              std::vector<uint16_t>(halfRun(type, half), halfRun(type, half) + halfCount(half)), halfKind(half));
                                }
                            }
                            subclass->required.insert(subclass->required.end(), type->required.begin(), type->required.end());
                        }
                        joinProtected(subclass);
                        types[0] = superclass != nullptr ? Value::object(superclass) : Value::null();
                        sp_ = types + 1;
                        break;
                    }
                    case Op::ClassInit: {
                        Value init = sp_[-1], cls = sp_[-2];
                        if (!cls.isObjType(ObjType::Class) || !init.isObjType(ObjType::Closure) || asClosure(init)->proto->arity != 0)
                            throw runtimeError("invalid class declaration");
                        asClass(cls)->staticInit = asClosure(init);
                        sp_--;
                        break;
                    }
                    case Op::EnsureClassInit:
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        SAVE_IP();
                        ensureClassInit(asClass(sp_[-1]));
                        break;
                    case Op::MissingArg: {
                        ObjString* parameter = asString(consts[READ_U16()]);
                        throw runtimeError(frame->closure->proto->name + "() is missing argument '" + parameter->chars + "'");
                    }
                    case Op::DataField: {
                        ObjString* name = asString(consts[READ_U16()]);
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        asClass(sp_[-1])->isData = true;
                        asClass(sp_[-1])->components.push_back(name);
                        break;
                    }
                    case Op::Component: {
                        const size_t index = READ_U8();
                        Value whole = sp_[-1];
                        guardRead(whole);
                        static const char* const kOrdinals[] = {"first", "second", "third", "fourth", "fifth"};
                        const std::string which = index < 5 ? kOrdinals[index] : std::to_string(index + 1) + "th";
                        if (whole.isObjType(ObjType::List)) {
                            const std::vector<Value>& items = asList(whole)->items;
                            if (index >= items.size())
                                throw runtimeError("cannot take a " + which + " part from a list of " + std::to_string(items.size()));
                            sp_[-1] = items[index];
                        } else if (whole.isObjType(ObjType::Instance) && asInstance(whole)->cls->isData) {
                            const ObjClass* cls = asInstance(whole)->cls;
                            if (index >= cls->components.size())
                                throw runtimeError("cannot take a " + which + " part from " + cls->name->chars + ", which has " +
                                                   std::to_string(cls->components.size()));
                            SAVE_IP();
                            sp_[-1] = getProperty(whole, cls->components[index]);
                        } else {
                            throw runtimeError("cannot take apart a value of type " + typeName(whole) +
                                               ": only a list or a data object has parts");
                        }
                        break;
                    }
                    case Op::MarkInterface:
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        asClass(sp_[-1])->isInterface = true;
                        break;
                    case Op::Extension: {
                        ObjString* name = asString(consts[READ_U16()]);
                        const int type = READ_U8();
                        Value function = *--sp_;
                        if (!function.isObjType(ObjType::Closure)) throw runtimeError("invalid extension function");
                        if (coreId_ != 0) throw notOwned("an extension function is added for the whole program");
                        const int id = type & kTypeMask;
                        if (id == kTypeInstance) {
                            Value target = *--sp_;
                            if (!target.isObjType(ObjType::Class))
                                throw runtimeError("an extension function is added to a class or a built-in type, not to a value of type " +
                                                   typeName(target));
                            addExtensionForm(asClass(target)->extensions, name, function);
                        } else if (id == 0) {
                            addExtensionForm(anyExtensions_, name, function);
                        } else {
                            addExtensionForm(typeExtensions_[id], name, function);
                        }
                        extensionEpoch_++;
                        if (type & kTypeNullable) {
                            // Remembered with the type it is for: a second type declaring the
                            // same name leaves a call on null with no answer (see findExtension).
                            Value owner = id == kTypeInstance ? Value::object(asClass(sp_[0])) : Value::integer(id);
                            const Value* before = nullExtensionOwners_.find(name);
                            const bool same = before == nullptr || (before->isInt() && owner.isInt() ? before->as.i == owner.as.i
                                                                    : before->isObj() && owner.isObj() && before->as.obj == owner.as.obj);
                            nullExtensions_.set(name, same ? function : Value::boolean(false));
                            if (before == nullptr) nullExtensionOwners_.set(name, owner);
                        }
                        hasExtensions_ = true;
                        break;
                    }
                    case Op::MarkAbstract:
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        asClass(sp_[-1])->isAbstract = true;
                        break;
                    case Op::CheckOverride: {
                        ObjString* name = asString(consts[READ_U16()]);
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        const int flags = READ_U8();
                        const bool marked = (flags & 1) != 0, property = (flags & 2) != 0;
                        const ObjClass* cls = asClass(sp_[-1]);

                        // What the class gets from above under this name: a method for a method,
                        // a field or property for a field or property; or something an interface
                        // or an abstract class asks for, which either kind supplies.
                        auto has = [&](const ObjClass* from) {
                            if (!property && from->methods.find(name) == nullptr && name->chars.find('(') != std::string::npos) {
                                // A form of a name with several (D56): the one above may have been
                                // compiled where the name had one form, under the name alone.
                                const Value* plain = from->methods.find(intern(name->chars.substr(0, name->chars.find('('))));
                                return plain != nullptr && plain->isObjType(ObjType::Closure) && !asClosure(*plain)->proto->chooser &&
                                       asClosure(*plain)->proto->signature.empty();
                            }
                            if (!property) return from->methods.find(name) != nullptr;
                            return from->getters.find(name) != nullptr || from->setters.find(name) != nullptr ||
                                   from->declaredIndex.find(name) != nullptr;
                        };
                        bool inherited = cls->super != nullptr && has(cls->super);
                        for (const ObjClass* implemented : cls->interfaces) inherited = inherited || has(implemented);
                        const ObjClass* askedBy = nullptr;
                        for (const auto& [wanted, by] : cls->required)
                            if (wanted == name && by != cls) askedBy = by;

                        if (marked) {
                            // toString and == are there for every object to replace.
                            if (!inherited && askedBy == nullptr && name != names.toString && name != names.opEq)
                                throw runtimeError("'" + name->chars + "' in class " + cls->name->chars +
                                                   " is marked override, but there is nothing of that name for it to replace");
                        } else if (askedBy != nullptr) {
                            throw runtimeError("'" + name->chars + "' in class " + cls->name->chars + " is what " +
                                               (askedBy->isInterface ? "interface " : "abstract class ") + askedBy->name->chars +
                                               " asks for; write override in front of it");
                        } else if (inherited) {
                            throw runtimeError("'" + name->chars + "' in class " + cls->name->chars +
                                               " takes the place of the one it inherits; write override in front of it");
                        }
                        break;
                    }
                    case Op::Require: {
                        ObjString* name = asString(consts[READ_U16()]);
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        asClass(sp_[-1])->required.emplace_back(name, asClass(sp_[-1]));
                        break;
                    }
                    case Op::ClassNumber: {
                        const int number = READ_U16();
                        const int library = READ_U16();
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        asClass(sp_[-1])->number = number;
                        asClass(sp_[-1])->library = library;
                        break;
                    }
                    case Op::MemberAccess: {
                        ObjString* name = asString(consts[READ_U16()]);
                        const int id = READ_U16();
                        const int flags = READ_U8();
                        const int kind = flags & 0xF;
                        const bool write = (flags & kAccessOfSetter) != 0;
                        if (!sp_[-1].isObjType(ObjType::Class) || kind > kAccessAsAbove) throw runtimeError("invalid class declaration");
                        ObjClass* cls = asClass(sp_[-1]);
                        const Value* before = cls->accessMarks.find(name);
                        const bool restricts = kind == kAccessPrivate || kind == kAccessProtected || kind == kAccessInternal;
                        if (restricts && !write && (before == nullptr || halfCount(halfOf(*before, false)) == 0)) {
                            // Restricted here and not above: then there must be nothing above
                            // for it to replace, since what is open stays open.
                            auto has = [&](const ObjClass* from) {
                                return from->methods.find(name) != nullptr || from->declaredIndex.find(name) != nullptr ||
                                       from->getters.find(name) != nullptr || from->setters.find(name) != nullptr;
                            };
                            const ObjClass* open = cls->super != nullptr && has(cls->super) ? cls->super : nullptr;
                            for (const ObjClass* implemented : cls->interfaces)
                                if (open == nullptr && has(implemented)) open = implemented;
                            for (const auto& [wanted, by] : cls->required)
                                if (open == nullptr && wanted == name && by != cls) open = by;
                            if (open != nullptr)
                                throw runtimeError("'" + name->chars + "' in class " + cls->name->chars + " cannot be " +
                                                   (kind == kAccessPrivate ? "private" : kind == kAccessProtected ? "protected" : "internal") +
                                                   ": it is open in " + (open->isInterface ? "interface " : "class ") + open->name->chars);
                        }
                        declareAccess(cls, cls->accessMarks, name, id, kind, write);
                        break;
                    }
                    case Op::StaticAccess: {
                        ObjString* name = asString(consts[READ_U16()]);
                        const int id = READ_U16();
                        const int flags = READ_U8();
                        const int kind = flags & 0xF;
                        if (!sp_[-1].isObjType(ObjType::Class) || id == 0 || kind < kAccessPrivate || kind > kAccessInternal)
                            throw runtimeError("invalid class declaration");
                        declareAccess(asClass(sp_[-1]), asClass(sp_[-1])->staticAccessMarks, name, id, kind, (flags & kAccessOfSetter) != 0);
                        break;
                    }
                    case Op::FinishClass: {
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        ObjClass* cls = asClass(sp_[-1]);
                        // Whether the class's own code may use every member its objects have:
                        // then nothing has to be looked up for it (see CHECK_ACCESS).
                        cls->accessAllOwn = cls->accessSharedOwn = true;
                        for (uint32_t at = 0; at < cls->accessMarks.capacity(); at++) {
                            const Table::Entry& entry = cls->accessMarks.entries()[at];
                            if (entry.key == nullptr) continue;
                            for (bool write : {false, true}) {
                                const uint32_t half = halfOf(entry.value, write);
                                const uint16_t* run = halfRun(cls, half);
                                bool own = halfCount(half) == 0;
                                for (uint32_t i = 0; i < halfCount(half); i++) own = own || run[i] == cls->number || run[i] == cls->library;
                                cls->accessAllOwn = cls->accessAllOwn && own;
                                if (halfKind(half) != kAccessPrivate) cls->accessSharedOwn = cls->accessSharedOwn && own;
                            }
                        }
                        if (cls->isInterface || cls->isAbstract) break;  // left for the classes below
                        for (const auto& [name, interface] : cls->required) {
                            if (cls->methods.find(name) != nullptr || cls->declaredIndex.find(name) != nullptr ||
                                cls->getters.find(name) != nullptr)
                                continue;
                            throw runtimeError("class " + cls->name->chars + " must have '" + name->chars + "', which " +
                                               (interface->isInterface ? "interface " : "abstract class ") + interface->name->chars +
                                               " requires");
                        }
                        cls->required.clear();
                        break;
                    }
                    case Op::Getter:
                    case Op::Setter: {
                        bool isGetter = static_cast<Op>(ip[-1]) == Op::Getter;
                        ObjString* name = asString(consts[READ_U16()]);
                        Value accessor = sp_[-1], cls = sp_[-2];
                        if (!cls.isObjType(ObjType::Class) || !accessor.isObjType(ObjType::Closure) ||
                            asClosure(accessor)->proto->arity != (isGetter ? 0 : 1))
                            throw runtimeError("invalid class declaration");
                        (isGetter ? asClass(cls)->getters : asClass(cls)->setters).set(name, accessor);
                        asClass(cls)->hasAccessors = true;
                        sp_--;
                        break;
                    }
                    case Op::PendingEnum:
                        *sp_++ = Value::enumName(asString(consts[READ_U16()]));
                        pendingEnums_++;
                        break;
                    case Op::EnumNext: {
                        ObjString* name = asString(consts[READ_U16()]);
                        const int ordinal = READ_U16();
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError("invalid enum declaration");
                        asClass(sp_[-1])->nextEntry = name;
                        asClass(sp_[-1])->nextOrdinal = ordinal;
                        break;
                    }
                    case Op::EnumEntry:
                    case Op::EnumSeal: {
                        const bool isEntry = static_cast<Op>(ip[-1]) == Op::EnumEntry;
                        ObjString* name = isEntry ? asString(consts[READ_U16()]) : nullptr;
                        const int ordinal = isEntry ? READ_U16() : 0;
                        Value target = sp_[isEntry ? -2 : -1];
                        if (!target.isObjType(ObjType::Class)) throw runtimeError("invalid enum declaration");
                        ObjClass* cls = asClass(target);
                        cls->isEnum = true;

                        // A static val, so that neither an entry nor the list can be replaced.
                        auto declare = [&](ObjString* member, Value value) {
                            FieldDecl field;
                            field.name = member;
                            field.isVal = true;
                            cls->staticIndex.set(member, Value::integer(static_cast<int64_t>(cls->staticDeclared.size())));
                            cls->staticDeclared.push_back(field);
                            cls->statics.set(member, value);
                        };
                        // The list is kept under a name of its own, and is what `entries` gives
                        // unless the enum has a property of that name in its place.
                        const Value* list = cls->statics.find(names.enumEntries);
                        if (list == nullptr) {
                            Value fresh = Value::object(alloc<ObjList>());
                            cls->statics.set(names.enumEntries, fresh);
                            if (cls->staticGetters.find(names.entries) == nullptr) declare(names.entries, fresh);
                            list = cls->statics.find(names.enumEntries);
                        }
                        if (!isEntry) {
                            cls->enumSealed = true;
                            break;
                        }

                        Value entry = sp_[-1];
                        if (!entry.isObjType(ObjType::Instance) || asInstance(entry)->cls != cls || !list->isObjType(ObjType::List))
                            throw runtimeError("invalid enum declaration");
                        nameEntry(asInstance(entry), name, ordinal);
                        asList(*list)->items.push_back(entry);
                        declare(name, entry);
                        sp_--;
                        break;
                    }
                    case Op::StaticGetter:
                    case Op::StaticSetter: {
                        bool isGetter = static_cast<Op>(ip[-1]) == Op::StaticGetter;
                        ObjString* name = asString(consts[READ_U16()]);
                        Value accessor = sp_[-1], cls = sp_[-2];
                        if (!cls.isObjType(ObjType::Class) || !accessor.isObjType(ObjType::Closure) ||
                            asClosure(accessor)->proto->arity != (isGetter ? 0 : 1))
                            throw runtimeError("invalid class declaration");
                        (isGetter ? asClass(cls)->staticGetters : asClass(cls)->staticSetters).set(name, accessor);
                        sp_--;
                        break;
                    }
                    case Op::StaticField: {
                        ObjString* name = asString(consts[READ_U16()]);
                        int flags = READ_U16();
                        FieldDecl field;
                        field.name = name;
                        field.type = static_cast<uint8_t>(flags & 0xFF);
                        field.isVal = (flags & kFieldVal) != 0;
                        const bool classTyped = (field.type & kTypeMask) == kTypeInstance;
                        Value value = sp_[-1];
                        Value target = sp_[classTyped ? -3 : -2];
                        if (classTyped) {
                            if (!sp_[-2].isObjType(ObjType::Closure)) throw runtimeError("invalid class declaration");
                            field.typeThunk = asClosure(sp_[-2]);
                        }
                        if (!target.isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        ObjClass* cls = asClass(target);
                        SAVE_IP();
                        checkStaticType(cls, field, value);
                        cls->staticIndex.set(name, Value::integer(static_cast<int64_t>(cls->staticDeclared.size())));
                        cls->staticDeclared.push_back(field);
                        cls->statics.set(name, value);
                        sp_ -= classTyped ? 2 : 1;
                        break;
                    }
                    case Op::DeclareField: {
                        ObjString* name = asString(consts[READ_U16()]);
                        int flags = READ_U16();
                        FieldDecl field;
                        field.name = name;
                        field.type = static_cast<uint8_t>(flags & 0xFF);
                        field.isVal = (flags & kFieldVal) != 0;
                        if ((field.type & kTypeMask) == kTypeInstance) {
                            Value thunk = *--sp_;
                            if (!thunk.isObjType(ObjType::Closure)) throw runtimeError("invalid class declaration");
                            field.typeThunk = asClosure(thunk);
                        }
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        ObjClass* cls = asClass(sp_[-1]);
                        if (const Value* inherited = cls->declaredIndex.find(name)) {
                            if ((flags & kFieldOverride) == 0)
                                throw runtimeError("field '" + name->chars + "' of class " + cls->name->chars +
                                                   " is already declared by a superclass; write override to declare it again");
                            field.overrides = true;
                            cls->declared[static_cast<size_t>(inherited->as.i)] = field;
                            break;
                        }
                        cls->declaredIndex.set(name, Value::integer(static_cast<int64_t>(cls->declared.size())));
                        cls->declared.push_back(field);
                        if (flags & kFieldRequired) {
                            cls->ownRequired.push_back(name);
                            cls->anyRequired = true;
                        }
                        break;
                    }
                    case Op::FieldInit: {
                        Value closure = sp_[-1], cls = sp_[-2];
                        if (!cls.isObjType(ObjType::Class) || !closure.isObjType(ObjType::Closure) ||
                            asClosure(closure)->proto->arity != 0)
                            throw runtimeError("invalid class declaration");
                        asClass(cls)->fieldInits.push_back(asClosure(closure));
                        sp_--;
                        break;
                    }
                    case Op::Method:
                    case Op::StaticMethod: {
                        bool isStatic = static_cast<Op>(ip[-1]) == Op::StaticMethod;
                        ObjString* name = asString(consts[READ_U16()]);
                        Value method = sp_[-1], cls = sp_[-2];
                        if (!cls.isObjType(ObjType::Class) || !method.isObjType(ObjType::Closure))
                            throw runtimeError("invalid class declaration");
                        if (isStatic) {
                            asClass(cls)->statics.set(name, method);
                        } else {
                            asClass(cls)->methods.set(name, method);
                            if (name == names.init) asClass(cls)->init = asClosure(method);
                        }
                        sp_--;
                        break;
                    }

                    case Op::BuildList: {
                        int count = READ_U16();
                        ObjList* list = alloc<ObjList>();
                        list->items.assign(sp_ - count, sp_);
                        account(static_cast<size_t>(count) * sizeof(Value));
                        sp_ -= count;
                        *sp_++ = Value::object(list);
                        break;
                    }
                    case Op::BuildMap: {
                        int count = READ_U16();
                        ObjMap* map = alloc<ObjMap>();
                        Value* entry = sp_ - 2 * count;
                        for (int i = 0; i < count; i++, entry += 2) map->map.set(entry[0], entry[1]);
                        account(static_cast<size_t>(count) * 64);
                        sp_ -= 2 * count;
                        *sp_++ = Value::object(map);
                        break;
                    }
                    case Op::BuildString: {
                        int count = READ_U16();
                        SAVE_IP();
                        std::string text;
                        for (int i = 0; i < count; i++) {
                            Value part = sp_[i - count];
                            if (part.isString()) text += asString(part)->chars;
                            else stringifyInto(text, part, false, 0);
                        }
                        sp_ -= count;
                        *sp_++ = str(std::move(text));
                        break;
                    }
                    case Op::Range: {
                        const int flags = READ_U8();
                        int64_t step = 1;
                        if (flags & 4) {
                            Value given = *--sp_;
                            if (!given.isInt() || given.as.i <= 0)
                                throw runtimeError("the step of a range must be a positive int");
                            step = given.as.i;
                        }
                        Value end = sp_[-1], start = sp_[-2];
                        if (!start.isInt() || !end.isInt())
                            throw runtimeError("range bounds must be ints, got " + typeName(start) + " and " + typeName(end));
                        sp_[-2] = Value::object(alloc<ObjRange>(start.as.i, end.as.i, (flags & 2) ? -step : step, (flags & 1) != 0));
                        sp_--;
                        break;
                    }

                    case Op::IterInit:
                        SAVE_IP();
                        sp_[-1] = makeIterator(sp_[-1]);
                        break;
                    case Op::DefaultArg: {
                        int slot = READ_U16();
                        int target = READ_I32();
                        if (base[slot].kind != ValueKind::Undefined) ip = code + target;
                        break;
                    }
                    case Op::IterNext: {
                        int slot = READ_U16();
                        int target = READ_I32();
                        Value iterator = base[slot];
                        if (!iterator.isObjType(ObjType::Iterator)) throw runtimeError("invalid loop state");
                        Value item;
                        if (iteratorNext(asIterator(iterator), item)) *sp_++ = item;
                        else ip = code + target;
                        break;
                    }

                    case Op::TryBegin: {
                        int target = READ_I32();
                        handlers_.push_back(Handler{frameCount_ - 1, sp_, code + target});
                        break;
                    }
                    case Op::TryEnd:
                        if (!handlers_.empty() && handlers_.back().frameIndex == frameCount_ - 1) handlers_.pop_back();
                        break;
                    case Op::Throw: {
                        AstraThrow thrown;
                        thrown.value = *--sp_;
                        throw thrown;
                    }

                    case Op::Import: {
                        size_t index = static_cast<size_t>(READ_U16());
                        if (loaded_[index]) {
                            *sp_++ = Value::null();
                            break;
                        }
                        loaded_[index] = true;
                        const FunctionProto* proto = program_->modules[index];
                        if (proto == nullptr) throw runtimeError("imported module failed to compile");
                        ObjClosure* closure = alloc<ObjClosure>(proto, proto->upvalues.size());
                        *sp_++ = Value::object(closure);
                        SAVE_IP();
                        callClosure(closure, 0);
                        LOAD_FRAME();
                        break;
                    }

                    case Op::Count_:
                        throw runtimeError("invalid instruction");
                }
            }
        } catch (AstraThrow& thrown) {
            if (!handled(thrown)) throw;
        } catch (const std::bad_alloc&) {
            // Asking for memory failed. That is almost always one large request (a list, a
            // string, a buffer), so what is left is plenty for the error itself; if even that
            // fails, the program ends as before.
            AstraThrow thrown = outOfMemory();
            if (!handled(thrown)) throw thrown;
        } catch (const std::length_error&) {
            AstraThrow thrown = outOfMemory();  // more than a list or a string can ever hold
            if (!handled(thrown)) throw thrown;
        }
    }

#undef LOAD_FRAME
#undef SAVE_IP
#undef READ_U8
#undef READ_U16
#undef READ_I32
#undef GC_SAFE_POINT
#undef ARITHMETIC
#undef COMPARISON
}

// ---- Converting values to text ----

std::string VM::stringify(Value v) {
    TempRoot root(*this, v);
    std::string text;
    stringifyInto(text, v, false, 0);
    return text;
}

std::string VM::repr(Value v) {
    TempRoot root(*this, v);
    std::string text;
    stringifyInto(text, v, true, 0);
    return text;
}

// `v` must be reachable by the collector: a toString() method may run and trigger it.
void VM::stringifyInto(std::string& out, Value v, bool quote, int depth) {
    switch (v.kind) {
        case ValueKind::Null: out += "null"; return;
        case ValueKind::Bool: out += v.as.b ? "true" : "false"; return;
        case ValueKind::Int: out += std::to_string(v.as.i); return;
        case ValueKind::Float: out += formatFloat(v.as.d); return;
        case ValueKind::Undefined: out += "undefined"; return;
        case ValueKind::Obj: break;
    }

    guardRead(v.as.obj);
    switch (v.as.obj->type) {
        case ObjType::String:
            if (quote) out += quoteString(asString(v)->chars);
            else out += asString(v)->chars;
            return;

        case ObjType::List: {
            if (depth > 16) {
                out += "[...]";
                return;
            }
            ObjList* list = asList(v);
            out += '[';
            // Indexed and re-checked each time round: a toString() may change the list.
            for (size_t i = 0; i < list->items.size(); i++) {
                if (i > 0) out += ", ";
                Value item = list->items[i];
                stringifyInto(out, item, true, depth + 1);
            }
            out += ']';
            return;
        }

        case ObjType::Map: {
            if (depth > 16) {
                out += "{...}";
                return;
            }
            ObjMap* map = asMap(v);
            out += '{';
            bool first = true;
            for (size_t i = 0; i < map->map.entries().size(); i++) {
                ValueMap::Entry entry = map->map.entries()[i];
                if (!entry.live) continue;
                if (!first) out += ", ";
                first = false;
                stringifyInto(out, entry.key, true, depth + 1);
                out += ": ";
                stringifyInto(out, entry.value, true, depth + 1);
            }
            out += '}';
            return;
        }

        case ObjType::Bytes: {
            const std::vector<uint8_t>& data = asBytes(v)->data;
            out += "Bytes[";
            for (size_t i = 0; i < data.size() && i < 32; i++) {
                if (i > 0) out += ", ";
                out += std::to_string(data[i]);
            }
            if (data.size() > 32) out += ", ... " + std::to_string(data.size() - 32) + " more";
            out += ']';
            return;
        }

        case ObjType::Range: {
            ObjRange* range = asRange(v);
            out += std::to_string(range->start);
            out += range->inclusive ? ".." : "..<";
            out += std::to_string(range->end);
            if (range->step != 1) out += " step " + std::to_string(range->step);
            return;
        }

        case ObjType::Closure:
            out += "<fun " + asClosure(v)->proto->name + ">";
            return;
        case ObjType::Native:
            out += "<native fun " + asNative(v)->name + ">";
            return;
        case ObjType::BoundMethod:
            out += "<fun " + asBoundMethod(v)->method->proto->name + ">";
            return;
        case ObjType::BoundNative:
            out += "<native fun " + asBoundNative(v)->method->name + ">";
            return;
        case ObjType::Class:
            out += "<class " + asClass(v)->name->chars + ">";
            return;

        case ObjType::Instance: {
            ObjInstance* instance = asInstance(v);
            const Value* method = instance->cls->methods.find(names.toString);
            if (method != nullptr && asClosure(*method)->proto->arity == 0) {
                Value bound = Value::object(alloc<ObjBoundMethod>(v, asClosure(*method)));
                Value result = call(bound, 0, nullptr);
                if (result.isString()) {
                    out += asString(result)->chars;
                } else {
                    TempRoot root(*this, result);
                    stringifyInto(out, result, false, depth + 1);
                }
                return;
            }
            if (instance->cls->isData) {
                // A data object prints as its class and header fields: Point(x=1, y=2).
                out += instance->cls->name->chars + "(";
                if (depth > 20) {
                    out += "...)";
                    return;
                }
                bool first = true;
                for (ObjString* part : instance->cls->components) {
                    if (!first) out += ", ";
                    first = false;
                    out += part->chars + "=";
                    const Value* field = instance->fields.find(part);
                    stringifyInto(out, field != nullptr ? *field : Value::null(), true, depth + 1);
                }
                out += ")";
                return;
            }
            if (instance->cls->isEnum) {
                // An enum entry prints as its name.
                const Value* entryName = instance->fields.find(names.entryName);
                if (entryName != nullptr && entryName->isString()) {
                    out += asString(*entryName)->chars;
                    return;
                }
            }
            out += "<" + instance->cls->name->chars + " instance>";
            return;
        }

        case ObjType::Fiber:
            out += "<flow>";
            return;

        case ObjType::Resource:
            out += std::string("<") + static_cast<const ObjResource*>(v.as.obj)->kind + ">";
            return;

        case ObjType::Upvalue:
        case ObjType::Iterator:
            out += "<internal>";
            return;
    }
}

// ---- Garbage collector (mark and sweep) ----

void VM::collectGarbage() {
    liveBytes_ = 0;

    // The context that is running, whose state is in the VM's own members...
    for (Value* slot = stack_; slot < sp_; slot++) markValue(*slot);
    for (int i = 0; i < frameCount_; i++) markObject(frames_[static_cast<size_t>(i)].closure);
    for (ObjUpvalue* upvalue = openUpvalues_; upvalue != nullptr; upvalue = upvalue->nextOpen) markObject(upvalue);
    markObject(context_->fiber);
    // ...and every other one that a thread is in the middle of: the contexts under the
    // running one, and those of the threads that are waiting for the lock or blocked.
    for (const ThreadState* thread : threads_) {
        for (const Context* context = thread->current; context != nullptr; context = context->parent)
            if (context != context_) markContext(*context);
        markValue(thread->function);
        markValue(thread->self);
        if (thread == thread_) continue;  // its temporaries are the VM's, marked below
        for (const Value& root : thread->tempRoots) markValue(root);
        for (const std::vector<Value>* values : thread->tempVectors)
            for (const Value& value : *values) markValue(value);
    }
    for (ObjFiber* fiber : waitingFibers_) markObject(fiber);
    markValue(workerSelf);
    for (const auto& worker : workerObjects) markValue(worker.second);
    for (const Value& task : loopTasks_) markValue(task);
    for (const Timer& timer : timers_) markValue(timer.task);
    for (const Value& global : globals_) markValue(global);
    for (const Table& table : typeExtensions_) markTable(table);
    markTable(nullExtensions_);
    markTable(nullExtensionOwners_);
    markTable(anyExtensions_);
    for (const auto& builtin : builtins_) markValue(builtin.second);
    for (const Value& root : tempRoots) markValue(root);
    for (const Value& root : pinned) markValue(root);
    for (const std::vector<Value>* values : tempVectors)
        for (const Value& value : *values) markValue(value);
    markObject(errorClass_);
    markObject(mapEntryClass_);
    for (const TypeConstraint& declared : globalTypes_) markObject(declared.cls);

    while (!gray_.empty()) {
        Obj* obj = gray_.back();
        gray_.pop_back();
        blacken(obj);
    }

    sweep();
    bytesAllocated_ = liveBytes_;
    nextGC_ = gcStress_ ? 0 : std::max(liveBytes_ * 2, kMinHeap);
}

void VM::markContext(const Context& context) {
    for (const Value* slot = context.stack; slot < context.sp; slot++) markValue(*slot);
    for (int i = 0; i < context.frameCount; i++) markObject(context.frames[i].closure);
    for (ObjUpvalue* upvalue = context.openUpvalues; upvalue != nullptr; upvalue = upvalue->nextOpen) markObject(upvalue);
    markObject(context.fiber);
}

void VM::markObject(Obj* obj) {
    if (obj == nullptr || obj->marked) return;
    obj->marked = true;
    if (obj->type == ObjType::String) {
        // Strings have no references to trace. Interned ones are not counted as collectable heap.
        if (!obj->immortal) liveBytes_ += sizeof(ObjString) + static_cast<ObjString*>(obj)->chars.size();
        return;
    }
    gray_.push_back(obj);
}

void VM::markTable(const Table& table) {
    const Table::Entry* entries = table.entries();
    for (uint32_t i = 0; i < table.capacity(); i++)
        if (entries[i].key != nullptr) markValue(entries[i].value);
    liveBytes_ += table.capacity() * sizeof(Table::Entry);
}

// Marks everything `obj` refers to, and adds its footprint to the live-heap estimate.
void VM::blacken(Obj* obj) {
    switch (obj->type) {
        case ObjType::String:
            break;
        case ObjType::List: {
            ObjList* list = static_cast<ObjList*>(obj);
            liveBytes_ += sizeof(ObjList) + list->items.capacity() * sizeof(Value);
            for (const Value& item : list->items) markValue(item);
            break;
        }
        case ObjType::Map: {
            ObjMap* map = static_cast<ObjMap*>(obj);
            liveBytes_ += sizeof(ObjMap) + map->map.entries().size() * 64;
            for (const ValueMap::Entry& entry : map->map.entries()) {
                if (!entry.live) continue;
                markValue(entry.key);
                markValue(entry.value);
            }
            break;
        }
        case ObjType::Range:
            liveBytes_ += sizeof(ObjRange);
            break;
        case ObjType::Bytes:
            liveBytes_ += sizeof(ObjBytes) + static_cast<ObjBytes*>(obj)->data.capacity();
            break;
        case ObjType::Closure: {
            ObjClosure* closure = static_cast<ObjClosure*>(obj);
            liveBytes_ += sizeof(ObjClosure) + closure->upvalues.size() * sizeof(ObjUpvalue*);
            for (ObjUpvalue* upvalue : closure->upvalues) markObject(upvalue);
            if (closure->proto->chooser) {
                const ObjForms* set = static_cast<const ObjForms*>(closure);
                liveBytes_ += sizeof(ObjForms) - sizeof(ObjClosure) + set->forms.size() * sizeof(ObjClosure*);
                for (ObjClosure* form : set->forms) markObject(form);
            }
            break;
        }
        case ObjType::Upvalue: {
            ObjUpvalue* upvalue = static_cast<ObjUpvalue*>(obj);
            liveBytes_ += sizeof(ObjUpvalue);
            // While it is open its variable lives on a stack. If that is the stack of a flow,
            // the flow is kept, and marks the stack itself if it has been set aside.
            if (upvalue->owner != nullptr) markObject(upvalue->owner);
            else markValue(*upvalue->location);
            break;
        }
        case ObjType::Resource:
            liveBytes_ += sizeof(ObjResource);
            break;
        case ObjType::Fiber: {
            ObjFiber* fiber = static_cast<ObjFiber*>(obj);
            liveBytes_ += sizeof(ObjFiber) + ObjFiber::kStackSize;
            markValue(fiber->entry);
            markValue(fiber->early);
            // One that is running is marked as a context of the thread it is running on;
            // what this object holds of its stack is out of date until it is set aside.
            if (fiber->state == FiberState::Suspended) markContext(fiber->context);
            break;
        }
        case ObjType::Native:
            liveBytes_ += sizeof(ObjNative);
            break;
        case ObjType::Class: {
            ObjClass* cls = static_cast<ObjClass*>(obj);
            liveBytes_ += sizeof(ObjClass);
            markObject(cls->name);
            markObject(cls->super);
            markObject(cls->init);
            markTable(cls->methods);
            markTable(cls->statics);
            markTable(cls->getters);
            markTable(cls->setters);
            for (const FieldDecl& field : cls->declared) {
                markObject(field.typeThunk);
                markObject(field.typeClass);
            }
            for (ObjClosure* fieldInit : cls->fieldInits) markObject(fieldInit);
            markTable(cls->staticGetters);
            markTable(cls->staticSetters);
            markObject(cls->staticInit);
            markTable(cls->extensions);
            markObject(cls->nextEntry);
            for (ObjClass* implemented : cls->interfaces) markObject(implemented);
            for (ObjString* part : cls->components) markObject(part);
            for (const auto& [name, interface] : cls->required) {
                markObject(name);
                markObject(interface);
            }
            for (const FieldDecl& field : cls->staticDeclared) {
                markObject(field.typeThunk);
                markObject(field.typeClass);
            }
            liveBytes_ += cls->staticDeclared.capacity() * sizeof(FieldDecl);
            liveBytes_ += cls->declared.capacity() * sizeof(FieldDecl) + cls->fieldInits.capacity() * sizeof(ObjClosure*);
            break;
        }
        case ObjType::Instance: {
            ObjInstance* instance = static_cast<ObjInstance*>(obj);
            liveBytes_ += sizeof(ObjInstance);
            markObject(instance->cls);
            markTable(instance->fields);
            break;
        }
        case ObjType::BoundMethod: {
            ObjBoundMethod* bound = static_cast<ObjBoundMethod*>(obj);
            liveBytes_ += sizeof(ObjBoundMethod);
            markValue(bound->receiver);
            markObject(bound->method);
            break;
        }
        case ObjType::BoundNative: {
            ObjBoundNative* bound = static_cast<ObjBoundNative*>(obj);
            liveBytes_ += sizeof(ObjBoundNative);
            markValue(bound->receiver);
            break;
        }
        case ObjType::Iterator: {
            ObjIterator* iterator = static_cast<ObjIterator*>(obj);
            liveBytes_ += sizeof(ObjIterator);
            markValue(iterator->source);
            break;
        }
    }
}

void VM::sweep() {
    Obj** link = &objects_;
    while (*link != nullptr) {
        Obj* obj = *link;
        if (obj->marked) {
            obj->marked = false;
            link = &obj->next;
        } else if (obj->immortal) {
            link = &obj->next;
        } else {
            *link = obj->next;
            if (!monitors_.empty()) monitors_.erase(obj);  // it was locked and then forgotten
            delete obj;
        }
    }
}

}  // namespace astra
