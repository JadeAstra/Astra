#include "vm.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>

namespace astra {

VM::VM() : output(&std::cout), input(&std::cin), rng(std::random_device{}()) {
    stack_ = static_cast<Value*>(std::malloc(kStackSize * sizeof(Value)));
    if (stack_ == nullptr) throw std::bad_alloc();
    stackEnd_ = stack_ + kStackSize;
    sp_ = stack_;
    frames_.resize(kMaxFrames);

    if (const char* stress = std::getenv("ASTRA_GC_STRESS"); stress != nullptr && *stress != '\0' && *stress != '0') {
        gcStress_ = true;
        nextGC_ = 0;
    }

    names.init = intern("init");
    names.toString = intern("toString");
    names.message = intern("message");
    names.length = intern("length");
    names.start = intern("start");
    names.end = intern("end");
    names.step = intern("step");
    names.name = intern("name");
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
    Obj* obj = objects_;
    while (obj != nullptr) {
        Obj* next = obj->next;
        delete obj;
        obj = next;
    }
    std::free(stack_);
}

// ---- Strings ----

ObjString* VM::intern(std::string_view s) {
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
    methods_[static_cast<int>(type)][intern(name)] = NativeMethod{name, minArgs, maxArgs, fn};
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

    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { depth++; }
        ~DepthGuard() { depth--; }
    } guard(nativeDepth_);

    int base = frameCount_;
    *sp_++ = callee;
    for (int i = 0; i < argc; i++) *sp_++ = args[i];
    if (callValue(callee, argc)) run(base);
    return *--sp_;
}

// Runs a method that takes no arguments on `receiver` and returns its result.
Value VM::callWithReceiver(Value receiver, ObjClosure* method) {
    if (nativeDepth_ >= kMaxNativeDepth || stackEnd_ - sp_ < 1) throw runtimeError("stack overflow");

    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { depth++; }
        ~DepthGuard() { depth--; }
    } guard(nativeDepth_);

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
        case ObjType::List: return "list";
        case ObjType::Map: return "map";
        case ObjType::Range: return "range";
        case ObjType::Closure:
        case ObjType::Native:
        case ObjType::BoundMethod:
        case ObjType::BoundNative: return "function";
        case ObjType::Class: return "class";
        case ObjType::Instance: return asInstance(v)->cls->name->chars;
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

void VM::callClosure(ObjClosure* closure, int argc) {
    const FunctionProto* proto = closure->proto;
    if (argc != proto->arity && (argc > proto->arity || argc < proto->requiredArgs()))
        checkArity(proto->name, proto->requiredArgs(), proto->arity, argc);

    Value* base = sp_ - argc - 1;
    if (frameCount_ == kMaxFrames || stackEnd_ - base < proto->maxStack) throw runtimeError("stack overflow");

    // Arguments that were left out are marked, and the function fills in their defaults.
    for (int missing = argc; missing < proto->arity; missing++) *sp_++ = Value::undefined();

    CallFrame& frame = frames_[static_cast<size_t>(frameCount_++)];
    frame.closure = closure;
    frame.ip = proto->code.data();
    frame.base = base;
}

// Calls the value sitting below `argc` arguments on the stack. Returns true if a new frame
// was pushed (the caller must then keep interpreting), false if the call already completed
// and left its result on the stack.
bool VM::callValue(Value callee, int argc) {
    if (callee.isObj()) {
        switch (callee.as.obj->type) {
            case ObjType::Closure:
                callClosure(asClosure(callee), argc);
                return true;

            case ObjType::Native: {
                ObjNative* native = asNative(callee);
                checkArity(native->name, native->minArgs, native->maxArgs, argc);
                Value result = native->fn(*this, argc, sp_ - argc);
                sp_ -= argc + 1;
                *sp_++ = result;
                return false;
            }

            case ObjType::Class: {
                ObjClass* cls = asClass(callee);
                ObjInstance* instance = alloc<ObjInstance>(cls);
                account(96);  // rough cost of the field table
                sp_[-argc - 1] = Value::object(instance);
                // Declared fields get their initial values before the initializer runs.
                for (size_t i = 0; i < cls->fieldInits.size(); i++) callWithReceiver(Value::object(instance), cls->fieldInits[i]);
                if (cls->init != nullptr) {
                    callClosure(cls->init, argc);
                    return true;
                }
                if (argc != 0)
                    throw runtimeError(cls->name->chars + "() takes no arguments but got " + std::to_string(argc));
                return false;
            }

            case ObjType::BoundMethod: {
                ObjBoundMethod* bound = asBoundMethod(callee);
                sp_[-argc - 1] = bound->receiver;
                callClosure(bound->method, argc);
                return true;
            }

            case ObjType::BoundNative: {
                ObjBoundNative* bound = asBoundNative(callee);
                const NativeMethod* method = bound->method;
                checkArity(method->name, method->minArgs, method->maxArgs, argc);
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

    // The function whose parameter names the arguments are matched against.
    const FunctionProto* proto = nullptr;
    auto display = [&]() -> std::string {
        return callee.isObjType(ObjType::Class) ? asClass(callee)->name->chars : proto->name;
    };
    if (callee.isObj()) {
        switch (callee.as.obj->type) {
            case ObjType::Closure:
                proto = asClosure(callee)->proto;
                break;
            case ObjType::BoundMethod:
                proto = asBoundMethod(callee)->method->proto;
                break;
            case ObjType::Class: {
                ObjClass* cls = asClass(callee);
                if (cls->init == nullptr) throw runtimeError(cls->name->chars + "() takes no arguments");
                proto = cls->init->proto;
                break;
            }
            case ObjType::Native:
                throw runtimeError("built-in function " + asNative(callee)->name + "() does not take named arguments");
            case ObjType::BoundNative:
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
    if (positional > arity) checkArity(display(), proto->requiredArgs(), arity, positional + named);

    // Scratch space kept between calls. It is emptied back onto the stack before the callee
    // runs, so a named call made from inside the callee can use it again.
    std::vector<Value>& arranged = namedScratch_;
    arranged.resize(static_cast<size_t>(arity));
    for (int i = 0; i < arity; i++) arranged[static_cast<size_t>(i)] = i < positional ? first[i] : Value::undefined();

    const Value* pair = first + positional;
    for (int i = 0; i < named; i++, pair += 2) {
        if (!pair[0].isString()) throw runtimeError("invalid call");
        const ObjString* name = asString(pair[0]);  // interned, like the parameter names

        int index = -1;
        for (int p = 0; p < arity; p++) {
            if (proto->paramNames[static_cast<size_t>(p)] == name) {
                index = p;
                break;
            }
        }
        if (index < 0) throw runtimeError(display() + "() has no parameter named '" + name->chars + "'");
        if (arranged[static_cast<size_t>(index)].kind != ValueKind::Undefined)
            throw runtimeError("argument '" + name->chars + "' of " + display() + "() is given more than once");
        arranged[static_cast<size_t>(index)] = pair[1];
    }

    // Only parameters with a default value may be left out.
    for (int i = 0; i < proto->requiredArgs(); i++)
        if (arranged[static_cast<size_t>(i)].kind == ValueKind::Undefined)
            throw runtimeError(display() + "() is missing argument '" + proto->paramNames[static_cast<size_t>(i)]->chars + "'");

    if (stackEnd_ - first < arity) throw runtimeError("stack overflow");
    for (int i = 0; i < arity; i++) first[i] = arranged[static_cast<size_t>(i)];
    sp_ = first + arity;
    return callValue(callee, arity);
}

const NativeMethod* VM::findNativeMethod(Value receiver, const ObjString* name) const {
    if (!receiver.isObj()) return nullptr;
    BuiltinType type;
    switch (receiver.as.obj->type) {
        case ObjType::String: type = BuiltinType::String; break;
        case ObjType::List: type = BuiltinType::List; break;
        case ObjType::Map: type = BuiltinType::Map; break;
        case ObjType::Range: type = BuiltinType::Range; break;
        default: return nullptr;
    }
    const auto& table = methods_[static_cast<int>(type)];
    auto it = table.find(name);
    return it == table.end() ? nullptr : &it->second;
}

// Calls method `name` on the receiver sitting below `argc` arguments. Same result as callValue.
bool VM::invoke(ObjString* name, int argc) {
    Value receiver = sp_[-argc - 1];

    if (receiver.isObj()) {
        if (receiver.as.obj->type == ObjType::Instance) {
            ObjInstance* instance = asInstance(receiver);
            // A field holding a function shadows a method of the same name.
            if (const Value* field = instance->fields.find(name)) {
                sp_[-argc - 1] = *field;
                return callValue(*field, argc);
            }
            if (const Value* method = instance->cls->methods.find(name)) {
                callClosure(asClosure(*method), argc);
                return true;
            }
            if (instance->cls->hasAccessors) {
                // obj.property(args): call whatever the getter of the property returns.
                if (const Value* getter = instance->cls->getters.find(name)) {
                    Value callee = callWithReceiver(receiver, asClosure(*getter));
                    sp_[-argc - 1] = callee;
                    return callValue(callee, argc);
                }
            }
            throw runtimeError("undefined method '" + name->chars + "' on " + instance->cls->name->chars);
        }

        if (receiver.as.obj->type == ObjType::Class) {
            ObjClass* cls = asClass(receiver);
            if (const Value* member = cls->findStatic(name)) {
                sp_[-argc - 1] = *member;
                return callValue(*member, argc);
            }
            throw runtimeError("undefined static method '" + name->chars + "' on class " + cls->name->chars);
        }
    }

    const NativeMethod* method = findNativeMethod(receiver, name);
    if (method == nullptr)
        throw runtimeError("undefined method '" + name->chars + "' on a value of type " + typeName(receiver));
    checkArity(method->name, method->minArgs, method->maxArgs, argc);
    Value result = method->fn(*this, receiver, argc, sp_ - argc);
    sp_ -= argc + 1;
    *sp_++ = result;
    return false;
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
        switch (obj.as.obj->type) {
            case ObjType::Instance: {
                ObjInstance* instance = asInstance(obj);
                if (const Value* field = instance->fields.find(name)) return *field;
                if (const Value* method = instance->cls->methods.find(name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*method)));
                if (instance->cls->setters.find(name) != nullptr)
                    throw runtimeError("property '" + name->chars + "' of " + instance->cls->name->chars +
                                       " cannot be read: it has no getter");
                throw runtimeError("undefined property '" + name->chars + "' on " + instance->cls->name->chars);
            }
            case ObjType::Class: {
                ObjClass* cls = asClass(obj);
                if (const Value* member = cls->findStatic(name)) return *member;
                if (name == names.name) return Value::object(cls->name);
                throw runtimeError("undefined static member '" + name->chars + "' on class " + cls->name->chars);
            }
            case ObjType::String:
                if (name == names.length) return Value::integer(static_cast<int64_t>(asString(obj)->chars.size()));
                break;
            case ObjType::List:
                if (name == names.length) return Value::integer(static_cast<int64_t>(asList(obj)->items.size()));
                break;
            case ObjType::Map:
                if (name == names.length) return Value::integer(static_cast<int64_t>(asMap(obj)->map.size()));
                break;
            case ObjType::Range: {
                ObjRange* range = asRange(obj);
                if (name == names.start) return Value::integer(range->start);
                if (name == names.end) return Value::integer(range->end);
                if (name == names.step) return Value::integer(range->step);
                if (name == names.length) return Value::integer(range->count());
                break;
            }
            default:
                break;
        }
    }

    if (const NativeMethod* method = findNativeMethod(obj, name))
        return Value::object(alloc<ObjBoundNative>(obj, method));
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
void VM::checkFieldAssignment(ObjInstance* instance, ObjString* name, Value value) {
    ObjClass* cls = instance->cls;
    const Value* index = cls->declaredIndex.find(name);
    if (index == nullptr)
        throw runtimeError(cls->name->chars + " has no field '" + name->chars +
                           "'; fields must be declared in the class with val or var");
    size_t at = static_cast<size_t>(index->as.i);

    if (cls->declared[at].isVal && instance->fields.find(name) != nullptr)
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
        cls->declared[at].typeClass = asClass(resolved);
    }

    const ObjClass* typeClass = cls->declared[at].typeClass;
    if (!matchesType(value, type, typeClass))
        throw runtimeError("cannot assign a value of type " + typeName(value) + " to field '" + name->chars + "' of " +
                           cls->name->chars + ", which is declared as " + declaredTypeText(type, typeClass));
}

void VM::setProperty(Value obj, ObjString* name, Value value) {
    if (obj.isObjType(ObjType::Instance)) {
        ObjInstance* instance = asInstance(obj);
        if (!instance->cls->declared.empty()) checkFieldAssignment(instance, name, value);
        instance->fields.set(name, value);
    } else if (obj.isObjType(ObjType::Class)) {
        asClass(obj)->statics.set(name, value);
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
                const std::string& chars = asString(obj)->chars;
                size_t i;
                if (!resolveIndex(toInt(index, "string index"), chars.size(), i))
                    throw runtimeError("string index " + std::to_string(index.as.i) + " is out of range (length " +
                                       std::to_string(chars.size()) + ")");
                return str(std::string(1, chars[i]));
            }
            case ObjType::Map: {
                const Value* found = asMap(obj)->map.find(index);
                return found != nullptr ? *found : Value::null();
            }
            default:
                break;
        }
    }
    throw runtimeError("cannot index a value of type " + typeName(obj));
}

void VM::setIndex(Value obj, Value index, Value value) {
    if (obj.isObjType(ObjType::List)) {
        std::vector<Value>& items = asList(obj)->items;
        size_t i;
        if (!resolveIndex(toInt(index, "list index"), items.size(), i))
            throw runtimeError("list index " + std::to_string(index.as.i) + " is out of range (length " +
                               std::to_string(items.size()) + ")");
        items[i] = value;
    } else if (obj.isObjType(ObjType::Map)) {
        asMap(obj)->map.set(index, value);
        account(64);
    } else {
        throw runtimeError("cannot assign to an index of a value of type " + typeName(obj));
    }
}

// ---- Operators (the cases the interpreter loop does not handle inline) ----

bool VM::binarySlow(Op op) {
    Value a = sp_[-2], b = sp_[-1];
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
    if (iterable.isObj()) {
        switch (iterable.as.obj->type) {
            case ObjType::List:
            case ObjType::Map:
            case ObjType::String:
                return Value::object(alloc<ObjIterator>(iterable));
            case ObjType::Range: {
                ObjIterator* it = alloc<ObjIterator>(iterable);
                it->nextValue = asRange(iterable)->start;
                it->remaining = asRange(iterable)->count();
                return Value::object(it);
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

#define READ_U8() (*ip++)
#define READ_U16() (ip += 2, static_cast<int>(ip[-2] | (ip[-1] << 8)))
#define READ_I32()                                                                                        \
    (ip += 4, static_cast<int32_t>(static_cast<uint32_t>(ip[-4]) | (static_cast<uint32_t>(ip[-3]) << 8) | \
                                   (static_cast<uint32_t>(ip[-2]) << 16) | (static_cast<uint32_t>(ip[-1]) << 24)))

#define GC_SAFE_POINT()                                \
    do {                                               \
        if (bytesAllocated_ > nextGC_) collectGarbage(); \
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
                        globals_[index] = *--sp_;
                        globalTypes_[index] = TypeConstraint{};
                        break;
                    }
                    case Op::DefineTypedGlobal: {
                        size_t index = static_cast<size_t>(READ_U16());
                        uint8_t type = READ_U8();
                        ObjClass* cls = popTypeClass(type);
                        Value value = *--sp_;
                        if (!matchesType(value, type, cls))
                            throw typeMismatch(program_->globalNames[index], type, cls, value);
                        globals_[index] = value;
                        globalTypes_[index] = TypeConstraint{type, cls};
                        break;
                    }
                    case Op::CheckParam: {
                        ObjString* parameter = asString(consts[READ_U16()]);
                        uint8_t type = READ_U8();
                        ObjClass* cls = popTypeClass(type);
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
                    case Op::SetUpvalue:
                        *frame->closure->upvalues[static_cast<size_t>(READ_U16())]->location = sp_[-1];
                        break;

                    case Op::GetProp: {
                        ObjString* name = asString(consts[READ_U16()]);
                        Value obj = sp_[-1];
                        if (obj.isObjType(ObjType::Instance)) {
                            ObjInstance* instance = asInstance(obj);
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
                    case Op::SetProp: {
                        ObjString* name = asString(consts[READ_U16()]);
                        Value value = sp_[-1];
                        SAVE_IP();
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
                        if (!super.isObjType(ObjType::Class)) throw runtimeError("superclass must be a class");
                        const Value* method = asClass(super)->methods.find(name);
                        if (method == nullptr)
                            throw runtimeError("undefined method '" + name->chars + "' in superclass " +
                                               asClass(super)->name->chars);
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
                        sp_[-2] = Value::boolean(valuesEqual(a, b));
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
                    case Op::Invoke: {
                        ObjString* name = asString(consts[READ_U16()]);
                        int argc = READ_U8();
                        GC_SAFE_POINT();
                        SAVE_IP();
                        if (invoke(name, argc)) LOAD_FRAME();
                        break;
                    }
                    case Op::SuperInvoke: {
                        ObjString* name = asString(consts[READ_U16()]);
                        int argc = READ_U8();
                        GC_SAFE_POINT();
                        Value super = *--sp_;
                        if (!super.isObjType(ObjType::Class)) throw runtimeError("superclass must be a class");
                        const Value* method = asClass(super)->methods.find(name);
                        if (method == nullptr)
                            throw runtimeError("undefined method '" + name->chars + "' in superclass " +
                                               asClass(super)->name->chars);
                        SAVE_IP();
                        callClosure(asClosure(*method), argc);
                        LOAD_FRAME();
                        break;
                    }
                    case Op::SuperInit: {
                        // From a generated initializer: run the superclass's init, if it has one.
                        int argc = READ_U8();
                        GC_SAFE_POINT();
                        Value super = *--sp_;
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
                    case Op::CloseUpvalue:
                        closeUpvalues(sp_ - 1);
                        sp_--;
                        break;

                    case Op::ReturnInit:
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
                        *sp_++ = result;
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
                        Value super = sp_[-2], sub = sp_[-1];
                        if (!super.isObjType(ObjType::Class))
                            throw runtimeError("superclass must be a class, got " + typeName(super));
                        if (!sub.isObjType(ObjType::Class)) throw runtimeError("invalid class declaration");
                        ObjClass* subclass = asClass(sub);
                        subclass->super = asClass(super);
                        subclass->methods.addAll(asClass(super)->methods);
                        subclass->init = asClass(super)->init;
                        subclass->declared = asClass(super)->declared;
                        subclass->declaredIndex.addAll(asClass(super)->declaredIndex);
                        subclass->fieldInits = asClass(super)->fieldInits;
                        subclass->anyRequired = asClass(super)->anyRequired;
                        subclass->getters.addAll(asClass(super)->getters);
                        subclass->setters.addAll(asClass(super)->setters);
                        subclass->hasAccessors = asClass(super)->hasAccessors;
                        sp_--;
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
                        if (cls->declaredIndex.find(name) != nullptr)
                            throw runtimeError("field '" + name->chars + "' of class " + cls->name->chars +
                                               " is already declared by a superclass");
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
                        bool inclusive = READ_U8() != 0;
                        Value end = sp_[-1], start = sp_[-2];
                        if (!start.isInt() || !end.isInt())
                            throw runtimeError("range bounds must be ints, got " + typeName(start) + " and " + typeName(end));
                        sp_[-2] = Value::object(alloc<ObjRange>(start.as.i, end.as.i, int64_t{1}, inclusive));
                        sp_--;
                        break;
                    }

                    case Op::IterInit:
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
            frames_[static_cast<size_t>(frameCount_ - 1)].ip = ip;
            if (!thrown.hasTrace && handlers_.empty()) {
                thrown.trace = captureTrace();
                thrown.hasTrace = true;
            }
            if (!tryHandle(thrown.value, baseFrame)) {
                unwindTo(baseFrame);
                throw;
            }
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

        case ObjType::Range: {
            ObjRange* range = asRange(v);
            out += std::to_string(range->start);
            out += range->inclusive ? "..=" : "..";
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
            out += "<" + instance->cls->name->chars + " instance>";
            return;
        }

        case ObjType::Upvalue:
        case ObjType::Iterator:
            out += "<internal>";
            return;
    }
}

// ---- Garbage collector (mark and sweep) ----

void VM::collectGarbage() {
    liveBytes_ = 0;

    for (Value* slot = stack_; slot < sp_; slot++) markValue(*slot);
    for (int i = 0; i < frameCount_; i++) markObject(frames_[static_cast<size_t>(i)].closure);
    for (ObjUpvalue* upvalue = openUpvalues_; upvalue != nullptr; upvalue = upvalue->nextOpen) markObject(upvalue);
    for (const Value& global : globals_) markValue(global);
    for (const auto& builtin : builtins_) markValue(builtin.second);
    for (const Value& root : tempRoots) markValue(root);
    for (const Value& root : pinned) markValue(root);
    for (const std::vector<Value>* values : tempVectors)
        for (const Value& value : *values) markValue(value);
    markObject(errorClass_);
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
        case ObjType::Closure: {
            ObjClosure* closure = static_cast<ObjClosure*>(obj);
            liveBytes_ += sizeof(ObjClosure) + closure->upvalues.size() * sizeof(ObjUpvalue*);
            for (ObjUpvalue* upvalue : closure->upvalues) markObject(upvalue);
            break;
        }
        case ObjType::Upvalue: {
            ObjUpvalue* upvalue = static_cast<ObjUpvalue*>(obj);
            liveBytes_ += sizeof(ObjUpvalue);
            markValue(*upvalue->location);
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
            delete obj;
        }
    }
}

}  // namespace astra
