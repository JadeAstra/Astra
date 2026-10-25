#include "vm.h"

#include "builtins.h"  // addOverflows and the like (D27)

#include <algorithm>
#include <bit>
#include <cstring>
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
    names.cause = intern("cause");
    names.length = intern("length");
    names.size = intern("size");
    names.indices = intern("indices");
    names.lastIndex = intern("lastIndex");
    names.keys = intern("keys");
    names.itemType = intern("itemType");
    names.keyType = intern("keyType");
    names.valueType = intern("valueType");
    names.values = intern("values");
    names.key = intern("key");
    names.value = intern("value");
    names.code = intern("code");
    names.character = intern("char");
    names.fromEnd = intern("fromEnd");
    names.iterator = intern("iterator");
    names.first = intern("first");
    names.last = intern("last");
    names.absoluteValue = intern("absoluteValue");
    names.sign = intern("sign");
    names.start = intern("start");
    names.end = intern("end");
    names.step = intern("step");
    names.name = intern("name");
    names.ordinal = intern("ordinal");
    names.compareTo = intern("compareTo");
    names.hasNext = intern("hasNext");
    names.next = intern("next");
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
    names.hashCode = intern("hashCode");
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
    opNames_[static_cast<int>(Op::Ushr)] = intern(">>>");
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
    releaseAllBlocks();
    Obj* obj = objects_;
    while (obj != nullptr) {
        Obj* next = obj->next;
        delete obj;
        obj = next;
    }
}

// ---- Strings ----

// ---- Types when the program runs (M6.6 part 1) ----

ObjTypeInfo* VM::typeOf(uint8_t id, bool nullable, ObjClass* cls, const std::vector<ObjTypeInfo*>& args) {
    if (root_ != nullptr) {
        // A core's VM has no table of its own: a type is the same object for every core.
        std::lock_guard<std::mutex> hold(root_->internMutex_);
        return root_->typeOf(id, nullable, cls, args);
    }
    std::string key;
    key.reserve(10 + 8 * (args.size() + 1));
    key.push_back(static_cast<char>(id));
    key.push_back(nullable ? 1 : 0);
    auto pointer = [&](const void* p) {
        const uintptr_t bits = reinterpret_cast<uintptr_t>(p);
        key.append(reinterpret_cast<const char*>(&bits), sizeof bits);
    };
    pointer(cls);
    for (const ObjTypeInfo* arg : args) pointer(arg);
    auto found = types_.find(key);
    if (found != types_.end()) return found->second;
    ObjTypeInfo* type = alloc<ObjTypeInfo>(id, nullable, cls, args);
    type->immortal = true;
    type->core = 0;  // every core's to read
    types_.emplace(std::move(key), type);
    return type;
}

ObjTypeInfo* VM::typeOfValue(const Value& v) {
    if (ObjClass* cls = classOf(v)) {
        if (v.isObjType(ObjType::Instance))
            if (const ObjTypeInfo* own = asInstance(v)->type()) return const_cast<ObjTypeInfo*>(own);
        return typeOf(kTypeInstance, false, cls, {});
    }
    auto plain = [&](uint8_t id) { return typeOf(id, false, nullptr, {}); };
    switch (v.kind) {
        case ValueKind::Null: return plain(kTypeNull);  // null#, as Python's NoneType (D48)
        case ValueKind::Bool: return plain(kTypeBool);
        case ValueKind::Int: return plain(kTypeInt);
        case ValueKind::Char: return plain(kTypeChar);
        case ValueKind::Byte: return plain(kTypeByte);
        case ValueKind::FromEnd: return plain(kTypeIndex);
        case ValueKind::Float: return plain(kTypeFloat);
        case ValueKind::Obj: break;
        default: return plain(0);
    }
    ObjTypeInfo* const any = typeOf(0, true, nullptr, {});
    switch (v.as.obj->type) {
        case ObjType::String: return plain(kTypeString);
        case ObjType::List: return asList(v)->type != nullptr ? asList(v)->type : typeOf(kTypeList, false, nullptr, {any});
        case ObjType::Map: return asMap(v)->type != nullptr ? asMap(v)->type : typeOf(kTypeMap, false, nullptr, {any, any});
        case ObjType::Set:
            if (asSet(v)->type != nullptr) return asSet(v)->type;
            return typeOf(asSet(v)->values ? kTypeValues : kTypeSet, false, nullptr, {any});
        case ObjType::Range: return plain(kTypeRange);
        case ObjType::Bytes: return plain(kTypeBytes);
        case ObjType::Closure:
        case ObjType::Native:
        case ObjType::BoundMethod:
        case ObjType::BoundNative: return plain(kTypeFunction);
        case ObjType::TypeInfo: return plain(kTypeType);
        case ObjType::Class: return typeOf(kTypeInstance, false, static_cast<ObjClass*>(v.as.obj), {});
        default: return plain(0);
    }
}

Value VM::classDescription(ObjClass* cls, const ObjTypeInfo* own, int which) {
    VM& root = root_ != nullptr ? *root_ : *this;  // what every core has made, kept once
    ObjClosure* meta = nullptr;
    {
        std::lock_guard<std::mutex> hold(root.internMutex_);
        auto found = cls->described[which].find(own);
        if (found != cls->described[which].end()) return found->second;
        if (cls->metaProto[which] == nullptr) return Value::null();
        meta = cls->meta[which];
    }
    // Made outside the lock, which making a type takes, as is the function; two cores may make
    // them at once, and the first kept is kept.
    if (meta == nullptr) {
        meta = alloc<ObjClosure>(cls->metaProto[which], 0);
        std::lock_guard<std::mutex> hold(root.internMutex_);
        if (cls->meta[which] == nullptr) cls->meta[which] = meta;
        meta = cls->meta[which];
    }
    const Value made = callWithTypes(Value::object(meta), own != nullptr ? own->args : std::vector<ObjTypeInfo*>{}, 0, nullptr);
    std::lock_guard<std::mutex> hold(root.internMutex_);
    return cls->described[which].emplace(own, made).first->second;
}

// A property's getter and setter run, as `obj.name` and `obj.name = value` run them.
// Op::BundleValues (D143): bundle name1 .. nameN -> name1 value1 .. nameN valueN, each value
// the bundle's for that name. Kept out of the instruction loop, as it builds a list.
void VM::bundleValues(int count) {
    Value* asked = sp_ - count;
    const Value bundle = asked[-1];
    if (!bundle.isObjType(ObjType::Map)) throw runtimeError(Thrown::ClassCastException, "expected a bundle of named arguments");
    std::vector<Value> pairs;
    pairs.reserve(static_cast<size_t>(count) * 2);
    for (int k = 0; k < count; k++) {
        const Value* found = asMap(bundle)->map.find(asked[k]);
        pairs.push_back(asked[k]);
        pairs.push_back(found != nullptr ? *found : Value::null());
    }
    sp_ = asked - 1;
    for (const Value& v : pairs) *sp_++ = v;
}

// Op::MergeBundle: a + b of two bundles, b's value where both have a name.
void VM::mergeBundles() {
    const Value a = sp_[-2], b = sp_[-1];
    if (!a.isObjType(ObjType::Map) || !b.isObjType(ObjType::Map)) throw runtimeError(Thrown::ClassCastException, "expected bundles of named arguments");
    ObjMap* made = alloc<ObjMap>();
    made->map = asMap(a)->map;
    for (const ValueMap::Entry& entry : asMap(b)->map.entries())
        if (entry.live) made->map.set(entry.key, entry.value);
    sp_[-2] = Value::object(made);
    sp_--;
}

bool VM::widenedByte(Value& v, uint8_t type) {
    if (v.kind != ValueKind::Byte) return false;
    const uint8_t id = type & kTypeMask;
    if (id != kTypeInt && id != kTypeNumber && !isIntStorage(id)) return false;
    v = Value::integer(v.as.i);
    return true;
}

int VM::globalSlotOf(const ObjClass* cls) {
    auto known = classSlots_.find(cls);
    if (known != classSlots_.end() && static_cast<size_t>(known->second) < globals_.size() && globals_[static_cast<size_t>(known->second)].isObjType(ObjType::Class) &&
        asClass(globals_[static_cast<size_t>(known->second)]) == cls)
        return known->second;
    for (size_t i = 0; i < globals_.size(); i++)
        if (globals_[i].isObjType(ObjType::Class) && asClass(globals_[i]) == cls) {
            classSlots_[cls] = static_cast<int>(i);
            return static_cast<int>(i);
        }
    return -1;
}

ObjClass* VM::classInGlobal(int slot) const {
    if (slot < 0 || static_cast<size_t>(slot) >= globals_.size() || !globals_[static_cast<size_t>(slot)].isObjType(ObjType::Class)) return nullptr;
    return asClass(globals_[static_cast<size_t>(slot)]);
}

Value VM::memberGet(Value obj, ObjString* name) {
    if (const ObjClass* cls = classOf(obj); cls != nullptr && cls->hasAccessors)
        if (const Value* getter = cls->getters.find(name)) return callWithReceiver(obj, asClosure(*getter));
    return getProperty(obj, name);
}

void VM::memberSet(Value obj, ObjString* name, Value value) {
    if (const ObjClass* cls = classOf(obj); cls != nullptr && cls->hasAccessors) {
        if (const Value* setter = cls->setters.find(name)) {
            const Value bound = Value::object(alloc<ObjBoundMethod>(obj, asClosure(*setter)));
            TempRoot keep(*this, bound);
            call(bound, 1, &value);
            return;
        }
    }
    setProperty(obj, name, value);
}

// ---- Atomic fields (D92) ----
// An `atomic var` field changed in one step, whoever runs the code: an int's or a float's kept
// as its payload with the processor's atomic operations, anything else under a lock shared by
// address. Any piece of a parallel section may change one of an object it may read; what it
// stores has to be a value every core may read.
Value VM::atomicField(Value object, ObjString* name, int kind, const Value* operands) {
    if (!object.isObjType(ObjType::Instance)) throw runtimeError(Thrown::InternalError, "an atomic field belongs to an object");
    guardRead(object);
    ObjInstance* instance = asInstance(object);
    const FieldDecl* d = instance->declOf(name);
    if (d == nullptr) throw runtimeError(Thrown::InternalError, "no atomic field '" + name->utf8() + "'");
    const Value operand = operands[kind == 4 ? 1 : 0];
    if (coreId_ != 0 && operand.isObj() && operand.as.obj->frozen == 0 && operand.as.obj->type != ObjType::String)
        throw runtimeError(Thrown::IllegalStateException, "in a parallel section an atomic field is given values: numbers, bools, small values, text or frozen data; not this " +
                                                              typeName(operand) + ", which a core may still change");
    auto change = [&](const Value& old) -> Value {
        if (kind == 4) return operand;
        if (old.isInt() && operand.isInt()) {
            int64_t r = 0;
            if (kind == 0 ? addOverflows(old.as.i, operand.as.i, r) : kind == 1 ? subOverflows(old.as.i, operand.as.i, r) : false)
                throw overflowError(kind == 0 ? Op::Add : Op::Sub, old.as.i, operand.as.i);
            if (kind == 2) r = std::max(old.as.i, operand.as.i);
            if (kind == 3) r = std::min(old.as.i, operand.as.i);
            return Value::integer(r);
        }
        if (!old.isNumber() || !operand.isNumber())
            throw runtimeError(Thrown::ClassCastException, "'" + name->utf8() + "' holds " + typeName(old) + ", which " + (kind < 2 ? "+= and -=" : "max and min") +
                                                               " do not take with " + typeName(operand));
        const double a = old.toDouble(), b = operand.toDouble();
        return Value::number(kind == 0 ? a + b : kind == 1 ? a - b : kind == 2 ? std::max(a, b) : std::min(a, b));
    };
    if (d->payload == FieldDecl::IntPayload || d->payload == FieldDecl::FloatPayload) {
        std::atomic_ref<int64_t> held(*reinterpret_cast<int64_t*>(instance->payloadData() + d->payloadOffset));
        const ValueKind valueKind = d->payload == FieldDecl::IntPayload ? ValueKind::Int : ValueKind::Float;
        auto toValue = [&](int64_t bits) {
            Value v;
            v.kind = valueKind;
            v.as.i = bits;
            return v;
        };
        auto toBits = [&](const Value& v) {
            if (valueKind == ValueKind::Float && v.isInt()) return Value::number(static_cast<double>(v.as.i)).as.i;
            return v.as.i;
        };
        int64_t old = held.load();
        if (kind == 4) {
            const Value expected = operands[0];
            if (!expected.isNumber()) return Value::boolean(false);
            int64_t want = toBits(expected);
            return Value::boolean(held.compare_exchange_strong(want, toBits(operand)));
        }
        for (;;) {
            const Value made = change(toValue(old));
            if (valueKind == ValueKind::Int && !made.isInt())
                throw runtimeError(Thrown::ClassCastException, "'" + name->utf8() + "' holds an int, which a float does not fit");
            if (held.compare_exchange_weak(old, toBits(made))) return toValue(toBits(made));
        }
    }
    if (d->payload != FieldDecl::NoPayload) throw runtimeError(Thrown::UnsupportedOperationException, "an atomic field holds a number, a bool, a small value or frozen data");
    static std::mutex locks[64];
    std::lock_guard<std::mutex> hold(locks[(reinterpret_cast<uintptr_t>(instance) >> 4) & 63]);
    Value& slot = instance->fields()[d->slot];
    if (kind == 4) {
        const Value expected = operands[0];
        const bool same = slot.kind == expected.kind && (slot.isObj() ? slot.as.obj == expected.as.obj : slot.isFloat() ? slot.as.i == expected.as.i : slot.as.i == expected.as.i);
        if (!same) return Value::boolean(false);
        slot = operand;
        return Value::boolean(true);
    }
    slot = change(slot);
    return slot;
}

Value VM::callWithTypes(Value callee, const std::vector<ObjTypeInfo*>& typeArgs, int argc, const Value* args) {
    struct Clear {
        const ObjTypeInfo*& pending;
        ~Clear() { pending = nullptr; }  // not taken by a callee that has none
    } clear{pendingTypeArgs_};
    pendingTypeArgs_ = typeArgs.empty() ? nullptr : typeOf(kTypeArguments, false, nullptr, typeArgs);
    return call(callee, argc, args);
}

std::string VM::typeInfoText(const ObjTypeInfo* type) const {
    std::string text;
    if (type->id == kTypeInstance) {
        text = type->cls->name->utf8();
    } else if (type->id == 0) {
        text = "any";
    } else {
        text = declaredTypeText(type->id, nullptr);
    }
    if (!type->args.empty()) {
        text += "<";
        for (size_t i = 0; i < type->args.size(); i++) text += (i ? ", " : "") + typeInfoText(type->args[i]);
        text += ">";
    }
    if (type->nullable) text += "?";
    return text;
}

ObjTypeInfo* VM::typeFromShape(const std::string& shape, size_t& at, const Value*& operand) {
    bool nullable = false;
    if (at < shape.size() && shape[at] == '?') {
        nullable = true;
        at++;
    }
    if (at >= shape.size()) throw runtimeError(Thrown::InternalError, "invalid type shape");
    const char c = shape[at++];
    switch (c) {
        case 'a': return typeOf(0, nullable, nullptr, {});
        case 'i': return typeOf(kTypeInt, nullable, nullptr, {});
        case 'f': return typeOf(kTypeFloat, nullable, nullptr, {});
        case 'n': return typeOf(kTypeNumber, nullable, nullptr, {});
        case 's': return typeOf(kTypeString, nullable, nullptr, {});
        case 'b': return typeOf(kTypeBool, nullable, nullptr, {});
        case 'y': return typeOf(kTypeBytes, nullable, nullptr, {});
        case 'c': return typeOf(kTypeChar, nullable, nullptr, {});
        case 'u': return typeOf(kTypeByte, nullable, nullptr, {});
        case 'h': return typeOf(kTypeInt16, nullable, nullptr, {});
        case 'd': return typeOf(kTypeInt32, nullable, nullptr, {});
        case 'H': return typeOf(kTypeUint16, nullable, nullptr, {});
        case 'D': return typeOf(kTypeUint32, nullable, nullptr, {});
        case 'g': return typeOf(kTypeFloat32, nullable, nullptr, {});
        case 'x': return typeOf(kTypeIndex, nullable, nullptr, {});
        case 'r': return typeOf(kTypeRange, nullable, nullptr, {});
        case 't': return typeOf(kTypeType, nullable, nullptr, {});
        case 'F': return typeOf(kTypeFunction, nullable, nullptr, {});
        case 'L': {
            ObjTypeInfo* item = typeFromShape(shape, at, operand);
            return typeOf(kTypeList, nullable, nullptr, {item});
        }
        case 'S': {
            ObjTypeInfo* item = typeFromShape(shape, at, operand);
            return typeOf(kTypeSet, nullable, nullptr, {item});
        }
        case 'V': {
            ObjTypeInfo* item = typeFromShape(shape, at, operand);
            return typeOf(kTypeValues, nullable, nullptr, {item});
        }
        case 'M': {
            ObjTypeInfo* key = typeFromShape(shape, at, operand);
            ObjTypeInfo* value = typeFromShape(shape, at, operand);
            return typeOf(kTypeMap, nullable, nullptr, {key, value});
        }
        case 'Z': {
            // FrozenList<T> and the others (D26): the list's type, frozen.
            const ObjTypeInfo* plain = typeFromShape(shape, at, operand);
            if (frozenTypeId(plain->id) == 0) throw runtimeError(Thrown::InternalError, "invalid type shape");
            return typeOf(frozenTypeId(plain->id), nullable, nullptr, plain->args);
        }
        case 'C': {
            const int count = shape[at++] - '0';
            const Value cls = *operand++;
            if (!cls.isObjType(ObjType::Class)) throw runtimeError(Thrown::ClassCastException, "a type must be a class or a built-in type, not a value of type " + typeName(cls));
            std::vector<ObjTypeInfo*> args;
            for (int i = 0; i < count; i++) args.push_back(typeFromShape(shape, at, operand));
            return typeOf(kTypeInstance, nullable, asClass(cls), args);
        }
        case 'A': {
            const int count = shape[at++] - '0';
            std::vector<ObjTypeInfo*> args;
            for (int i = 0; i < count; i++) args.push_back(typeFromShape(shape, at, operand));
            return typeOf(kTypeArguments, false, nullptr, args);
        }
        case 'P': {
            const Value given = *operand++;
            if (!given.isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
            ObjTypeInfo* type = static_cast<ObjTypeInfo*>(given.as.obj);
            if (!nullable || type->nullable) return type;  // as it is: no need to look it up
            return typeOf(type->id, true, type->cls, type->args);
        }
        default:
            throw runtimeError(Thrown::InternalError, "invalid type shape");
    }
}

std::string VM::valueTypeText(const Value& v) const {
    if (v.isObjType(ObjType::List) && asList(v)->type != nullptr) return typeInfoText(asList(v)->type);
    if (v.isObjType(ObjType::Map) && asMap(v)->type != nullptr) return typeInfoText(asMap(v)->type);
    if (v.isObjType(ObjType::Instance) && asInstance(v)->type() != nullptr) return typeInfoText(asInstance(v)->type());
    return typeName(v);
}

const std::vector<ObjTypeInfo*>& VM::supertypesOf(ObjClass* cls, const ObjTypeInfo* own) {
    VM& root = root_ != nullptr ? *root_ : *this;  // what every core has made, kept once
    {
        std::lock_guard<std::mutex> hold(root.internMutex_);
        auto found = cls->supertypesMade.find(own);
        if (found != cls->supertypesMade.end()) return found->second;
    }
    // Made outside the lock, which making a type takes. Two cores may make them at once, and
    // make the same types, which are interned: the first kept is kept.
    std::vector<ObjTypeInfo*> made;
    const ObjTypeInfo* args = own != nullptr ? typeOf(kTypeArguments, false, nullptr, own->args) : nullptr;
    for (ObjClosure* maker : cls->supertypeMakers) {
        pendingTypeArgs_ = args;
        const Value type = call(Value::object(maker), 0, nullptr);
        if (type.isObjType(ObjType::TypeInfo) && static_cast<ObjTypeInfo*>(type.as.obj)->id == kTypeInstance)
            made.push_back(static_cast<ObjTypeInfo*>(type.as.obj));
    }
    std::lock_guard<std::mutex> hold(root.internMutex_);
    return cls->supertypesMade.emplace(own, std::move(made)).first->second;
}

const ObjTypeInfo* VM::typeAs(ObjClass* cls, const ObjTypeInfo* own, const ObjClass* target) {
    while (cls != target) {
        if (cls == nullptr) return nullptr;
        // A supertype written with type arguments that leads to the target, or else the
        // superclass, written without them.
        const ObjTypeInfo* next = nullptr;
        if (!cls->supertypeMakers.empty())
            for (ObjTypeInfo* made : supertypesOf(cls, own))
                if (made->cls->isSubclassOf(target)) {
                    next = made;
                    break;
                }
        if (next != nullptr) {
            cls = next->cls;
            own = next;
            continue;
        }
        if (cls->super == nullptr || !cls->super->isSubclassOf(target)) return nullptr;  // by an interface written bare
        cls = cls->super;
        own = nullptr;
    }
    return own;
}

bool VM::isSubtype(const ObjTypeInfo* a, const ObjTypeInfo* b) {
    if (a == b) return true;
    if (a->nullable && !b->nullable) return false;
    if (b->id == 0) return true;  // any
    if (a->id == 0) return false;
    if (b->id == kTypeNumber && (a->id == kTypeInt || a->id == kTypeFloat || a->id == kTypeNumber)) return true;
    // A FrozenList is a List (D26).
    if (a->id != b->id && unfrozenTypeId(a->id) != b->id) return false;
    if (a->id == kTypeInstance && !a->cls->isSubclassOf(b->cls)) return false;
    if (a->id == kTypeInstance && !b->args.empty() && a->cls != b->cls) {
        // A subclass's type, seen as the class's: `Pair<int>` as the `Base<List<int>>` it extends.
        a = typeAs(a->cls, a, b->cls);
        if (a == nullptr) return true;  // made without type arguments: takes any
    }
    if (a->id == kTypeInstance && b->args.empty()) return true;
    if (a->args.size() != b->args.size()) return b->args.empty();
    for (size_t i = 0; i < a->args.size(); i++)
        if (!isSubtype(a->args[i], b->args[i])) return false;
    return true;
}

bool VM::fitsType(const Value& v, const ObjTypeInfo* type) {
    if (v.isNull()) return type->nullable || type->id == 0 || type->id == kTypeNull;
    if (type->id == 0) return true;
    if (type->id == kTypeNull) return false;  // null alone (D48)
    if (type->id == kTypeInstance) {
        const ObjClass* const cls = classOf(v);
        if (cls == nullptr || !cls->isSubclassOf(type->cls)) return false;
        if (type->args.empty() || !v.isObjType(ObjType::Instance)) return true;  // a value keeps no type arguments yet
        // An object made without its type arguments takes any; one of a subclass is seen as
        // the class it extends.
        ObjInstance* instance = asInstance(v);
        const ObjTypeInfo* made = typeAs(instance->cls, instance->type(), type->cls);
        if (made == nullptr || made->args.size() != type->args.size()) return true;
        for (size_t i = 0; i < made->args.size(); i++)
            if (!isSubtype(made->args[i], type->args[i])) return false;
        return true;
    }
    if (!matchesType(v, type->id, nullptr)) return false;
    if (type->args.empty()) return true;
    // A list or map made before its type was kept, or by code that did not say, takes anything.
    const ObjTypeInfo* made = v.isObjType(ObjType::List) ? asList(v)->type : v.isObjType(ObjType::Map) ? asMap(v)->type
                              : v.isObjType(ObjType::Set)                         ? asSet(v)->type
                                                                                  : nullptr;
    if (made == nullptr || made->args.size() != type->args.size()) return true;
    for (size_t i = 0; i < made->args.size(); i++)
        if (!isSubtype(made->args[i], type->args[i])) return false;
    return true;
}

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
    interned_.emplace(std::string_view(str->utf8()), str);
    return str;
}

ObjString* VM::newString(std::string s) {
    account(s.size());
    return alloc<ObjString>(std::move(s));
}

ObjString* VM::joinStrings(const ObjString* first, const ObjString* second) {
    account(first->units().size() + second->units().size());
    return alloc<ObjString>(first, second);
}

ObjString* VM::newString(Characters text) {
    account(text.units.size());
    return alloc<ObjString>(std::move(text));
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

ObjNative* VM::builtinStatic(const std::string& cls, const std::string& name) {
    auto found = builtins_.find(cls);
    if (found == builtins_.end() || !found->second.isObjType(ObjType::Class)) return nullptr;
    const Value* member = asClass(found->second)->statics.find(intern(name));
    if (member == nullptr || !member->isObjType(ObjType::Native)) return nullptr;
    return asNative(*member);
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
    auto& forms = found->second.forms;
    if (std::find(forms.begin(), forms.end(), params) == forms.end()) forms.push_back(params);
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
    else if (name == "char") type = BuiltinType::Char;
    else if (name == "byte") type = BuiltinType::Byte;
    else if (name == "Index") type = BuiltinType::Index;
    else if (name == "Set") type = BuiltinType::Set;
    else return false;
    return true;
}

void VM::collect(Value iterable, std::vector<Value>& out) {
    guardRead(iterable);
    if (iterable.isObjType(ObjType::List)) {
        ObjList* list = asList(iterable);
        if (!list->packed()) {
            out.insert(out.end(), list->values().begin(), list->values().end());
            return;
        }
        for (size_t i = 0; i < list->size(); i++) out.push_back(listGet(list, i));
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
        // The standard library defines the classes runtime errors are instances of (D35).
        loaded_[0] = true;
        execute(program_->modules[0]);
        const std::vector<std::string>& known = globalsFoundByName();
        for (size_t i = 0; i < globals_.size(); i++) {
            if (!globals_[i].isObjType(ObjType::Class)) continue;
            for (size_t k = 0; k < static_cast<size_t>(Thrown::Count_); k++)
                if (thrownClasses_[k] == nullptr && program_->globalNames[i] == known[k]) thrownClasses_[k] = asClass(globals_[i]);
            if (program_->globalNames[i] == "RuntimeException" && errorClass_ == nullptr) errorClass_ = asClass(globals_[i]);
            if (program_->globalNames[i] == "MapEntry" && globals_[i].isObjType(ObjType::Class) && mapEntryClass_ == nullptr)
                mapEntryClass_ = asClass(globals_[i]);
            if (program_->globalNames[i] == "Class" && globals_[i].isObjType(ObjType::Class) && classOfFn_.isNull()) classOfFn_ = globals_[i];
            if (program_->globalNames[i] == "ListView" && globals_[i].isObjType(ObjType::Class) && viewClass_ == nullptr)
                viewClass_ = asClass(globals_[i]);
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
    if (nativeDepth_ >= kMaxNativeDepth || !roomFor(sp_ + argc + 1)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");

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
    if (nativeDepth_ >= kMaxNativeDepth || !roomFor(sp_ + argc + 1)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");

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
    // Under its own name always, whether or not the name has other forms: a call the checker
    // has told which form it means names it so (one with a receiver that takes null, which null
    // alone could not choose).
    if (!added->proto->signature.empty()) table.set(intern(name->utf8() + added->proto->signature), function);
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
    std::unique_ptr<FunctionProto>& proto = extensionSets_[name->utf8()];
    if (proto == nullptr) {
        proto = std::make_unique<FunctionProto>();
        proto->name = name->utf8();
        proto->chooser = true;
    }
    for (ObjClosure* form : forms) table.set(intern(name->utf8() + form->proto->signature), Value::object(form));
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
                throw const_cast<VM*>(this)->runtimeError(Thrown::IllegalStateException, 
                    name->utf8() + "() is called on null, and more than one type has its own " + name->utf8() +
                    "() that takes null, so there is no telling which is meant; use ?: to say what stands in for null");
            break;
        case ValueKind::Bool: found = typeExtensions_[kTypeBool].find(name); break;
        case ValueKind::Int: found = typeExtensions_[kTypeInt].find(name); break;
        case ValueKind::Float: found = typeExtensions_[kTypeFloat].find(name); break;
        case ValueKind::Char: found = typeExtensions_[kTypeChar].find(name); break;
        case ValueKind::Byte: found = typeExtensions_[kTypeByte].find(name); break;
        case ValueKind::FromEnd: found = typeExtensions_[kTypeIndex].find(name); break;
        case ValueKind::Obj:
            switch (receiver.as.obj->type) {
                case ObjType::String: found = typeExtensions_[kTypeString].find(name); break;
                case ObjType::List: found = typeExtensions_[kTypeList].find(name); break;
                case ObjType::Map: found = typeExtensions_[kTypeMap].find(name); break;
                case ObjType::Set: found = typeExtensions_[asSet(receiver)->values ? kTypeValues : kTypeSet].find(name); break;
                case ObjType::Range: found = typeExtensions_[kTypeRange].find(name); break;
                case ObjType::Bytes: found = typeExtensions_[kTypeBytes].find(name); break;
                case ObjType::Closure:
                case ObjType::Native:
                case ObjType::BoundMethod:
                case ObjType::BoundNative: found = typeExtensions_[kTypeFunction].find(name); break;
                case ObjType::Instance:
                case ObjType::Value: {
                    const ObjClass* cls = classOf(receiver);
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
    if (candidate.isObjType(ObjType::Value) || candidate.isSmall() || candidate.isRef()) {
        const ObjClass* cls = classOf(candidate);
        if (cls->declaredIndex.find(name) != nullptr || cls->methods.find(name) != nullptr || cls->getters.find(name) != nullptr)
            return true;
        return findExtension(candidate, name) != nullptr;
    }
    if (candidate.isObjType(ObjType::Instance)) {
        const ObjInstance* instance = asInstance(candidate);
        const ObjClass* cls = instance->cls;
        if (instance->hasValue(name) || cls->methods.find(name) != nullptr ||
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
    if (name == names.size)
        return type == ObjType::String || type == ObjType::List || type == ObjType::Map || type == ObjType::Range ||
               type == ObjType::Bytes || type == ObjType::Set;
    if (name == names.indices || name == names.lastIndex) return type == ObjType::String || type == ObjType::List;
    if (name == names.itemType) return type == ObjType::List;
    if (name == names.keyType || name == names.valueType) return type == ObjType::Map;
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
            throw runtimeError(Thrown::InternalError, "class " + cls->name->utf8() + " has too many private, protected and internal members");
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
                    throw notOwned("class " + owner->name->utf8() + " has protected static members, and a subclass of it is declared for the whole program");
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
bool VM::memberDenied(const ObjClass* cls, const ObjString* name, const FunctionProto* proto, bool write) const {
    // Code written in the object's own class may use all that the class has, or all but what a
    // class above keeps private: then a name that is private nowhere is its to use.
    if ((cls->accessBits >> (name->hash & 63) & 1) == 0) return false;
    if (proto != nullptr && (cls->accessAllOwn || (cls->accessSharedOwn && (name->restricted & 4) == 0)) && writtenIn(proto, cls)) return false;
    const Value* mark = cls->accessMarks.find(name);
    if (mark == nullptr) return false;
    if (proto == nullptr) return halfCount(halfOf(*mark, false)) != 0 || (write && halfCount(halfOf(*mark, true)) != 0);
    return halfDenies(cls, halfOf(*mark, false), proto) || (write && halfDenies(cls, halfOf(*mark, true), proto));
}

bool VM::accessDenied(Value target, const ObjString* name, const FunctionProto* proto, bool write) const {
    if (!target.isObj() && !target.isSmall() && !target.isRef()) return false;
    const ObjClass* cls;
    const Table* marks;
    if (!target.isObj() || target.as.obj->type == ObjType::Instance || target.as.obj->type == ObjType::Value) {
        return memberDenied(classOf(target), name, proto, write);
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
    if (mark == nullptr) return runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' cannot be used here");
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
        return runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' cannot be assigned here: its setter is " + word + " in class " +
                            declaring->name->utf8() + ", so " + who + "assign to it");
    return runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' is " + word + " in class " + declaring->name->utf8() + ": " + who + "use it");
}

// An entry of FunctionProto::outerReceivers: an upvalue of the function, or with kCapturedReceiver
// a capture of the anonymous object a member runs on.
ObjUpvalue* VM::receiverUpvalue(const CallFrame* frame, const Value* base, uint16_t entry) const {
    if ((entry & kCapturedReceiver) == 0) return frame->closure->upvalues[entry];
    const uint16_t k = entry & static_cast<uint16_t>(~kCapturedReceiver);
    if (!base[0].isObjType(ObjType::Instance) || k >= asInstance(base[0])->captureCount) return nullptr;
    return asInstance(base[0])->captured(k);
}

Value VM::implicitReceiver(const CallFrame* frame, const Value* base, const ObjString* name) const {
    const FunctionProto* proto = frame->closure->proto;
    auto has = [&](Value candidate) {
        return hasMember(candidate, name) && !(name->restricted != 0 && accessDenied(candidate, name, proto, false));
    };
    implicitTried_ = implicitFirstCount_;
    if (implicitFirstCount_ != 0) {
        // The objects of blocks copied in place, innermost first (D58, D59).
        const int count = implicitFirstCount_;
        implicitFirstCount_ = 0;
        const bool only = std::exchange(implicitOnly_, false);
        for (int i = count; i-- > 0;)
            if (has(implicitFirst_[i])) return implicitFirst_[i];
        if (only) return Value::undefined();
    }
    if (has(base[0])) return base[0];
    for (uint16_t upvalue : proto->outerReceivers) {
        const ObjUpvalue* cell = receiverUpvalue(frame, base, upvalue);
        if (cell != nullptr && has(*cell->location)) return *cell->location;
    }
    return Value::undefined();
}

// A bare name that is neither a member the code may use nor a variable: said plainly when
// it is a member of the object the code runs on that the code may not use.
AstraThrow VM::undefinedName(const CallFrame* frame, const Value* base, const ObjString* name, size_t global) {
    const FunctionProto* proto = frame->closure->proto;
    auto kept = [&](Value candidate) { return hasMember(candidate, name) && accessDenied(candidate, name, proto, false); };
    for (int i = std::exchange(implicitTried_, uint8_t{0}); i-- > 0;)
        if (kept(implicitFirst_[i])) return accessError(implicitFirst_[i], name, proto);
    if (kept(base[0])) return accessError(base[0], name, proto);
    for (uint16_t upvalue : proto->outerReceivers) {
        const ObjUpvalue* cell = receiverUpvalue(frame, base, upvalue);
        if (cell == nullptr) continue;
        Value outer = *cell->location;
        if (kept(outer)) return accessError(outer, name, proto);
    }
    return runtimeError(Thrown::IllegalStateException, "undefined variable '" + program_->globalNames[global] + "'");
}

// Runs a method that takes no arguments on `receiver` and returns its result.
Value VM::callWithReceiver(Value receiver, ObjClosure* method) {
    if (nativeDepth_ >= kMaxNativeDepth || !roomFor(sp_ + 1)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");

    NativeCallGuard guard(nativeDepth_, nativeBase_);

    int base = frameCount_;
    *sp_++ = receiver;
    callClosure(method, 0);
    run(base);
    return *--sp_;
}

// ---- Errors ----

AstraThrow VM::runtimeError(const std::string& message) { return runtimeError(Thrown::RuntimeException, message); }

AstraThrow VM::runtimeError(Thrown kind, const std::string& message) {
    AstraThrow t;
    ObjClass* cls = thrownClasses_[static_cast<size_t>(kind)];
    if (cls == nullptr) cls = errorClass_;
    if (cls == nullptr) {
        t.value = str(message);  // before the standard library has its classes
    } else {
        ObjInstance* error = newInstance(cls);
        error->setField(names.message, str(message));
        error->setField(names.cause, Value::null());
        t.value = Value::object(error);
    }
    return t;
}

// The classes of Thrown, in its order, then the other globals the VM finds by name.
const std::vector<std::string>& VM::globalsFoundByName() {
    static const std::vector<std::string> names{
        "RuntimeException", "ArithmeticException", "IndexOutOfBoundsException", "NullPointerException", "ClassCastException",
        "IllegalArgumentException", "NumberFormatException", "IllegalStateException", "UnsupportedOperationException",
        "NoSuchElementException", "ConcurrentModificationException", "AssertionError", "StackOverflowError", "OutOfMemoryError",
        "InternalError", "IOException", "Throwable", "SocketTimeoutException", "MapEntry"};
    return names;
}

std::vector<std::string> VM::membersFoundByName() const {
    std::vector<std::string> found{"onError"};  // a worker's handler (worker.cpp)
    // Every member of the names table is an ObjString*.
    const ObjString* const* first = reinterpret_cast<const ObjString* const*>(&names);
    for (size_t i = 0; i < sizeof names / sizeof(ObjString*); i++)
        if (first[i] != nullptr) found.push_back(first[i]->utf8());
    return found;
}

AstraThrow VM::errorOf(const char* className, const std::string& message) {
    const std::vector<std::string>& known = globalsFoundByName();
    if (std::find(known.begin(), known.end(), className) == known.end())
        return runtimeError(Thrown::InternalError, std::string("internal error: ") + className + " is not among VM::globalsFoundByName()");
    for (size_t k = 0; k < static_cast<size_t>(Thrown::Count_); k++)
        if (known[k] == className) return runtimeError(static_cast<Thrown>(k), message);
    return runtimeError(message);
}

// What the program is told when memory runs out: an OutOfMemoryError, which it can catch.
AstraThrow VM::outOfMemory() {
    return runtimeError(Thrown::OutOfMemoryError, "out of memory");
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
        std::string line = "at " + p->name + " (" + p->source + ":" + std::to_string(p->lines[offset]) + ")";
        if (f.repeats > 0) {
            // In place of the frames a call of itself as its last step saved (D29).
            std::string count = std::to_string(static_cast<uint64_t>(f.repeats) + 1);
            for (int at = static_cast<int>(count.size()) - 3; at > 0; at -= 3) count.insert(static_cast<size_t>(at), ",");
            line += ", " + count + " times";
        }
        trace.push_back(std::move(line));
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
    pendingTypeArgs_ = nullptr;  // a call that failed before it began
    pendingDestination_ = Value::null();
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
        case ValueKind::Ref: return refClass(v)->name->utf8();
        case ValueKind::Small: return smallClassOf(v)->name->utf8();
        case ValueKind::Bool: return "bool";
        case ValueKind::Int: return "int";
        case ValueKind::Char: return "char";
        case ValueKind::Byte: return "byte";
        case ValueKind::FromEnd: return "Index";
        case ValueKind::Float: return "float";
        case ValueKind::Undefined: return "undefined";
        case ValueKind::Obj: break;
    }
    switch (v.as.obj->type) {
        case ObjType::String: return "string";
        case ObjType::List: return "List";
        case ObjType::Map: return "Map";
        case ObjType::Set: return asSet(v)->values ? "Values" : "Set";
        case ObjType::Range: return "Range";
        case ObjType::Bytes: return "Bytes";
        case ObjType::Closure:
        case ObjType::Native:
        case ObjType::BoundMethod:
        case ObjType::BoundNative: return "Function";
        case ObjType::Class: return "Class";
        case ObjType::Instance: return asInstance(v)->cls->name->utf8();
        case ObjType::Value: return asValue(v)->cls->name->utf8();
        case ObjType::Fiber: return "task";
        case ObjType::Resource: return static_cast<const ObjResource*>(v.as.obj)->kind;
        case ObjType::Upvalue:
        case ObjType::Iterator: return "internal";
        case ObjType::TypeInfo: return "Type";
    }
    return "unknown";
}

// A byte is taken where an int or a number is (M6.6 part 8, group 9).
int64_t VM::toInt(Value v, const char* what) {
    if (!v.isInt() && !v.isByte()) throw runtimeError(Thrown::ClassCastException, std::string(what) + " must be an int, got " + typeName(v));
    return v.as.i;
}

double VM::toNumber(Value v, const char* what) {
    if (v.isByte()) return static_cast<double>(v.as.i);
    if (!v.isNumber()) throw runtimeError(Thrown::ClassCastException, std::string(what) + " must be a number, got " + typeName(v));
    return v.toDouble();
}

const std::string& VM::toStr(Value v, const char* what) {
    if (!v.isString()) throw runtimeError(Thrown::ClassCastException, std::string(what) + " must be a string, got " + typeName(v));
    return asString(v)->utf8();
}

// ---- Calls ----

void VM::checkArity(const std::string& name, int minArgs, int maxArgs, int argc) {
    if (argc >= minArgs && (maxArgs < 0 || argc <= maxArgs)) return;
    std::string expected = maxArgs < 0 ? "at least " + std::to_string(minArgs)
                           : minArgs == maxArgs ? std::to_string(minArgs)
                           : std::to_string(minArgs) + " to " + std::to_string(maxArgs);
    throw runtimeError(Thrown::IllegalArgumentException, name + "() expects " + expected + " argument(s) but got " + std::to_string(argc));
}

// A bare name was passed where nothing says which enum it could belong to.
AstraThrow VM::unsettledName(Value pending) {
    pendingEnums_--;
    return runtimeError(Thrown::IllegalStateException, "undefined variable '" + static_cast<ObjString*>(pending.as.obj)->utf8() + "'");
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
        throw runtimeError(Thrown::ClassCastException, "'" + name->utf8() + "' is not an entry of enum " + type->name->utf8());
    return *entry;
}

// For a function with a `vararg` parameter: gathers the arguments from that parameter's
// position on into a list, leaving the parameters after it to their defaults. Returns the
// new argument count.
int VM::packVarargs(const FunctionProto* proto, int argc) {
    const int at = proto->varargIndex;
    if (argc < proto->requiredArgs()) checkArity(proto->name, proto->requiredArgs(), -1, argc);
    if (!roomFor(sp_ - argc - 1 + proto->maxStack)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");

    for (; argc < at; argc++) *sp_++ = Value::undefined();
    const int extra = argc - at;
    if (pendingEnums_ > 0) rejectPendingNames(sp_ - extra, extra);
    ObjList* list = alloc<ObjList>();
    list->values().assign(sp_ - extra, sp_);
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
    if (classOf(v) == nullptr || className == nullptr) return -1;
    // The class is known by its name, as written: the last part of it.
    auto named = [&](const ObjClass* c) {
        const std::string& n = c->name->utf8();
        const std::string& want = className->utf8();
        return n == want || (n.size() > want.size() && n[n.size() - want.size() - 1] == '.' &&
                             n.compare(n.size() - want.size(), want.size(), want) == 0);
    };
    int distance = 0;
    for (const ObjClass* c = classOf(v); c != nullptr; c = c->super, distance++) {
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
        if (name->utf8().empty()) {
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
            const std::string& n = asString(pairs[2 * k])->utf8();
            given += (given.empty() ? "" : ", ") + (n.empty() ? std::string() : n + " = ") + typeName(pairs[2 * k + 1]);
        }
        throw runtimeError(Thrown::IllegalArgumentException, "no form of " + name + "() takes " + (given.empty() ? std::string("no arguments") : "(" + given + ")") +
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
        throw runtimeError(Thrown::IllegalArgumentException, "this call fits more than one form of " + name + "(): " + formsText(tied) +
                           "; write the types of what is passed, so that the form is chosen when compiling");
    }
    return best;
}

// The first `base` of `outer` (any where it has fewer), then all of `own`.
const ObjTypeInfo* VM::joinTypeArgs(const ObjTypeInfo* outer, int base, const ObjTypeInfo* own) {
    std::vector<ObjTypeInfo*> all;
    all.reserve(static_cast<size_t>(base) + (own != nullptr ? own->args.size() : 0));
    for (int i = 0; i < base; i++) all.push_back(typeArgument(outer, i));
    if (own != nullptr) all.insert(all.end(), own->args.begin(), own->args.end());
    return typeOf(kTypeArguments, false, nullptr, all);
}

void VM::callClosure(ObjClosure* closure, int argc, bool packed) {
    if (closure->proto->chooser) closure = chooseForm(static_cast<const ObjForms*>(closure), sp_ - argc, argc, nullptr, 0);
    const FunctionProto* proto = closure->proto;
    if (proto->varargIndex >= 0 && !packed) argc = packVarargs(proto, argc);
    if (argc != proto->arity && (argc > proto->arity || argc < proto->requiredArgs()))
        checkArity(proto->name, proto->requiredArgs(), proto->arity, argc);

    Value* base = sp_ - argc - 1;
    if ((frameCount_ == frameLimit_ || stackEnd_ - base < proto->maxStack) && !growContext(base + proto->maxStack, frameCount_ + 1))
        throw runtimeError(Thrown::StackOverflowError, "stack overflow");

    if (pendingEnums_ > 0) {
        // A bare name may only arrive at a parameter that could make an enum entry of it.
        for (int i = 0; i < argc; i++)
            if (base[i + 1].kind == ValueKind::EnumName && (i >= 64 || ((proto->classParams >> i) & 1) == 0))
                throw unsettledName(base[i + 1]);
    }

    // Arguments that were left out are marked, and the function fills in their defaults.
    for (int missing = argc; missing < proto->arity; missing++) *sp_++ = Value::undefined();

    // A generic function declared inside another (G1) has those of where it was made and then
    // its own, made before the frame is, as making them may collect.
    const ObjTypeInfo* joined = (proto->typeArgs & kTypeArgsOwn) != 0 && proto->typeArgBase != 0
                                    ? joinTypeArgs(closure->typeArgs, proto->typeArgBase, pendingTypeArgs_)
                                    : nullptr;

    CallFrame& frame = frames_[static_cast<size_t>(frameCount_++)];
    frame.dropResult = false;
    frame.repeats = 0;
    frame.closure = closure;
    // A generic function takes those WithTypes gave; a lambda has those of where it was made.
    // Only a function that reads them keeps them, which the others are not slowed by.
    if (proto->typeArgs != 0) {
        frameTypeArgs(frameCount_ - 1) = joined != nullptr ? joined
                                         : pendingTypeArgs_ != nullptr && (proto->typeArgs & kTypeArgsOwn) != 0 ? pendingTypeArgs_
                                                                                                                 : closure->typeArgs;
        pendingTypeArgs_ = nullptr;
        if ((proto->typeArgs & kTakesDestination) != 0) frameDestination(frameCount_ - 1) = pendingDestination_;
    }
    pendingDestination_ = Value::null();
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
    if (!roomFor(sp_ + arity + 1)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");

    int given = argc - 1;
    const int target = arity - 1;
    if (given < proto->requiredArgs()) {
        // The lambda may pass over parameters that have a default, and no others.
        if (proto->paramNames.size() != static_cast<size_t>(arity)) checkArity(proto->name, proto->requiredArgs(), arity, argc);
        throw runtimeError(Thrown::IllegalArgumentException, proto->name + "() is missing argument '" + proto->paramNames[static_cast<size_t>(given)]->utf8() + "'");
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
        list->values().assign(sp_ - extra, sp_);
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
                        throw runtimeError(Thrown::IllegalStateException, native->name + "() cannot be used in a parallel section: nothing waits there, and what the " +
                                           "whole program has one of is left alone");
                    guardNative(sp_ - argc, argc);
                }
                nativeBase_ = sp_ - argc - 1;
                pendingTypeArgs_ = nullptr;
                boxAll(sp_ - argc, sp_);  // the C++ is given boxes, never a place in a list
                pendingDestination_ = Value::null();
                Value result = native->fn(*this, argc, sp_ - argc);
                if (result.kind == ValueKind::Undefined) return true;  // it set a call going in its place (tailCallOn)
                sp_ -= argc + 1;
                *sp_++ = result;
                return false;
            }

            case ObjType::Class:
                return construct(asClass(callee), argc, packed);

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
                // What changes its receiver: not one that is another core's, or frozen (D26).
                if (method->mutates && bound->receiver.isObj()) guardWrite(bound->receiver.as.obj);
                if (coreId_ != 0) {
                    guardRead(bound->receiver);
                    guardBand(bound->receiver, method->name);
                    guardNative(sp_ - argc, argc);
                }
                nativeBase_ = sp_ - argc - 1;
                pendingTypeArgs_ = nullptr;
                if (!method->takesPlaces) boxAll(sp_ - argc, sp_);
                Value result = method->fn(*this, bound->receiver, argc, sp_ - argc);
                sp_ -= argc + 1;
                *sp_++ = result;
                return false;
            }

            default:
                break;
        }
    }
    throw runtimeError(Thrown::UnsupportedOperationException, "cannot call a value of type " + typeName(callee));
}

// A call with arguments passed by name. The stack holds the callee, the positional
// arguments, then a (name, value) pair for each named one. They are rearranged into the
// callee's parameter order, leaving the "not passed" mark for parameters that have a default,
// and the call then proceeds as usual.
bool VM::callNamed(int positional, int named) {
    Value* first = sp_ - positional - 2 * named;
    Value callee = first[-1];

    // A lambda written after a call to a built-in function is simply its last argument.
    if (named == 1 && sp_[-2].isString() && asString(sp_[-2])->utf8().empty() &&
        (callee.isObjType(ObjType::Native) || callee.isObjType(ObjType::BoundNative))) {
        sp_[-2] = sp_[-1];
        sp_--;
        return callValue(callee, positional + 1);
    }

    // The function whose parameter names the arguments are matched against.
    const FunctionProto* proto = nullptr;
    auto display = [&]() -> std::string {
        return callee.isObjType(ObjType::Class) ? asClass(callee)->name->utf8() : proto->name;
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
                if (cls->init == nullptr) throw runtimeError(Thrown::IllegalArgumentException, cls->name->utf8() + "() takes no arguments");
                proto = cls->init->proto;
                break;
            }
            case ObjType::Native:
                if (!asNative(callee)->params.empty()) return callNativeNamed(positional, named);
                throw runtimeError(Thrown::UnsupportedOperationException, "built-in function " + asNative(callee)->name + "() does not take named arguments");
            case ObjType::BoundNative:
                if (!asBoundNative(callee)->method->params.empty()) return callNativeNamed(positional, named);
                throw runtimeError(Thrown::UnsupportedOperationException, "built-in method " + asBoundNative(callee)->method->name +
                                   "() does not take named arguments");
            default:
                break;
        }
    }
    if (proto == nullptr) throw runtimeError(Thrown::UnsupportedOperationException, "cannot call a value of type " + typeName(callee));

    int arity = proto->arity;
    if (proto->paramNames.size() != static_cast<size_t>(arity))
        throw runtimeError(Thrown::UnsupportedOperationException, display() + "() does not take named arguments");
    const int vararg = proto->varargIndex;
    if (positional > arity && vararg < 0) checkArity(display(), proto->requiredArgs(), arity, positional + named);

    // The common case of a lambda after the call: every parameter before the last was given,
    // so the lambda is simply the next argument.
    if (named == 1 && vararg < 0 && positional + 1 == arity && asString(sp_[-2])->utf8().empty()) {
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
        list->values().assign(first + vararg, first + positional);
        account(static_cast<size_t>(positional - vararg) * sizeof(Value));
        arranged[static_cast<size_t>(vararg)] = Value::object(list);
    }

    const Value* pair = first + positional;
    for (int i = 0; i < named; i++, pair += 2) {
        if (!pair[0].isString()) throw runtimeError(Thrown::InternalError, "invalid call");
        const ObjString* name = asString(pair[0]);  // interned, like the parameter names

        int index = -1;
        if (name->utf8().empty()) {
            // The lambda written after the call: it is the last parameter (see callTrailing).
            if (arity == 0) throw runtimeError(Thrown::UnsupportedOperationException, display() + "() takes no arguments, so no lambda can follow the call");
            index = arity - 1;
            name = proto->paramNames[static_cast<size_t>(index)];
        }
        for (int p = 0; index < 0 && p < arity; p++) {
            if (proto->paramNames[static_cast<size_t>(p)] == name) index = p;
        }
        if (index < 0) throw runtimeError(Thrown::IllegalArgumentException, display() + "() has no parameter named '" + name->utf8() + "'");
        if (arranged[static_cast<size_t>(index)].kind != ValueKind::Undefined)
            throw runtimeError(Thrown::IllegalArgumentException, "argument '" + name->utf8() + "' of " + display() + "() is given more than once");
        arranged[static_cast<size_t>(index)] = pair[1];
    }

    if (vararg >= 0) {
        Value& gathered = arranged[static_cast<size_t>(vararg)];
        if (gathered.kind == ValueKind::Undefined) {
            gathered = Value::object(alloc<ObjList>());
        } else if (!gathered.isObjType(ObjType::List)) {
            throw runtimeError(Thrown::IllegalArgumentException, "argument '" + proto->paramNames[static_cast<size_t>(vararg)]->utf8() + "' of " + display() +
                               "() is a vararg parameter: by name it takes a list, not a value of type " + typeName(gathered));
        }
    }

    // Only parameters with a default value may be left out.
    for (int i = 0; i < proto->requiredArgs(); i++)
        if (arranged[static_cast<size_t>(i)].kind == ValueKind::Undefined)
            throw runtimeError(Thrown::IllegalArgumentException, display() + "() is missing argument '" + proto->paramNames[static_cast<size_t>(i)]->utf8() + "'");

    if (!roomFor(first + arity)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");
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
    const std::vector<std::string>* best = nullptr;
    if (callee.isObjType(ObjType::BoundNative) && asBoundNative(callee)->method->forms.size() > 1) {
        // Declared in several forms: the smallest that has a parameter for each name given.
        for (const std::vector<std::string>& form : asBoundNative(callee)->method->forms) {
            bool fits = static_cast<int>(form.size()) >= positional;
            const Value* pair = first + positional;
            for (int i = 0; fits && i < named; i++, pair += 2) {
                const std::string& name = pair[0].isString() ? asString(pair[0])->utf8() : std::string();
                fits = name.empty() ? !form.empty() : std::find(form.begin(), form.end(), name) != form.end();
            }
            if (fits && (best == nullptr || form.size() < best->size())) best = &form;
        }
    }
    const Native chosen{best != nullptr ? *best : native.params, native.name, native.minArgs, native.maxArgs};
    const Native* method = &chosen;
    const int count = static_cast<int>(method->params.size());
    if (positional > count) checkArity(method->name, method->minArgs, method->maxArgs, positional + named);

    std::vector<Value>& arranged = namedScratch_;
    arranged.assign(static_cast<size_t>(count), Value::undefined());
    for (int i = 0; i < positional; i++) arranged[static_cast<size_t>(i)] = first[i];
    int given = positional;
    const Value* pair = first + positional;
    for (int i = 0; i < named; i++, pair += 2) {
        if (!pair[0].isString()) throw runtimeError(Thrown::InternalError, "invalid call");
        const std::string& name = asString(pair[0])->utf8();
        int index = name.empty() ? count - 1 : -1;  // a lambda after the call is the last parameter
        for (int p = 0; index < 0 && p < count; p++)
            if (method->params[static_cast<size_t>(p)] == name) index = p;
        if (index < 0) throw runtimeError(Thrown::IllegalArgumentException, method->name + "() has no parameter named '" + name + "'");
        if (arranged[static_cast<size_t>(index)].kind != ValueKind::Undefined)
            throw runtimeError(Thrown::IllegalArgumentException, "argument '" + method->params[static_cast<size_t>(index)] + "' of " + method->name +
                               "() is given more than once");
        arranged[static_cast<size_t>(index)] = pair[1];
        if (index + 1 > given) given = index + 1;
    }

    if (!roomFor(first + given)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");
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
    const int count = static_cast<int>(list->size());
    if (!roomFor(sp_ + count + 1)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");

    std::copy_backward(slot + 1, sp_, sp_ + count - 1);
    for (int i = 0; i < count; i++) slot[i] = listGet(list, static_cast<size_t>(i));
    sp_ += count - 1;
    if (named > 0) return callNamed(count, named);
    return callValue(sp_[-count - 1], count);
}

const NativeMethod* VM::findNativeMethod(Value receiver, const ObjString* name) const {
    BuiltinType type;
    if (receiver.isNumber()) {
        type = BuiltinType::Number;
    } else if (receiver.isChar()) {
        type = BuiltinType::Char;
    } else if (receiver.isByte()) {
        type = BuiltinType::Byte;
    } else if (receiver.isFromEnd()) {
        type = BuiltinType::Index;
    } else if (!receiver.isObj()) {
        return nullptr;
    } else {
        switch (receiver.as.obj->type) {
            case ObjType::String: type = BuiltinType::String; break;
            case ObjType::List: type = BuiltinType::List; break;
            case ObjType::Map: type = BuiltinType::Map; break;
            case ObjType::Set: type = BuiltinType::Set; break;
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
    if (receiver.isSmall()) {
        // A method run on a value kept in the value itself (M6.6 part 5): `this` is the value.
        ObjClass* cls = smallClassOf(receiver);
        if (const Value* method = cls->methods.find(name)) {
            if (trailing) callTrailing(asClosure(*method), argc);
            else callClosure(asClosure(*method), argc);
            return true;
        }
        receiver = sp_[-argc - 1] = boxSmall(receiver);  // a field, a property or an extension: on a box
    }
    if (receiver.isRef()) {
        // A method run on a value where it is kept (M6.6 part 4): `this` is the place.
        ObjClass* cls = refClass(receiver);
        if (const Value* method = cls->methods.find(name)) {
            if (trailing) callTrailing(asClosure(*method), argc);
            else callClosure(asClosure(*method), argc);
            return true;
        }
        Value field;
        if (refField(receiver, name, field)) {
            sp_[-argc - 1] = field;
            return trailing ? callValueTrailing(field, argc) : callValue(field, argc);
        }
        if (const Value* getter = cls->getters.find(name)) {
            Value callee = callWithReceiver(receiver, asClosure(*getter));
            sp_[-argc - 1] = callee;
            return trailing ? callValueTrailing(callee, argc) : callValue(callee, argc);
        }
        receiver = sp_[-argc - 1] = ownRef(receiver);  // an extension or the like: on a copy
    }
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
            if (Value field; instance->read(name, field)) {
                sp_[-argc - 1] = field;
                return trailing ? callValueTrailing(field, argc) : callValue(field, argc);
            }
            if (instance->cls->hasAccessors) {
                // obj.property(args): call whatever the getter of the property returns.
                if (const Value* getter = instance->cls->getters.find(name)) {
                    Value callee = callWithReceiver(receiver, asClosure(*getter));
                    sp_[-argc - 1] = callee;
                    return trailing ? callValueTrailing(callee, argc) : callValue(callee, argc);
                }
            }
            // What an extend brought to the class (D33).
            if (const Value* method = instance->cls->extendMethod(name)) {
                if (trailing) callTrailing(asClosure(*method), argc);
                else callClosure(asClosure(*method), argc);
                return true;
            }
            if (const Value* getter = instance->cls->extendGetter(name)) {
                Value callee = callWithReceiver(receiver, asClosure(*getter));
                sp_[-argc - 1] = callee;
                return trailing ? callValueTrailing(callee, argc) : callValue(callee, argc);
            }
            if (const Value* extension = findExtension(receiver, name)) {
                if (trailing) callTrailing(asClosure(*extension), argc);
                else callClosure(asClosure(*extension), argc);
                return true;
            }
            if (const int done = scopeFunction(name, argc)) return done == 2;
            throw runtimeError(Thrown::UnsupportedOperationException, "undefined method '" + name->utf8() + "' on " + instance->cls->name->utf8());
        }

        if (receiver.as.obj->type == ObjType::Value) {
            // A method of a value's type (M6.6 part 2), run on the value; or what a field or a
            // property holds, called.
            ObjValue* box = asValue(receiver);
            if (const Value* method = box->cls->methods.find(name)) {
                if (trailing) callTrailing(asClosure(*method), argc);
                else callClosure(asClosure(*method), argc);
                return true;
            }
            if (const int64_t at = valueFieldIndex(box, name); at >= 0) {
                const Value field = valueField(box, static_cast<size_t>(at));
                sp_[-argc - 1] = field;
                return trailing ? callValueTrailing(field, argc) : callValue(field, argc);
            }
            if (const Value* getter = box->cls->getters.find(name)) {
                Value callee = callWithReceiver(receiver, asClosure(*getter));
                sp_[-argc - 1] = callee;
                return trailing ? callValueTrailing(callee, argc) : callValue(callee, argc);
            }
            if (const Value* extension = findExtension(receiver, name)) {
                if (trailing) callTrailing(asClosure(*extension), argc);
                else callClosure(asClosure(*extension), argc);
                return true;
            }
            if (const int done = scopeFunction(name, argc)) return done == 2;
            throw runtimeError(Thrown::UnsupportedOperationException, "undefined method '" + name->utf8() + "' on " + box->cls->name->utf8());
        }

        if (receiver.as.obj->type == ObjType::Class) {
            ObjClass* cls = asClass(receiver);
            if (cls->staticInit != nullptr || (cls->super != nullptr && cls->super->staticInit != nullptr)) ensureClassInit(cls);
            if (const Value* member = cls->findStatic(name)) {
                sp_[-argc - 1] = *member;
                return trailing ? callValueTrailing(*member, argc) : callValue(*member, argc);
            }
            throw runtimeError(Thrown::UnsupportedOperationException, "undefined static method '" + name->utf8() + "' on class " + cls->name->utf8());
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
        throw runtimeError(Thrown::UnsupportedOperationException, "undefined method '" + name->utf8() + "' on a value of type " + typeName(receiver));
    }
    if (receiver.isObjType(ObjType::Range) && asRange(receiver)->fromEnd() && method->name != "of")
        throw rangeFromEnd(asRange(receiver), method->name + "()");
    checkArity(method->name, method->minArgs, method->maxArgs, argc);
    if (pendingEnums_ > 0) rejectPendingNames(sp_ - argc, argc);
    if (method->mutates && receiver.isObj()) guardWrite(receiver.as.obj);  // nor frozen (D26)
    if (coreId_ != 0) {
        guardBand(receiver, method->name);
        guardNative(sp_ - argc, argc);
    }
    nativeBase_ = sp_ - argc - 1;
    pendingTypeArgs_ = nullptr;
    if (!method->takesPlaces) boxAll(sp_ - argc, sp_);
    receiver = boxed(receiver);
    Value result = method->fn(*this, receiver, argc, sp_ - argc);
    sp_ -= argc + 1;
    *sp_++ = result;
    return false;
}

Value VM::tailCallOn(Value receiver, Value function) {
    if (!function.isObjType(ObjType::Closure)) throw runtimeError(Thrown::ClassCastException, "expected a function written in Astra, got a value of type " + typeName(function));
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
// block's result. (Written with a lambda they are the standard library's inline functions, copied in
// by the compiler, and never get here, D59.)
//
// The block is called as part of the program, not from inside this function, so that a task
// can be set aside in it. Returns 0 if `name` is not one of these, 1 if the result is on the
// stack, and 2 if a call has been set going.
int VM::scopeFunction(ObjString* name, int argc) {
    if (argc != 1) return 0;
    const std::string& n = name->utf8();
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
        if (!roomFor(sp_ + 1)) throw runtimeError(Thrown::StackOverflowError, "stack overflow");
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
    if (a.isSmall() && b.isSmall() && a.refIndex == b.refIndex && smallClassOf(a)->methods.find(names.opEq) == nullptr)
        return valuesEqual(a, b);  // field by field, with no box (M6.6 part 5)
    if (a.isRef() || b.isRef() || a.isSmall() || b.isSmall()) {
        TempRoot keepA(*this, a = boxed(a));
        TempRoot keepB(*this, b = boxed(b));
        return equals(a, b, depth);
    }
    if (!a.isObj() || !b.isObj()) return valuesEqual(a, b);
    if (a.as.obj == b.as.obj) return true;

    // In a parallel section another core's item is not looked into. Telling two objects apart
    // by which they are needs no looking, so only the cases below that do look ask.
    if (a.isObjType(ObjType::Value)) {
        // Two values of one value type are equal when their fields are (M6.6 part 2), unless
        // the type says what == means itself.
        if (const Value* method = asValue(a)->cls->methods.find(names.opEq)) {
            TempRoot bound(*this, Value::object(alloc<ObjBoundMethod>(a, asClosure(*method))));
            return call(bound.get(), 1, &b).truthy();
        }
        if (!b.isObjType(ObjType::Value) || depth >= kMaxEqualsDepth) return false;
        return valuesSame(asValue(a), asValue(b), depth);
    }
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
                Value left = Value::null(), right = Value::null();
                x->read(part, left);
                y->read(part, right);
                if (!equals(left, right, depth + 1)) return false;
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
            ObjList* x = asList(a);
            ObjList* y = asList(b);
            if (x->size() != y->size()) return false;
            const bool sameNarrow = x->store() == ListStore::Narrow && x->pack->narrow == y->pack->narrow && x->pack->narrow != Narrow::Float32;
            if (x->packed() && y->packed() && x->store() == y->store() && (x->store() == ListStore::Ints || x->store() == ListStore::Bools || sameNarrow) &&
                !x->pack->nullable && !y->pack->nullable)
                return x->pack->block == y->pack->block;  // ints and bools, equal where their bytes are
            // An element's own == may change either list, so the sizes are looked at again.
            for (size_t i = 0; i < x->size() && i < y->size(); i++) {
                const Value left = listGet(x, i);
                TempRoot keep(*this, left);
                const Value right = listGet(y, i);
                TempRoot keepToo(*this, right);
                if (!equals(left, right, depth + 1)) return false;
            }
            return x->size() == y->size();
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
            return x->start == y->start && x->end == y->end && x->step == y->step && x->inclusive == y->inclusive &&
                   x->startFromEnd == y->startFromEnd && x->endFromEnd == y->endFromEnd;
        }
        case ObjType::Set: {
            const ObjSet* x = asSet(a);
            const ObjSet* y = asSet(b);
            if (x->values != y->values) return false;
            if (x->values) {
                // A map's values (G22): the same values in the same order, as for lists.
                const std::vector<Value> xs = setItems(x);
                TempVector keepX(*this, const_cast<std::vector<Value>&>(xs));
                const std::vector<Value> ys = setItems(y);
                TempVector keepY(*this, const_cast<std::vector<Value>&>(ys));
                if (xs.size() != ys.size()) return false;
                for (size_t i = 0; i < xs.size(); i++)
                    if (!equals(xs[i], ys[i])) return false;
                return true;
            }
            // The same items, whatever their order.
            if (setSize(x) != setSize(y)) return false;
            const std::vector<Value> items = setItems(x);
            TempVector keep(*this, const_cast<std::vector<Value>&>(items));
            for (const Value& item : items)
                if (!setContains(y, item)) return false;
            return true;
        }
        default:
            return valuesEqual(a, b);
    }
}

// Calls an overloaded operator; the receiver and its operands are already on the stack in
// call layout, so this always pushes a frame.
void VM::invokeOperator(Value receiver, ObjString* name, int argc) {
    const ObjClass* cls = classOf(receiver);
    const Value* method = cls->methods.find(name);
    if (method == nullptr) {
        const std::string symbol = name == names.opNeg ? "-" : name == names.opBitNot ? "~" : name->utf8();
        throw runtimeError(Thrown::UnsupportedOperationException, cls->name->utf8() + " does not define operator '" + symbol + "'");
    }
    callClosure(asClosure(*method), argc);
}

// ---- Properties and indexing ----

Value VM::getProperty(Value obj, ObjString* name) {
    if (obj.isSmall()) {
        // A field of a value kept in the value itself (M6.6 part 5); anything else, on a box.
        const ObjClass* cls = smallClassOf(obj);
        if (const Value* at = cls->declaredIndex.find(name)) {
            const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[static_cast<size_t>(at->as.i)];
            return readValueField(smallBytes(obj) + field.offset, field);
        }
        obj = boxSmall(obj);
    }
    if (obj.isRef()) {
        Value read;
        if (refField(obj, name, read)) return read;
        obj = ownRef(obj);  // a method or property of it: on a copy that may be kept
    }
    if (obj.isObj()) {
        guardRead(obj.as.obj);
        switch (obj.as.obj->type) {
            case ObjType::Instance: {
                ObjInstance* instance = asInstance(obj);
                if (Value field; instance->read(name, field)) return fieldRead(field);
                if (const Value* method = instance->cls->methods.find(name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*method)));
                if (instance->cls->setters.find(name) != nullptr)
                    throw runtimeError(Thrown::IllegalStateException, "property '" + name->utf8() + "' of " + instance->cls->name->utf8() +
                                       " cannot be read: it has no getter");
                if (const Value* extension = findExtension(obj, name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*extension)));
                if (const Value* getter = instance->cls->extendGetter(name)) return callWithReceiver(obj, asClosure(*getter));  // D33
                if (const Value* method = instance->cls->extendMethod(name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*method)));
                if (const FieldDecl* decl = instance->declOf(name)) {
                    if (decl->payload == FieldDecl::NoPayload && instance->fields()[decl->slot].isLazy())
                        return forceLazy(instance->fields()[decl->slot], [&](Value v) { instance->fields()[decl->slot] = v; },
                                         "'" + name->utf8() + "' of " + instance->cls->name->utf8());
                    throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' of " + instance->cls->name->utf8() + " is read before it is given a value " +
                                       "(a lateinit field is read only once it has one)");
                }
                throw runtimeError(Thrown::UnsupportedOperationException, "undefined property '" + name->utf8() + "' on " + instance->cls->name->utf8());
            }
            case ObjType::Value: {
                ObjValue* box = asValue(obj);
                if (const int64_t at = valueFieldIndex(box, name); at >= 0) return valueField(box, static_cast<size_t>(at));
                if (const Value* method = box->cls->methods.find(name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*method)));
                if (const Value* getter = box->cls->getters.find(name)) return callWithReceiver(obj, asClosure(*getter));
                if (const Value* extension = findExtension(obj, name))
                    return Value::object(alloc<ObjBoundMethod>(obj, asClosure(*extension)));
                throw runtimeError(Thrown::UnsupportedOperationException, "undefined property '" + name->utf8() + "' on " + box->cls->name->utf8());
            }
            case ObjType::Class: {
                ObjClass* cls = asClass(obj);
                if (cls->staticInit != nullptr || (cls->super != nullptr && cls->super->staticInit != nullptr)) ensureClassInit(cls);
                if (const Value* member = cls->findStatic(name)) {
                    if (!member->isLazy()) return *member;
                    for (ObjClass* k = cls; k != nullptr; k = k->super)
                        if (k->statics.find(name) != nullptr)
                            return forceLazy(*member, [&](Value v) { k->statics.set(name, v); }, cls->name->utf8() + "." + name->utf8());
                }
                for (const ObjClass* k = cls; k != nullptr; k = k->super) {
                    if (const Value* getter = k->staticGetters.find(name)) return call(*getter, 0, nullptr);
                    if (k->staticSetters.find(name) != nullptr)
                        throw runtimeError(Thrown::IllegalStateException, "static property '" + name->utf8() + "' of " + k->name->utf8() +
                                           " cannot be read: it has no getter");
                }
                throw runtimeError(Thrown::UnsupportedOperationException, "undefined static member '" + name->utf8() + "' on class " + cls->name->utf8());
            }
            case ObjType::String:
                if (name == names.size) return Value::integer(static_cast<int64_t>(asString(obj)->length));
                if (name == names.indices)
                    return Value::object(alloc<ObjRange>(int64_t{0}, static_cast<int64_t>(asString(obj)->length), int64_t{1}, false));
                if (name == names.lastIndex) return Value::integer(static_cast<int64_t>(asString(obj)->length) - 1);
                break;
            case ObjType::List:
                if (name == names.size) return Value::integer(static_cast<int64_t>(asList(obj)->size()));
                if (name == names.indices)
                    return Value::object(alloc<ObjRange>(int64_t{0}, static_cast<int64_t>(asList(obj)->size()), int64_t{1}, false));
                if (name == names.lastIndex) return Value::integer(static_cast<int64_t>(asList(obj)->size()) - 1);
                if (name == names.itemType) return classValue(typeArgument(asList(obj)->type, 0));  // M6.6 part 1, D162
                break;
            case ObjType::Map:
                if (name == names.size) return Value::integer(static_cast<int64_t>(asMap(obj)->map.size()));
                if (name == names.keyType) return classValue(typeArgument(asMap(obj)->type, 0));
                if (name == names.valueType) return classValue(typeArgument(asMap(obj)->type, 1));
                if (name == names.keys) return mapView(asMap(obj), false);  // live views (D77)
                if (name == names.values) return mapValues(asMap(obj));  // a live view (G22)
                if (name == names.entries) return mapView(asMap(obj), true);
                break;
            case ObjType::Set:
                if (name == names.size) return Value::integer(static_cast<int64_t>(setSize(asSet(obj))));
                if (name == names.itemType) return classValue(typeArgument(asSet(obj)->type, 0));
                break;
            case ObjType::Range: {
                ObjRange* range = asRange(obj);
                if (name == names.start) return range->startFromEnd ? Value::fromEnd(range->start) : Value::integer(range->start);
                if (name == names.end) return range->endFromEnd ? Value::fromEnd(range->end) : Value::integer(range->end);
                if (range->fromEnd()) throw rangeFromEnd(range, name->utf8());
                if (name == names.step) return Value::integer(range->step);
                if (name == names.size) return Value::integer(range->count());
                // The numbers it starts and finishes on, when it holds any.
                if (name == names.first && range->count() > 0) return Value::integer(range->start);
                if (name == names.last && range->count() > 0)
                    return Value::integer(range->start + (range->count() - 1) * range->step);
                break;
            }
            case ObjType::Bytes:
                if (name == names.size) return Value::integer(static_cast<int64_t>(asBytes(obj)->data.size()));
                break;
            default:
                break;
        }
    }

    if (obj.isChar() && name == names.code) return Value::integer(obj.as.i);  // the character's number (D24)
    // An Index (D146): ^k, or an int from the start.
    if ((obj.isFromEnd() || obj.isInt()) && name == names.fromEnd) return Value::boolean(obj.isFromEnd());
    if ((obj.isFromEnd() || obj.isInt()) && name == names.value) return Value::integer(obj.as.i);
    if (obj.isInt() && name == names.character) {
        // The character numbered n (D142).
        if (!isCharacter(obj.as.i))
            throw runtimeError(Thrown::IllegalArgumentException, std::to_string(obj.as.i) + " is not a character's number: characters are numbered 0 to 1114111, leaving out 55296 to 57343");
        return Value::character(static_cast<uint32_t>(obj.as.i));
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
    throw runtimeError(Thrown::UnsupportedOperationException, "undefined property '" + name->utf8() + "' on a value of type " + typeName(obj));
}

// ---- Value types (M6.6 part 2, value-lists.html) ----

// The class a class-typed field is declared with, found the first time it is needed (the
// class may be declared later in the program than the field).
ObjClass* VM::fieldClass(FieldDecl& field) {
    if ((field.type & kTypeMask) != kTypeInstance) return nullptr;
    if (field.typeClass == nullptr) {
        Value resolved = call(Value::object(field.typeThunk), 0, nullptr);
        if (!resolved.isObjType(ObjType::Class))
            throw runtimeError(Thrown::ClassCastException, "a type must be a class or a built-in type, not a value of type " + typeName(resolved));
        // (In a parallel section several cores may find this out at once: they all find the same class.)
        field.typeClass = asClass(resolved);
    }
    return field.typeClass;
}

// A value type's fixed layout: each field at an offset aligned to its width, in the order the
// class declares them, a value type held by another in place. Worked out when the first value
// of the type is made, when every class its fields name exists; then never again.
const ValueLayout& VM::layoutOf(ObjClass* cls) {
    if (const ValueLayout* known = cls->layout.load(std::memory_order_acquire)) return *known;
    // The value types being laid out, to refuse one that holds itself.
    static thread_local std::vector<const ObjClass*> laying;
    if (std::find(laying.begin(), laying.end(), cls) != laying.end())
        throw runtimeError(Thrown::IllegalStateException, "value " + cls->name->utf8() + " holds itself, and would have no end: make the field nullable, " +
                           cls->name->utf8() + "?, which keeps it in a box");
    laying.push_back(cls);
    struct Done {
        std::vector<const ObjClass*>& list;
        ~Done() { list.pop_back(); }
    } done{laying};

    auto made = std::make_unique<ValueLayout>();
    uint32_t at = 0;
    auto place = [&](uint32_t size, uint32_t align) {
        at = (at + align - 1) / align * align;
        made->align = std::max(made->align, align);
        const uint32_t offset = at;
        at += size;
        return offset;
    };
    for (FieldDecl& declared : cls->declared) {
        ValueField field;
        field.name = declared.name;
        field.type = static_cast<uint8_t>(declared.type);
        const uint8_t id = declared.type & kTypeMask;
        const bool nullable = (declared.type & kTypeNullable) != 0;
        if (declared.type == 0) {
            field.kind = ValueField::Any;
        } else if (!nullable && id == kTypeInt) {
            field.kind = ValueField::Int;
        } else if (!nullable && id == kTypeFloat) {
            field.kind = ValueField::Float;
        } else if (!nullable && id == kTypeBool) {
            field.kind = ValueField::Bool;
        } else if (!nullable && narrowOf(id) != Narrow::None) {
            field.kind = ValueField::Narrow;  // a byte, a char or a storage width (D148)
            field.narrow = narrowOf(id);
        } else if (id == kTypeInstance) {
            field.cls = fieldClass(declared);
            field.kind = !nullable && field.cls->isValue ? ValueField::Nested : ValueField::Object;
        } else if (id == kTypeString || id == kTypeList || id == kTypeMap || id == kTypeRange || id == kTypeBytes ||
                   id == kTypeFunction || id == kTypeType || id == kTypeSet || id == kTypeFrozenList || id == kTypeFrozenMap || id == kTypeFrozenSet ||
                   id == kTypeValues) {
            field.kind = ValueField::Object;  // null is a null pointer, where the type takes it
        } else {
            field.kind = ValueField::Any;  // Number, and an int, float or bool that may be null
        }
        switch (field.kind) {
            case ValueField::Int:
            case ValueField::Float: field.offset = place(8, 8); break;
            case ValueField::Bool: field.offset = place(1, 1); break;
            case ValueField::Narrow: field.offset = place(narrowSize(field.narrow), narrowSize(field.narrow)); break;
            case ValueField::Object:
                field.offset = place(8, 8);
                made->objects.push_back(field.offset);
                break;
            case ValueField::Any:
                field.offset = place(sizeof(Value), 8);
                made->values.push_back(field.offset);
                break;
            case ValueField::Nested: {
                const ValueLayout& inner = layoutOf(field.cls);
                field.offset = place(inner.size, inner.align);
                for (uint32_t offset : inner.objects) made->objects.push_back(field.offset + offset);
                for (uint32_t offset : inner.values) made->values.push_back(field.offset + offset);
                break;
            }
        }
        made->fields.push_back(field);
    }
    made->size = (at + made->align - 1) / made->align * made->align;

    // Two cores may lay a type out at once; the first kept is kept, and the same.
    VM& root = root_ != nullptr ? *root_ : *this;
    std::lock_guard<std::mutex> hold(root.internMutex_);
    if (const ValueLayout* known = cls->layout.load(std::memory_order_acquire)) return *known;
    // At most 8 bytes, holding no object: kept in the value itself, with no box (M6.6 part 5).
    if (made->size <= 8 && made->objects.empty() && made->values.empty()) {
        static std::atomic<uint32_t> nextSmall{1};
        const uint32_t id = nextSmall.fetch_add(1, std::memory_order_relaxed);
        if (id < kMaxSmallClasses) {
            cls->smallId = id;
            smallClasses[id].store(cls, std::memory_order_release);
            root.smallKept_.push_back(cls);
        }
    }
    cls->layoutOwned = std::move(made);
    cls->layout.store(cls->layoutOwned.get(), std::memory_order_release);
    return *cls->layoutOwned;
}

#ifdef ASTRA_OP_COUNTS
// In the build that counts instructions: how many values were put in boxes of their own (M6.6
// part 4), shown when the program ends.
namespace {
struct BoxCount {
    std::atomic<uint64_t> made{0};
    ~BoxCount() {
        std::fprintf(stderr, "value boxes made: %llu\n", static_cast<unsigned long long>(made.load()));
        std::fprintf(stderr, "objects made, boxes included: %llu\n", static_cast<unsigned long long>(VM::objectsMade.load() + made.load()));
    }
} boxCount;
}  // namespace
std::atomic<uint64_t> VM::objectsMade{0};
#endif

// A box for a value of `cls`, its fields zero: 0, 0.0, false and null.
ObjValue* VM::newValue(ObjClass* cls) {
#ifdef ASTRA_OP_COUNTS
    boxCount.made.fetch_add(1, std::memory_order_relaxed);
#endif
    const ValueLayout& layout = layoutOf(cls);
    ObjValue* box = new (layout.size) ObjValue(cls);
    std::memset(box->data(), 0, layout.size);
    box->core = 0;  // every core's to read: it never changes once made
    box->next = objects_;
    objects_ = box;
    bytesAllocated_ += sizeof(ObjValue) + layout.size;
    return box;
}

// The value of a field kept at `at` in a value's layout.
Value VM::readValueField(const uint8_t* at, const ValueField& field) {
    switch (field.kind) {
        case ValueField::Int: {
            int64_t i;
            std::memcpy(&i, at, sizeof i);
            return Value::integer(i);
        }
        case ValueField::Float: {
            double d;
            std::memcpy(&d, at, sizeof d);
            return Value::number(d);
        }
        case ValueField::Bool: return Value::boolean(*at != 0);
        case ValueField::Narrow: return readNarrow(field.narrow, at);
        case ValueField::Object: {
            Obj* obj;
            std::memcpy(&obj, at, sizeof obj);
            return obj != nullptr ? Value::object(obj) : Value::null();
        }
        case ValueField::Any: {
            Value v;
            std::memcpy(&v, at, sizeof v);
            return v;
        }
        case ValueField::Nested: {
            if (field.cls->smallId != 0) return smallFrom(field.cls, at, field.cls->layout.load(std::memory_order_relaxed)->size);
            // A value held in another, read on its own: a box of its own, a copy (M6.6 part 2).
            ObjValue* box = newValue(field.cls);
            std::memcpy(box->data(), at, layoutOf(field.cls).size);
            return Value::object(box);
        }
    }
    return Value::null();
}

// What an int that does not fit a storage width is told (D148).
std::string VM::storageMismatch(Narrow narrow, Value v) {
    int64_t low, high;
    narrowRange(narrow, low, high);
    static const char* const kinds[] = {"", "byte", "char", "int16", "int32", "uint16", "uint32", "float32"};
    static const char* const wrapped[] = {"", "toByteWrapped()", "", "toInt16Wrapped()", "toInt32Wrapped()", "toUint16Wrapped()", "toUint32Wrapped()", ""};
    const size_t at = static_cast<size_t>(narrow);
    return std::to_string(v.as.i) + " does not fit in " + (narrow == Narrow::Int16 || narrow == Narrow::Int32 ? "an " : "a ") + kinds[at] +
           ", which holds " + std::to_string(low) + " to " + std::to_string(high) + "; " + wrapped[at] + " keeps the low " +
           std::to_string(narrowSize(narrow) * 8) + " bits";
}

void VM::checkRange(uint8_t type, Value v, const std::string& where) {
    if (isIntStorage(type & kTypeMask) && v.isInt()) throw runtimeError(Thrown::ClassCastException, storageMismatch(narrowOf(type & kTypeMask), v) + " (" + where + ")");
}

// Writes `value` into a field kept at `at`, checked against the field's declared type as an
// object's field is.
void VM::writeValueField(uint8_t* at, const ValueField& field, const ObjClass* owner, Value value) {
    if (field.kind == ValueField::Narrow && value.isInt() && field.narrow != Narrow::Byte && field.narrow != Narrow::Char &&
        !fitsNarrow(field.narrow, value))
        throw runtimeError(Thrown::ClassCastException, storageMismatch(field.narrow, value) + " (field '" + field.name->utf8() + "' of " + owner->name->utf8() + ")");
    if (field.type != 0 && !(value.isNull() && (field.type & kTypeNullable) != 0) && (!matchesType(value, field.type, field.cls) && !widenedByte(value, field.type)))
        throw runtimeError(Thrown::ClassCastException, "cannot assign a value of type " + typeName(value) + " to field '" + field.name->utf8() + "' of " +
                           owner->name->utf8() + ", which is declared as " + declaredTypeText(field.type, field.cls));
    if (value.isRef() && field.kind == ValueField::Nested) {
        std::memmove(at, refAt(value, false), layoutOf(field.cls).size);  // the same bytes, perhaps
        return;
    }
    value = own(value);  // a value kept in another is kept, as in a box
    switch (field.kind) {
        case ValueField::Int: std::memcpy(at, &value.as.i, sizeof value.as.i); break;
        case ValueField::Float: std::memcpy(at, &value.as.d, sizeof value.as.d); break;
        case ValueField::Bool: *at = value.as.b ? 1 : 0; break;
        case ValueField::Narrow:
            if (field.narrow == Narrow::Byte && value.isInt()) value = Value::byte(static_cast<uint8_t>(value.as.i));
            writeNarrow(field.narrow, at, value);
            break;
        case ValueField::Object: {
            Obj* obj = value.isObj() ? value.as.obj : nullptr;
            std::memcpy(at, &obj, sizeof obj);
            break;
        }
        case ValueField::Any: std::memcpy(at, &value, sizeof value); break;
        case ValueField::Nested:
            std::memcpy(at, value.isSmall() ? smallBytes(value) : asValue(value)->data(), layoutOf(field.cls).size);
            break;
    }
}

inline int operandU16(const uint8_t* at);  // below, with the interpreter

// A value held in another, read with one of its own fields at once, as in `s.pref.width`: the
// instruction after (a GetProp or GetOwn at `next`) is done here as well, and the field read
// where it is, with no copy made of the value that holds it. `at` is where `field` is, a
// Nested one. Gives where the next instruction to run is, and the value read in `out`; a copy
// of the held value where the instruction after is not such a read.
const uint8_t* VM::readThrough(const uint8_t* at, const ValueField* field, const uint8_t* next, const Value* consts, Value& out) {
    for (;;) {
        const Op op = static_cast<Op>(*next);
        if (op != Op::GetProp && op != Op::GetOwn && op != Op::GetPropRef && op != Op::GetOwnRef) break;
        const ObjString* name = asString(consts[operandU16(next + 1)]);
        // A name some class keeps from being read, and this value's class among them: whether it
        // may be read is told by the instruction itself. (One only kept from being assigned, as a
        // `private set` width elsewhere is, or kept by other classes only, is read here.)
        if ((name->restricted & 1) != 0 && (field->cls->accessBits >> (name->hash & 63) & 1) != 0) break;
        const Value* index = field->cls->declaredIndex.find(name);
        if (index == nullptr) break;
        const ValueField& inner = layoutOf(field->cls).fields[static_cast<size_t>(index->as.i)];
        next += 5;  // the instruction and its two operands
        if (inner.kind != ValueField::Nested) {
            out = readValueField(at + inner.offset, inner);
            return next;
        }
        at += inner.offset;
        field = &inner;
    }
    out = readValueField(at, *field);
    return next;
}

// Field `name` of a value, or -1 if its type has no such field.
int64_t VM::valueFieldIndex(const ObjValue* box, const ObjString* name) const {
    const Value* at = box->cls->declaredIndex.find(name);
    return at != nullptr ? at->as.i : -1;
}

Value VM::valueField(const ObjValue* box, size_t index) {
    const ValueField& field = layoutOf(box->cls).fields[index];
    return readValueField(box->data() + field.offset, field);
}

// Gives a field of a value being made its value; a value once made never changes.
void VM::setValueField(ObjValue* box, ObjString* name, Value value) {
    const int64_t index = valueFieldIndex(box, name);
    if (index < 0)
        throw runtimeError(Thrown::UnsupportedOperationException, box->cls->name->utf8() + " has no field '" + name->utf8() + "'; fields must be declared in it with val or var");
    if (!box->constructing)
        throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' of value " + box->cls->name->utf8() +
                           " cannot be given a new value: a value never changes once it is made");
    const ValueField& field = layoutOf(box->cls).fields[static_cast<size_t>(index)];
    writeValueField(box->data() + field.offset, field, box->cls, value);
}

// Whether two values of one value type hold the same, field by field, with == for each.
bool VM::valuesSame(const ObjValue* a, const ObjValue* b, int depth) {
    if (a->cls != b->cls) return false;
    const ValueLayout& layout = layoutOf(a->cls);
    for (size_t i = 0; i < layout.fields.size(); i++) {
        const ValueField& field = layout.fields[i];
        switch (field.kind) {
            case ValueField::Int:
            case ValueField::Bool:
                if (std::memcmp(a->data() + field.offset, b->data() + field.offset, field.kind == ValueField::Int ? 8 : 1) != 0)
                    return false;
                break;
            case ValueField::Float: {
                double x, y;
                std::memcpy(&x, a->data() + field.offset, sizeof x);
                std::memcpy(&y, b->data() + field.offset, sizeof y);
                if (!(x == y)) return false;  // -0.0 is 0.0, as == says
                break;
            }
            default: {
                const Value x = readValueField(a->data() + field.offset, field);
                TempRoot keep(*this, x);
                const Value y = readValueField(b->data() + field.offset, field);
                TempRoot keepToo(*this, y);
                if (!equals(x, y, depth + 1)) return false;
            }
        }
    }
    return true;
}

// ---- Lists (M6.6 part 3, value-lists.html) ----
//
// A list keeps its items as 16-byte values, or, when it was made to hold values (List<int>,
// List<float>, List<bool>, a value type's, and the nullable ones of those), packed one after
// another at their own widths, a bit per item for null beside them where the type takes null.
// Everything that reads or changes a list's items goes through these, or works on `block`
// itself where it knows the store.

namespace {
bool nullAt(const ObjList* list, size_t i) { return (list->pack->nulls[i / 64] >> (i % 64) & 1) != 0; }
void setNullAt(ObjList* list, size_t i, bool null) {
    const uint64_t bit = uint64_t{1} << (i % 64);
    if (null) list->pack->nulls[i / 64] |= bit;
    else list->pack->nulls[i / 64] &= ~bit;
}
}  // namespace

// How a list made to hold `item` keeps its items, and how wide each is.
ListStore VM::storeFor(const ObjTypeInfo* item, uint32_t& width, ObjClass*& itemClass) {
    itemClass = nullptr;
    switch (item->id) {
        case kTypeInt: width = 8; return ListStore::Ints;
        case kTypeFloat: width = 8; return ListStore::Floats;
        case kTypeBool: width = 1; return ListStore::Bools;
        case kTypeByte:
        case kTypeChar:
        case kTypeInt16:
        case kTypeInt32:
        case kTypeUint16:
        case kTypeUint32:
        case kTypeFloat32: width = narrowSize(narrowOf(item->id)); return ListStore::Narrow;
        case kTypeInstance:
            if (item->cls != nullptr && item->cls->isValue) {
                itemClass = item->cls;
                width = layoutOf(item->cls).size;
                return ListStore::Items;
            }
            break;
        default: break;
    }
    width = sizeof(Value);
    return ListStore::Values;
}

// Gives a list the type it was made to hold, where it has none yet, and from then on keeps its
// items as that type says: packed, for a list of values.
void VM::typeList(ObjList* list, ObjTypeInfo* type) {
    if (list->type != nullptr || type == nullptr || type->id != kTypeList || type->args.size() != 1) return;
    list->type = type;
    if (type->packs.load(std::memory_order_relaxed) == 0) return;  // as most lists: 16-byte values
    uint32_t width = 0;
    ObjClass* itemClass = nullptr;
    const ListStore store = storeFor(type->args[0], width, itemClass);
    type->packs.store(store == ListStore::Values ? 0 : 1, std::memory_order_relaxed);
    if (store == ListStore::Values) return;
    std::vector<Value> items = std::move(list->values());
    list->values().clear();
    list->values().shrink_to_fit();
    list->pack = std::make_unique<PackedItems>();
    list->pack->store = store;
    list->pack->width = width;
    list->pack->itemClass = itemClass;
    list->pack->narrow = store == ListStore::Narrow ? narrowOf(type->args[0]->id) : Narrow::None;
    list->pack->nullable = type->args[0]->nullable;
    list->pack->count = 0;
    // The items it was made with, checked and packed: a list that cannot hold one of them is
    // an error where it is made.
    TempRoot keep(*this, Value::object(list));
    listInsert(list, 0, items.data(), items.size());
}

uint8_t* VM::itemAt(ObjList* list, size_t i) { return list->pack->block.data() + i * list->pack->width; }

// Item `i` of a packed list (listGet takes the others itself). A value type's item is copied into a
// box of its own.
Value VM::packedGet(ObjList* list, size_t i) {
    if (list == bandObject_ && (i < bandFrom_ || i >= bandTo_)) [[unlikely]] throw outsideBand(i);
    if (list->pack->nullable && nullAt(list, i)) return Value::null();
    const uint8_t* at = itemAt(list, i);
    switch (list->pack->store) {
        case ListStore::Ints: {
            int64_t v;
            std::memcpy(&v, at, sizeof v);
            return Value::integer(v);
        }
        case ListStore::Floats: {
            double v;
            std::memcpy(&v, at, sizeof v);
            return Value::number(v);
        }
        case ListStore::Bools: return Value::boolean(*at != 0);
        case ListStore::Narrow: return readNarrow(list->pack->narrow, at);
        case ListStore::Items: {
            if (list->pack->itemClass->smallId != 0) return smallFrom(list->pack->itemClass, at, list->pack->width);
            ObjValue* box = newValue(list->pack->itemClass);
            std::memcpy(box->data(), at, list->pack->width);
            return Value::object(box);
        }
        default: return Value::null();
    }
}

// Whether `v` may be an item of a packed list; an error saying why not, where it may not.
void VM::checkItem(const ObjList* list, Value v) {
    if (v.isNull() && list->pack->nullable) return;
    // A byte goes where an int does: kept as its number.
    if (v.isByte() && (list->pack->store == ListStore::Ints ||
                       (list->pack->store == ListStore::Narrow && list->pack->narrow != Narrow::Byte && list->pack->narrow != Narrow::Char)))
        v = Value::integer(v.as.i);
    bool fits = false;
    switch (list->pack->store) {
        case ListStore::Ints: fits = v.isInt(); break;
        case ListStore::Floats: fits = v.isFloat(); break;
        case ListStore::Bools: fits = v.isBool(); break;
        case ListStore::Narrow:
            if (v.isInt() && list->pack->narrow != Narrow::Byte && list->pack->narrow != Narrow::Char && !fitsNarrow(list->pack->narrow, v))
                throw runtimeError(Thrown::ClassCastException, storageMismatch(list->pack->narrow, v));
            fits = fitsNarrow(list->pack->narrow, v);
            break;
        case ListStore::Items:
            fits = (v.isObjType(ObjType::Value) && asValue(v)->cls == list->pack->itemClass) || (v.isRef() && refClass(v) == list->pack->itemClass) ||
                   (v.isSmall() && smallClassOf(v) == list->pack->itemClass);
            break;
        default: fits = true; break;
    }
    if (!fits)
        throw runtimeError(Thrown::ClassCastException, "cannot put a value of type " + typeName(v) + " in a " + typeInfoText(list->type) +
                           ", which keeps only what it was made to hold");
}

// Writes `v`, already checked, as item `i` of a packed list.
void VM::writeItem(ObjList* list, size_t i, Value v) {
    uint8_t* at = itemAt(list, i);
    if (list->pack->nullable) setNullAt(list, i, v.isNull());
    if (v.isNull()) {
        std::memset(at, 0, list->pack->width);
        return;
    }
    switch (list->pack->store) {
        case ListStore::Ints: std::memcpy(at, &v.as.i, 8); break;
        case ListStore::Floats: std::memcpy(at, &v.as.d, 8); break;
        case ListStore::Bools: *at = v.as.b ? 1 : 0; break;
        case ListStore::Narrow: writeNarrow(list->pack->narrow, at, v); break;
        case ListStore::Items:
            std::memmove(at, v.isRef() ? refAt(v, false) : v.isSmall() ? smallBytes(v) : asValue(v)->data(), list->pack->width);
            break;
        default: break;
    }
}

void VM::listSet(ObjList* list, size_t i, Value v) {
    if (!list->packed()) {
        list->values()[i] = own(v);
        return;
    }
    checkItem(list, v);
    writeItem(list, i, v);
}

// Puts `n` items at position `at`, moving those from there on along.
void VM::listInsert(ObjList* list, size_t at, const Value* items, size_t n) {
    if (!list->packed()) {
        std::vector<Value>& values = list->values();
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(at), items, items + n);
        for (size_t k = at; k < at + n; k++) values[k] = own(values[k]);
        account(n * sizeof(Value));
        return;
    }
    for (size_t k = 0; k < n; k++) checkItem(list, items[k]);  // all of them, before anything changes
    const size_t width = list->pack->width;
    const size_t old = list->pack->count;
    const size_t before = list->pack->block.capacity();
    list->pack->block.resize((old + n) * width);
    uint8_t* base = list->pack->block.data();
    std::memmove(base + (at + n) * width, base + at * width, (old - at) * width);
    if (list->pack->nullable) {
        list->pack->nulls.resize((old + n + 63) / 64, 0);
        for (size_t i = old; i-- > at;) setNullAt(list, i + n, nullAt(list, i));
    }
    list->pack->count = old + n;
    for (size_t k = 0; k < n; k++) writeItem(list, at + k, items[k]);
    if (list->pack->block.capacity() > before) account(list->pack->block.capacity() - before);
}

void VM::listAdd(ObjList* list, Value v) { listInsert(list, list->size(), &v, 1); }

// Takes `n` items away from position `at`, moving those after it back.
void VM::listErase(ObjList* list, size_t at, size_t n) {
    if (!list->packed()) {
        std::vector<Value>& values = list->values();
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(at), values.begin() + static_cast<std::ptrdiff_t>(at + n));
        return;
    }
    const size_t width = list->pack->width;
    uint8_t* base = list->pack->block.data();
    std::memmove(base + at * width, base + (at + n) * width, (list->pack->count - at - n) * width);
    if (list->pack->nullable)
        for (size_t i = at; i + n < list->pack->count; i++) setNullAt(list, i, nullAt(list, i + n));
    list->pack->count -= n;
    list->pack->block.resize(list->pack->count * width);
    if (list->pack->nullable) list->pack->nulls.resize((list->pack->count + 63) / 64);
}

void VM::listClear(ObjList* list) {
    if (!list->packed()) {
        list->values().clear();
        return;
    }
    list->pack->count = 0;
    list->pack->block.clear();
    list->pack->nulls.clear();
}

// The items as 16-byte values, for what reads them so; the list itself is as it was.
std::vector<Value> VM::listCopy(ObjList* list) {
    if (!list->packed()) return list->values();
    std::vector<Value> out;
    out.reserve(list->pack->count);
    for (size_t i = 0; i < list->pack->count; i++) out.push_back(listGet(list, i));
    return out;
}

// A new list of the same type and store as `like`, with nothing in it.
ObjList* VM::listLike(const ObjList* like) {
    ObjList* list = alloc<ObjList>();
    list->type = like->type;
    if (like->packed()) {
        list->pack = std::make_unique<PackedItems>();
        list->pack->store = like->pack->store;
        list->pack->width = like->pack->width;
        list->pack->itemClass = like->pack->itemClass;
        list->pack->narrow = like->pack->narrow;   // what each item of a narrow list is
        list->pack->nullable = like->pack->nullable;
    }
    return list;
}

// `count` items of `from`, from position `start`, put at the end of `to`, both of one store:
// packed ones as one block of memory.
void VM::listCopyRange(ObjList* to, const ObjList* from, size_t start, size_t count) {
    if (!from->packed()) {
        const std::vector<Value>& values = from->values();
        to->values().insert(to->values().end(), values.begin() + static_cast<std::ptrdiff_t>(start),
                            values.begin() + static_cast<std::ptrdiff_t>(start + count));
        account(count * sizeof(Value));
        return;
    }
    const size_t old = to->pack->count;
    to->pack->block.resize((old + count) * to->pack->width);
    std::memcpy(to->pack->block.data() + old * to->pack->width, from->pack->block.data() + start * from->pack->width, count * from->pack->width);
    if (to->pack->nullable) {
        to->pack->nulls.resize((old + count + 63) / 64, 0);
        for (size_t i = 0; i < count; i++) setNullAt(to, old + i, nullAt(from, start + i));
    }
    to->pack->count = old + count;
    account(count * to->pack->width);
}

// Two items of one list change places.
void VM::listSwap(ObjList* list, size_t i, size_t j) {
    if (!list->packed()) {
        std::swap(list->values()[i], list->values()[j]);
        return;
    }
    uint8_t tmp[64];
    std::vector<uint8_t> big;
    uint8_t* t = list->pack->width <= sizeof tmp ? tmp : (big.resize(list->pack->width), big.data());
    std::memcpy(t, itemAt(list, i), list->pack->width);
    std::memcpy(itemAt(list, i), itemAt(list, j), list->pack->width);
    std::memcpy(itemAt(list, j), t, list->pack->width);
    if (list->pack->nullable) {
        const bool a = nullAt(list, i);
        setNullAt(list, i, nullAt(list, j));
        setNullAt(list, j, a);
    }
}

// A value made from its header alone, every argument given (M6.6 part 4): its fields written
// straight from the arguments, with no initializer run. Where the result goes, if `dest` is such
// a place and still there (value-lists.html section 7), every argument worked out before any of
// the old value is written over; otherwise in a box of its own. The result in place of the call.
void VM::makeFromHeader(ObjClass* cls, int argc, Value dest) {
    const ValueLayout& layout = layoutOf(cls);
    Value* const args = sp_ - argc;
    if (dest.isRef() && refClass(dest) == cls && refBytes(dest) != nullptr) {
        uint8_t small[256];
        std::vector<uint8_t> large;
        uint8_t* item = layout.size <= sizeof small ? small : (large.resize(layout.size), large.data());
        uint8_t* const at = refAt(dest, true);
        std::memcpy(item, at, layout.size);
        for (int k = 0; k < argc; k++) {
            const ValueField& field = layout.fields[static_cast<size_t>(cls->declaredIndex.find(cls->components[static_cast<size_t>(k)])->as.i)];
            writeValueField(item + field.offset, field, cls, args[k]);
        }
        std::memcpy(at, item, layout.size);
        sp_ = args;
        sp_[-1] = dest;
        return;
    }
    if (cls->smallId != 0) {
        // Small: in the value itself, with no box (M6.6 part 5).
        uint8_t bytes[8] = {};
        for (int k = 0; k < argc; k++) {
            const ValueField& field = layout.fields[static_cast<size_t>(cls->declaredIndex.find(cls->components[static_cast<size_t>(k)])->as.i)];
            writeValueField(bytes + field.offset, field, cls, args[k]);
        }
        sp_ = args;
        sp_[-1] = smallFrom(cls, bytes, sizeof bytes);
        return;
    }
    ObjValue* box = newValue(cls);
    sp_[-argc - 1] = Value::object(box);  // kept where the collector sees it
    for (int k = 0; k < argc; k++) {
        const ValueField& field = layout.fields[static_cast<size_t>(cls->declaredIndex.find(cls->components[static_cast<size_t>(k)])->as.i)];
        writeValueField(box->data() + field.offset, field, cls, args[k]);
    }
    sp_ = args;
}

// A class finished: one more for VM::layInline to wait for. Kept out of run().
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void VM::classFinished() {
    (root_ != nullptr ? *root_ : *this).classesFinished_.fetch_add(1, std::memory_order_acq_rel);
}

// Which of a class's fields are kept inside its objects (M6.6 part 5, value-lists.html section
// 4): those whose type is a value type kept in a box elsewhere, not small, laid out one after
// another after the object's other slots. One that may be null is kept there too (G16): its slot
// holds a Ref to it, or null. Worked out when the first object of the class is made where every
// class those fields name exists; until then, objects are made with no room for any of them and
// keep their values in boxes, and the layout is tried again once another class has been made.
void VM::layInline(ObjClass* cls) {
    if (!cls->fieldsLaid.load(std::memory_order_acquire)) layFields(cls);
    VM& root = root_ != nullptr ? *root_ : *this;
    const uint64_t finished = root.classesFinished_.load(std::memory_order_acquire);
    if (cls->inlineTriedAt.load(std::memory_order_acquire) == finished) return;  // still waiting for a class
    std::vector<ObjClass::InlineField> fields;
    std::vector<int32_t> offsets(cls->declared.size(), -1);
    uint32_t at = 0;
    bool waiting = false;
    for (size_t i = 0; i < cls->declared.size(); i++) {
        FieldDecl& declared = cls->declared[i];
        if ((declared.type & kTypeMask) != kTypeInstance) continue;
        ObjClass* type = declared.typeClass;
        const ValueLayout* layout = nullptr;
        try {
            if (type == nullptr) type = fieldClass(declared);
            if (type->isValue) layout = &layoutOf(type);
        } catch (AstraThrow&) {
            waiting = true;  // not there yet
            continue;
        }
        if (layout == nullptr || type->smallId != 0) continue;  // not a value type, or kept in the value itself
        at = (at + 7) / 8 * 8;
        offsets[i] = static_cast<int32_t>(at);
        fields.push_back({at, layout->size, type});
        at += layout->size;
    }
    std::lock_guard<std::mutex> hold(root.internMutex_);
    if (cls->inlineLaid.load(std::memory_order_acquire)) return;
    if (waiting) {
        cls->inlineTriedAt.store(finished, std::memory_order_release);
        return;
    }
    for (size_t i = 0; i < offsets.size(); i++) cls->declared[i].inlineOffset = offsets[i];
    cls->inlineFields = std::move(fields);
    cls->inlineSlots = (at + sizeof(Value) - 1) / sizeof(Value);
    cls->inlineLaid.store(true, std::memory_order_release);
}

// Where the objects of a class keep each field (M6.6 part 6, FieldDecl::slot), worked out when
// the first one is made: an int, a float or a bool whose type is written and is not null as its
// payload alone, the first 256 of them, after the bits that say which have a value; any other
// field in a slot of its own.
void VM::layFields(ObjClass* cls) {
    VM& root = root_ != nullptr ? *root_ : *this;
    std::lock_guard<std::mutex> hold(root.internMutex_);
    if (cls->fieldsLaid.load(std::memory_order_acquire)) return;
    auto kindOf = [](const FieldDecl& d) {
        if ((d.type & kTypeNullable) != 0) return FieldDecl::NoPayload;
        switch (d.type & kTypeMask) {
            case kTypeInt: return FieldDecl::IntPayload;
            case kTypeFloat: return FieldDecl::FloatPayload;
            case kTypeBool: return FieldDecl::BoolPayload;
            default: return narrowOf(d.type & kTypeMask) != Narrow::None ? FieldDecl::NarrowPayload : FieldDecl::NoPayload;
        }
    };
    // A field its init must set, here or in a class above.
    auto required = [&](const ObjString* name) {
        for (const ObjClass* k = cls; k != nullptr; k = k->super)
            for (const ObjString* r : k->ownRequired)
                if (r == name) return true;
        return false;
    };
    // A bit each only for the vals and the fields init must set, up to 256 of those.
    uint32_t tracked = 0;
    for (const FieldDecl& d : cls->declared)
        if (kindOf(d) != FieldDecl::NoPayload && (d.isVal || required(d.name))) tracked++;
    tracked = std::min(tracked, 256u);
    uint32_t at = (tracked + 63) / 64 * 8, slot = 0, bit = 0;
    // The 8-byte payloads first, then the narrow ones from the widest down, each in line.
    std::vector<FieldDecl*> narrow[5];
    for (FieldDecl& d : cls->declared) {
        d.inlineOffset = -1;  // (part 5 lays those out later, from what is here)
        const FieldDecl::PayloadKind kind = kindOf(d);
        const bool needsBit = d.isVal || required(d.name);
        if (kind != FieldDecl::NoPayload && (!needsBit || bit < tracked)) {
            d.payload = kind;
            d.tracked = needsBit;
            d.payloadBit = static_cast<uint16_t>(needsBit ? bit++ : 0);
            d.slot = -1;
            if (kind == FieldDecl::NarrowPayload) {
                d.narrow = narrowOf(d.type & kTypeMask);
                narrow[narrowSize(d.narrow)].push_back(&d);
                continue;
            }
            d.payloadOffset = static_cast<int32_t>(at);
            at += 8;
        } else {
            d.payload = FieldDecl::NoPayload;
            d.payloadOffset = -1;
            d.slot = static_cast<int32_t>(slot++);
        }
    }
    for (size_t size : {4, 2, 1})
        for (FieldDecl* d : narrow[size]) {
            d->payloadOffset = static_cast<int32_t>(at);
            at += static_cast<uint32_t>(size);
        }
    at = (at + 7) / 8 * 8;  // the slots after them are 8-byte aligned
    cls->slotCount = slot;
    cls->payloadBytes = at;
    cls->fieldsLaid.store(true, std::memory_order_release);
}

// A field declared after the class's objects were laid out: a slot of its own, which the
// objects made before do not have.
void VM::fieldAdded(ObjClass* cls, size_t index) {
    FieldDecl& d = cls->declared[index];
    d.payload = FieldDecl::NoPayload;
    d.payloadOffset = -1;
    d.slot = cls->fieldsLaid.load(std::memory_order_acquire) ? static_cast<int32_t>(cls->slotCount++) : -1;
}

// `pts.add(Point(1, 2))` (Op::AddNew): the value written straight into a new item at the end of
// a list that keeps its items packed, its fields from the arguments, as Point(1, 2) would give
// them; false, with nothing done, where that is not so and the call and the add follow.
bool VM::addNew(int argc) {
    Value* const args = sp_ - argc;
    if (!args[-2].isObjType(ObjType::List) || !args[-1].isObjType(ObjType::Class)) return false;
    ObjList* list = asList(args[-2]);
    ObjClass* cls = asClass(args[-1]);
    if (list->store() != ListStore::Items || list->pack->itemClass != cls || !cls->madeFromHeader ||
        static_cast<size_t>(argc) != cls->components.size())
        return false;
    guardWrite(list);
    if (cls->staticInit != nullptr) ensureClassInit(cls);
    const ValueLayout& layout = layoutOf(cls);
    // Made apart first, so that an argument of the wrong type leaves the list as it was.
    uint8_t small[256];
    std::vector<uint8_t> large;
    uint8_t* item = layout.size <= sizeof small ? small : (large.resize(layout.size), large.data());
    std::memset(item, 0, layout.size);
    for (int k = 0; k < argc; k++) {
        const ValueField& field = layout.fields[static_cast<size_t>(cls->declaredIndex.find(cls->components[static_cast<size_t>(k)])->as.i)];
        writeValueField(item + field.offset, field, cls, args[k]);
    }
    PackedItems& pack = *list->pack;
    const size_t before = pack.block.capacity();
    pack.block.insert(pack.block.end(), item, item + layout.size);
    if (pack.nullable) {
        pack.nulls.resize((pack.count + 1 + 63) / 64, 0);
        setNullAt(list, pack.count, false);
    }
    pack.count++;
    if (pack.block.capacity() > before) account(pack.block.capacity() - before);
    sp_ = args - 2;
    *sp_++ = Value::boolean(true);
    return true;
}

// Puts a list's items in the order `order` gives, the position each comes from: each item is
// moved once, a packed one as its bytes.
void VM::listArrange(ObjList* list, const std::vector<size_t>& order) {
    if (!list->packed()) {
        std::vector<Value> arranged;
        arranged.reserve(order.size());
        for (size_t from : order) arranged.push_back(list->values()[from]);
        list->values() = std::move(arranged);
        return;
    }
    PackedItems& pack = *list->pack;
    std::vector<uint8_t> block(pack.block.size());
    for (size_t i = 0; i < order.size(); i++)
        std::memcpy(block.data() + i * pack.width, pack.block.data() + order[i] * pack.width, pack.width);
    if (pack.nullable) {
        std::vector<bool> nulls(order.size());
        for (size_t i = 0; i < order.size(); i++) nulls[i] = nullAt(list, order[i]);
        for (size_t i = 0; i < order.size(); i++) setNullAt(list, i, nulls[i]);
    }
    pack.block = std::move(block);
}


// The collector's view of a packed list of a value type that holds objects.
void VM::markItems(ObjList* list) {
    if (!list->packed() || list->pack->store != ListStore::Items) return;
    const ValueLayout* layout = list->pack->itemClass->layout.load(std::memory_order_acquire);
    markObject(list->pack->itemClass);
    if (layout == nullptr || (layout->objects.empty() && layout->values.empty())) return;
    for (size_t i = 0; i < list->pack->count; i++) {
        const uint8_t* item = itemAt(list, i);
        for (uint32_t offset : layout->objects) {
            Obj* held;
            std::memcpy(&held, item + offset, sizeof held);
            markObject(held);
        }
        for (uint32_t offset : layout->values) {
            Value held;
            std::memcpy(&held, item + offset, sizeof held);
            markValue(held);
        }
    }
}

// ---- References (M6.6 part 4, value-lists.html section 6) ----

Value VM::ownRef(Value r) {
    const uint8_t* at = refAt(r, false);
    ObjClass* cls = refClass(r);
    ObjValue* box = newValue(cls);
    std::memcpy(box->data(), at, layoutOf(cls).size);
    return Value::object(box);
}

Value VM::boxSmall(Value v) {
    ObjClass* cls = smallClassOf(v);
    ObjValue* box = newValue(cls);
    std::memcpy(box->data(), smallBytes(v), layoutOf(cls).size);
    return Value::object(box);
}

uint8_t* VM::refAt(Value r, bool write) {
    uint8_t* at = refBytes(r);
    if (at == nullptr)
        throw runtimeError(Thrown::IndexOutOfBoundsException, "item " + std::to_string(r.refIndex) + " of the list is no longer there: the list was made shorter "
                           "while the item was in use");
    if (r.as.obj == bandObject_) [[unlikely]] {
        // In a parallel section over this list: this core's own part of it, and no other.
        if (r.refIndex < bandFrom_ || r.refIndex >= bandTo_) throw outsideBand(r.refIndex);
    } else if (r.as.obj->type == ObjType::Instance) {
        if (write) guardWrite(r.as.obj);
        else guardRead(r.as.obj);
    } else if (write) {
        if (r.as.obj->type == ObjType::List) guardWrite(r.as.obj);
        else if (!static_cast<ObjValue*>(r.as.obj)->constructing)
            throw runtimeError(Thrown::UnsupportedOperationException, "a value of " + refClass(r)->name->utf8() + " cannot be changed there: a value never changes "
                               "where others may see it; change it through the place that holds it");
    }
    return at;
}

bool VM::refField(Value r, const ObjString* name, Value& out) {
    ObjClass* cls = refClass(r);
    const Value* index = cls->declaredIndex.find(name);
    if (index == nullptr) return false;
    const ValueField& field = layoutOf(cls).fields[static_cast<size_t>(index->as.i)];
    if (field.kind == ValueField::Nested) {
        refAt(r, false);  // checked: still there
        out = Value::ref(r.as.obj, r.refIndex, static_cast<uint16_t>(r.refOffset + field.offset), static_cast<uint8_t>(r.refDepth + 1));
    } else {
        out = readValueField(refAt(r, false) + field.offset, field);
    }
    return true;
}

// `place.x = v` where the place is a Ref (M6.6 part 4, value-lists.html section 8): the field
// written where the value is. Only a `var` field; a value never changes where others see it.
void VM::setRefField(Value r, ObjString* name, Value value) {
    ObjClass* cls = refClass(r);
    const Value* index = cls->declaredIndex.find(name);
    if (index == nullptr)
        throw runtimeError(Thrown::UnsupportedOperationException, cls->name->utf8() + " has no field '" + name->utf8() + "'; fields must be declared in it with val or var");
    const size_t at = static_cast<size_t>(index->as.i);
    if (cls->declared[at].isVal) throw runtimeError(Thrown::UnsupportedOperationException, "cannot assign to val field '" + name->utf8() + "' of " + cls->name->utf8());
    const ValueField& field = layoutOf(cls).fields[at];
    writeValueField(refAt(r, true) + field.offset, field, cls, value);
}

Value VM::placeIn(Value& slot) {
    if (slot.isRef() && slot.as.obj->type != ObjType::Value) return slot;  // where it is already: in a list or an object
    const Value kept = boxed(slot);
    // Null, for `p?.move()` to pass by: what is changed through it reports a change to null.
    if (kept.isNull()) return kept;
    if (!kept.isObjType(ObjType::Value))
        throw runtimeError(Thrown::NullPointerException, "cannot change a field of " + (kept.isNull() ? std::string("null") : "a value of type " + typeName(kept)) +
                           " through its place");
    // A box of its own, which nothing else holds: a change made in it is seen through this
    // variable or field alone (`val q = p; p.x = 5` leaves q as it was).
    ObjValue* fresh = newValue(asValue(kept)->cls);
    std::memcpy(fresh->data(), asValue(kept)->data(), layoutOf(fresh->cls).size);
    fresh->constructing = true;  // may be written, through the place
    slot = Value::object(fresh);
    return Value::ref(fresh, 0, 0, 0);
}

bool VM::construct(ObjClass* cls, int argc, bool packed, const Value* captures, uint16_t captureCount) {
    if (cls->staticInit != nullptr) ensureClassInit(cls);  // an enum has its entries by now
    if (cls->isAbstract)
        throw runtimeError(Thrown::UnsupportedOperationException, cls->name->utf8() + " is abstract and cannot be created; create a class that extends it");
    if (cls->isInterface)
        throw runtimeError(Thrown::UnsupportedOperationException, cls->name->utf8() + " is an interface and cannot be created; create a class that implements it");
    if (cls->enumSealed)
        throw runtimeError(Thrown::UnsupportedOperationException, "enum " + cls->name->utf8() + " has only the entries it declares; " + cls->name->utf8() +
                           "() cannot make another");
    // A generic class's type arguments, given by WithTypes: the object keeps its type.
    const ObjTypeInfo* typeArgs = std::exchange(pendingTypeArgs_, nullptr);
    if (cls->isValue) {
        const Value dest = std::exchange(pendingDestination_, Value::null());
        // Every argument given: none left out by a call by name (`Insets(top = 1, right = 2)`),
        // which the initializer gives its default.
        bool allGiven = cls->madeFromHeader && static_cast<size_t>(argc) == cls->components.size() && !packed;
        for (int k = 0; allGiven && k < argc; k++) allGiven = sp_[-argc + k].kind != ValueKind::Undefined;
        if (allGiven) {
            makeFromHeader(cls, argc, dest);
            return false;
        }
        // A value, in a box (M6.6 part 2): its fields are given their values, and then it never
        // changes. (A generic value type's type arguments are not kept yet.)
        ObjValue* box = newValue(cls);
        sp_[-argc - 1] = Value::object(box);
        box->constructing = true;
        for (size_t i = 0; i < cls->fieldInits.size(); i++) callWithReceiver(Value::object(box), cls->fieldInits[i]);
        if (cls->init != nullptr) {
            callClosure(cls->init, argc, packed);
            return true;
        }
        box->constructing = false;
        if (argc != 0) throw runtimeError(Thrown::IllegalArgumentException, cls->name->utf8() + "() takes no arguments but got " + std::to_string(argc));
        return false;
    }
    if (!cls->inlineLaid.load(std::memory_order_acquire)) layInline(cls);
    ObjInstance* instance = newInstance(cls, captureCount, typeArgs != nullptr);
    for (uint16_t i = 0; i < captureCount; i++) instance->fields()[instance->fieldCount + i] = captures[i];
    if (typeArgs != nullptr) instance->setType(typeOf(kTypeInstance, false, cls, typeArgs->args));
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
    if (argc != 0) throw runtimeError(Thrown::IllegalArgumentException, cls->name->utf8() + "() takes no arguments but got " + std::to_string(argc));
    return false;
}

ObjUpvalue* VM::capturedOf(Value receiver, int k) {
    if (!receiver.isObjType(ObjType::Instance) || k >= asInstance(receiver)->captureCount)
        throw runtimeError(Thrown::InternalError, "invalid bytecode: no such captured variable");
    return asInstance(receiver)->captured(static_cast<uint32_t>(k));
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
        if (!instance->hasValue(name))
            throw runtimeError(Thrown::IllegalStateException, owner->name->utf8() + ".init() must set field '" + name->utf8() + "'");
}

// Enforces the field declarations of a class that has any (its own or inherited): only
// declared fields exist, a val can be assigned only once, and a typed field only takes
// values of its type.
void VM::checkFieldAssignment(ObjInstance* instance, ObjString* stored, Value value) {
    ObjClass* cls = instance->cls;
    // A property with accessors keeps its value under "field:" and its name; messages say the name.
    struct Shown {
        std::string text;
        const std::string& utf8() const { return text; }
    };
    const Shown shown{stored->utf8().rfind("field:", 0) == 0 ? stored->utf8().substr(6) : stored->utf8()};
    const Shown* const name = &shown;
    const Value* index = cls->declaredIndex.find(stored);
    if (index == nullptr)
        throw runtimeError(Thrown::UnsupportedOperationException, cls->name->utf8() + " has no field '" + name->utf8() +
                           "'; fields must be declared in the class with val or var");
    size_t at = static_cast<size_t>(index->as.i);

    if (cls->declared[at].isVal && instance->hasValue(stored) &&
        !(cls->declared[at].overrides && instance->constructing))
        throw runtimeError(Thrown::UnsupportedOperationException, "cannot assign to val field '" + name->utf8() + "' of " + cls->name->utf8());

    uint8_t type = cls->declared[at].type;
    if (type == 0) return;
    if (value.isNull() && (type & kTypeNullable) != 0) return;

    if ((type & kTypeMask) == kTypeInstance && cls->declared[at].typeClass == nullptr) {
        // First use of a class-typed field: find out which class its type names.
        TempRoot keepAlive(*this, value);
        Value resolved = call(Value::object(cls->declared[at].typeThunk), 0, nullptr);
        if (!resolved.isObjType(ObjType::Class))
            throw runtimeError(Thrown::ClassCastException, "a type must be a class or a built-in type, not a value of type " + typeName(resolved));
        // (In a parallel section several cores may find this out at once: they all find the same class.)
        cls->declared[at].typeClass = asClass(resolved);
    }

    const ObjClass* typeClass = cls->declared[at].typeClass;
    if ((!matchesType(value, type, typeClass) && !widenedByte(value, type))) checkRange(type, value, "field '" + name->utf8() + "' of " + cls->name->utf8());
    if (!matchesType(value, type, typeClass))
        throw runtimeError(Thrown::ClassCastException, "cannot assign a value of type " + typeName(value) + " to field '" + name->utf8() + "' of " +
                           cls->name->utf8() + ", which is declared as " + declaredTypeText(type, typeClass));
}

// `super.name(...)`: the method of the superclass, or failing that the one that comes with an
// interface the object's class implements (a class with only interfaces has no superclass).
const Value* VM::superMethod(Value super, Value self, const ObjString* name) const {
    if (super.isObjType(ObjType::Class)) {
        if (const Value* method = asClass(super)->methods.find(name)) return method;
    }
    if (const ObjClass* cls = classOf(self)) {
        for (const ObjClass* implemented : cls->interfaces)
            if (const Value* method = implemented->methods.find(name)) return method;
    }
    return nullptr;
}

// Gives an enum entry its name and its place in the list. A getter the enum wrote for `name`
// or `ordinal` stands in for the field; the real ones are kept under names of their own.
// An enum's entries keep their name and their place in the list in fields the class has
// besides those it declares, and in `name` and `ordinal` unless properties of those names
// stand in for them. They are added to the class before its first entry is made.
void VM::declareEntryFields(ObjClass* cls) {
    if (cls->declaredIndex.find(names.entryName) != nullptr) return;
    auto add = [&](ObjString* name) {
        if (cls->declaredIndex.find(name) != nullptr) return;
        FieldDecl field;
        field.name = name;
        field.isVal = true;
        cls->declaredIndex.set(name, Value::integer(static_cast<int64_t>(cls->declared.size())));
        cls->declared.push_back(field);
        fieldAdded(cls, cls->declared.size() - 1);
    };
    add(names.entryName);
    add(names.entryOrdinal);
    if (cls->getters.find(names.name) == nullptr) add(names.name);
    if (cls->getters.find(names.ordinal) == nullptr) add(names.ordinal);
    cls->shape = newShape();
}

void VM::nameEntry(ObjInstance* entry, ObjString* name, int ordinal) {
    const ObjClass* cls = entry->cls;
    if (entry->declOf(names.entryName) == nullptr) throw runtimeError(Thrown::InternalError, "invalid enum declaration");
    entry->setField(names.entryName, Value::object(name));
    entry->setField(names.entryOrdinal, Value::integer(ordinal));
    if (cls->getters.find(names.name) == nullptr) entry->setField(names.name, Value::object(name));
    if (cls->getters.find(names.ordinal) == nullptr) entry->setField(names.ordinal, Value::integer(ordinal));
}

Value VM::classValue(ObjTypeInfo* type) {
    if (classOfFn_.isNull()) return Value::object(type);  // the standard library not loaded yet
    Value args[2] = {Value::object(type), Value::null()};
    return call(classOfFn_, 2, args);
}

Value VM::makeEntry(Value key, Value value) {
    if (mapEntryClass_ == nullptr) {
        ObjList* pair = alloc<ObjList>();
        pair->values() = {key, value};
        return Value::object(pair);
    }
    // A value (D73): a box holding the key and the value, in MapEntry's layout.
    TempRoot keepKey(*this, key), keepValue(*this, value);
    ObjValue* entry = newValue(mapEntryClass_);
    const ValueLayout& layout = layoutOf(mapEntryClass_);
    writeValueField(entry->data() + layout.fields[0].offset, layout.fields[0], mapEntryClass_, key);
    writeValueField(entry->data() + layout.fields[1].offset, layout.fields[1], mapEntryClass_, value);
    return Value::object(entry);
}

// What a list made of a map's keys, values or entries holds, as the map says (M6.6 part 1):
// 0 its keys, 1 its values, 2 its entries; null for a map made without its type.
ObjTypeInfo* VM::mapPartType(const ObjMap* map, int part) {
    if (map->type == nullptr || map->type->args.size() != 2) return nullptr;
    ObjTypeInfo* item = part < 2 ? map->type->args[static_cast<size_t>(part)]
                                 : mapEntryClass_ != nullptr ? typeOf(kTypeInstance, false, mapEntryClass_, map->type->args) : nullptr;
    return item != nullptr ? typeOf(kTypeList, false, nullptr, {item}) : nullptr;
}

Value VM::mapEntries(ObjMap* map) {
    ObjTypeInfo* const type = mapPartType(map, 2);
    ObjList* list = alloc<ObjList>();
    TempRoot keep(*this, Value::object(list));
    for (const ValueMap::Entry& entry : map->map.entries())
        if (entry.live) list->values().push_back(makeEntry(entry.key, entry.value));
    account(list->values().size() * sizeof(Value));
    typeList(list, type);
    return Value::object(list);
}

Value VM::mapKeys(ObjMap* map) {
    ObjTypeInfo* const type = mapPartType(map, 0);
    ObjList* list = alloc<ObjList>();
    for (const ValueMap::Entry& entry : map->map.entries())
        if (entry.live) list->values().push_back(entry.key);
    account(list->values().size() * sizeof(Value));
    typeList(list, type);
    return Value::object(list);
}

// A map's values (G22): a Values, a live view of them as a keys view is of its keys.
Value VM::mapValues(ObjMap* map) {
    ObjTypeInfo* item = mapPartType(map, 1);
    ObjSet* view = newSet(typeOf(kTypeValues, false, nullptr, {item != nullptr && !item->args.empty() ? item->args[0] : typeOf(0, true, nullptr, {})}));
    view->viewOf = map;
    view->values = true;
    return Value::object(view);
}

// Runs what works out a class's static values, if it has not run yet, superclasses first.
void VM::ensureClassInit(ObjClass* cls) {
    if (cls->super != nullptr && cls->super->staticInit != nullptr) ensureClassInit(cls->super);
    ObjClosure* init = cls->staticInit;
    if (init == nullptr) return;
    // Working out a class's static values changes the class, which every core sees.
    if (cls->core != coreId_)
        throw notOwned("class " + cls->name->utf8() + " is used for the first time here, and its static values are worked out on " +
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
            throw runtimeError(Thrown::ClassCastException, "a type must be a class or a built-in type, not a value of type " + typeName(resolved));
        field.typeClass = asClass(resolved);
    }
    if ((!matchesType(value, type, field.typeClass) && !widenedByte(value, type))) checkRange(type, value, "static field '" + field.name->utf8() + "' of " + owner->name->utf8());
    if (!matchesType(value, type, field.typeClass))
        throw runtimeError(Thrown::ClassCastException, "cannot assign a value of type " + typeName(value) + " to static field '" + field.name->utf8() +
                           "' of " + owner->name->utf8() + ", which is declared as " + declaredTypeText(type, field.typeClass));
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
            if (field.isVal) throw runtimeError(Thrown::UnsupportedOperationException, "cannot assign to static val '" + name->utf8() + "' of " + k->name->utf8());
            checkStaticType(k, field, value);
            k->statics.set(name, value);
            return;
        }
        if (const Value* setter = k->staticSetters.find(name)) {
            call(*setter, 1, &value);
            return;
        }
        if (k->staticGetters.find(name) != nullptr)
            throw runtimeError(Thrown::IllegalStateException, "static property '" + name->utf8() + "' of " + k->name->utf8() +
                               " cannot be assigned: it has no setter");
    }
    cls->statics.set(name, value);
}

void VM::setProperty(Value obj, ObjString* name, Value value) {
    if (obj.isRef()) {
        setRefField(obj, name, value);  // in place, where the value is
        return;
    }
    // A Ref is copied where the value is kept as it is; one kept inside an object is copied from
    // where it is (M6.6 part 5).
    if (obj.isObjType(ObjType::Value)) {
        setValueField(asValue(obj), name, own(value));  // only while it is being made
        return;
    }
    if (obj.isObjType(ObjType::Instance)) {
        ObjInstance* instance = asInstance(obj);
        guardWrite(instance);
        if (instance->cls->isEnum && (name == names.name || name == names.ordinal))
            throw runtimeError(Thrown::UnsupportedOperationException, "cannot assign to '" + name->utf8() + "' of an entry of enum " + instance->cls->name->utf8());
        checkFieldAssignment(instance, name, value);  // every field is declared (R24)
        if (instance->inlined) {
            const FieldDecl* decl = instance->declOf(name);
            if (decl != nullptr && decl->inlineOffset >= 0) {
                // A value kept inside the object: its bytes copied in (M6.6 part 5).
                const int64_t at = decl->slot;
                const uint32_t offset = static_cast<uint32_t>(decl->inlineOffset);
                if (value.isNull()) {
                    // One that may be null (G16), given null: the slot says so, and the bytes are
                    // cleared, holding nothing for the collector.
                    std::memset(instance->inlineData() + offset, 0, instance->cls->inlineFieldAt(offset)->size);
                    instance->fields()[at] = Value::null();
                    return;
                }
                ValueField whole;
                whole.kind = ValueField::Nested;
                whole.cls = instance->cls->inlineFieldAt(offset)->cls;
                writeValueField(instance->inlineData() + offset, whole, instance->cls, value);
                instance->fields()[at] = Value::ref(instance, 0, static_cast<uint16_t>(offset), 0);
                return;
            }
        }
        if (!instance->setField(name, own(value)))
            throw runtimeError(Thrown::UnsupportedOperationException, instance->cls->name->utf8() + " has no field '" + name->utf8() + "'");
    } else if (obj.isObjType(ObjType::Class)) {
        assignStatic(asClass(obj), name, own(value));
    } else {
        throw runtimeError(Thrown::UnsupportedOperationException, "cannot set property '" + name->utf8() + "' on a value of type " + typeName(obj));
    }
}

namespace {

// Resolves a possibly negative index against a length; returns false if out of range.
// A position from the start: a negative one is never one counted from the end (D146).
bool resolveIndex(int64_t index, size_t length, size_t& out) {
    if (index < 0 || static_cast<uint64_t>(index) >= length) return false;
    out = static_cast<size_t>(index);
    return true;
}

}  // namespace

// Where `index` is in `size` items: an int from the start, or ^k from the end (D146); false
// past either end. A negative int is no position, never one counted from the end.
bool VM::position(Value index, size_t size, size_t& out, const char* what) {
    if (index.isFromEnd()) {
        if (static_cast<uint64_t>(index.as.i) > size) return false;
        out = size - static_cast<size_t>(index.as.i);
        return true;
    }
    return resolveIndex(toInt(index, what), size, out);
}

AstraThrow VM::outOfRange(const char* what, Value index, size_t size) {
    std::string message = std::string(what) + " " + stringify(index) + " is out of range (size " + std::to_string(size) + ")";
    if (index.isInt() && index.as.i < 0) message += ": a negative number is no position; the last is [^1]";
    return runtimeError(Thrown::IndexOutOfBoundsException, message);
}

// The positions a range in an index takes in `size` items (D147): from `begin` up to `end`,
// left out. Inclusive as `..` is; an end counted from the end resolved; an error past either
// end or running backwards.
void VM::sliceBounds(const ObjRange* range, size_t size, size_t& begin, size_t& end) {
    if (range->step != 1) throw runtimeError(Thrown::IllegalArgumentException, "a slice takes a range that counts up by one, not " + stringify(Value::object(const_cast<ObjRange*>(range))));
    const int64_t n = static_cast<int64_t>(size);
    const int64_t first = range->startFromEnd ? n - range->start : range->start;
    const int64_t last = (range->endFromEnd ? n - range->end : range->end) + (range->inclusive ? 1 : 0);
    const std::string shown = stringify(Value::object(const_cast<ObjRange*>(range)));
    if (first < 0 || first > n || last < 0 || last > n)
        throw runtimeError(Thrown::IndexOutOfBoundsException, "the slice " + shown + " is out of range (size " + std::to_string(size) + ")");
    if (last < first) throw runtimeError(Thrown::IllegalArgumentException, "the slice " + shown + " runs backwards: from " + std::to_string(first) + " to " + std::to_string(last - 1));
    begin = static_cast<size_t>(first);
    end = static_cast<size_t>(last);
}

// A range with an end counted from the end, used where its own numbers are wanted.
AstraThrow VM::rangeFromEnd(const ObjRange* range, const std::string& use) {
    const std::string shown = stringify(Value::object(const_cast<ObjRange*>(range)));
    return runtimeError(Thrown::IllegalArgumentException, "the range " + shown + " has an end counted from the end (^), which " + use +
                        " cannot work with: slice with it, items[" + shown + "], or make it plain with of(size)");
}

Value VM::getIndex(Value obj, Value index) {
    if (obj.isObj()) {
        guardRead(obj.as.obj);
        // A range in the index: that part, a copy (D147).
        if (index.isObjType(ObjType::Range) && (obj.as.obj->type == ObjType::List || obj.as.obj->type == ObjType::String ||
                                                obj.as.obj->type == ObjType::Bytes)) {
            size_t begin, end;
            if (obj.isObjType(ObjType::List)) {
                ObjList* list = asList(obj);
                sliceBounds(asRange(index), list->size(), begin, end);
                ObjList* part = listLike(list);
                TempRoot keep(*this, Value::object(part));
                listCopyRange(part, list, begin, end - begin);
                return Value::object(part);
            }
            if (obj.isString()) {
                const ObjString* text = asString(obj);
                sliceBounds(asRange(index), text->length, begin, end);
                return str(text->slice(begin, end));
            }
            const std::vector<uint8_t>& data = asBytes(obj)->data;
            sliceBounds(asRange(index), data.size(), begin, end);
            ObjBytes* part = alloc<ObjBytes>();
            part->data.assign(data.begin() + static_cast<std::ptrdiff_t>(begin), data.begin() + static_cast<std::ptrdiff_t>(end));
            account(part->data.size());
            return Value::object(part);
        }
        switch (obj.as.obj->type) {
            case ObjType::List: {
                ObjList* list = asList(obj);
                size_t i;
                if (!position(index, list->size(), i, "list index")) throw outOfRange("list index", index, list->size());
                return listGet(list, i);
            }
            case ObjType::String: {
                // One character, wherever it is: positions count characters, not bytes.
                const ObjString* text = asString(obj);
                size_t i;
                if (!position(index, text->length, i, "string index")) throw outOfRange("string index", index, text->length);
                return Value::character(text->at(i));  // a char (D24)
            }
            case ObjType::Map: {
                const Value* found = asMap(obj)->map.find(own(index));
                return found != nullptr ? *found : Value::null();
            }
            case ObjType::Bytes: {
                const std::vector<uint8_t>& data = asBytes(obj)->data;
                size_t i;
                if (!position(index, data.size(), i, "bytes index")) throw outOfRange("bytes index", index, data.size());
                return Value::byte(data[i]);  // a byte (D19)
            }
            default:
                break;
        }
    }
    throw runtimeError(Thrown::UnsupportedOperationException, "cannot index a value of type " + typeName(obj));
}

void VM::setIndex(Value obj, Value index, Value value) {
    if (obj.isObjType(ObjType::List)) {
        ObjList* list = asList(obj);
        size_t i;
        if (obj.as.obj != bandObject_) guardWrite(obj.as.obj);
        if (!position(index, list->size(), i, "list index")) throw outOfRange("list index", index, list->size());
        // In a parallel section over the parts of a list, a piece writes its own part of it (D96).
        if (obj.as.obj == bandObject_ && (i < bandFrom_ || i >= bandTo_ || obj.as.obj->frozen != 0)) [[unlikely]] {
            if (obj.as.obj->frozen != 0) guardWrite(obj.as.obj);
            throw outsideBand(i);
        }
        listSet(list, i, value);
    } else if (obj.isObjType(ObjType::Map)) {
        guardWrite(obj.as.obj);
        asMap(obj)->map.set(own(index), own(value));
        account(64);
    } else if (obj.isObjType(ObjType::Bytes)) {
        std::vector<uint8_t>& data = asBytes(obj)->data;
        size_t i;
        if (!position(index, data.size(), i, "bytes index")) throw outOfRange("bytes index", index, data.size());
        if (!value.isByte() && (!value.isInt() || value.as.i < 0 || value.as.i > 255))
            throw runtimeError(Thrown::IllegalArgumentException, "a byte is a whole number from 0 to 255, not " + stringify(value));
        // In a parallel section over the parts of a buffer, a core writes its own part of it.
        if (obj.as.obj->core != coreId_ && !(obj.as.obj == bandObject_ && i >= bandFrom_ && i < bandTo_)) {
            if (obj.as.obj == bandObject_)
                throw notOwned("position " + std::to_string(i) + " is outside this core's part of the buffer, which is " +
                               std::to_string(bandFrom_) + " to " + std::to_string(bandTo_ - 1));
            throw notOwned(obj.as.obj);
        }
        data[i] = static_cast<uint8_t>(value.as.i);
    } else {
        throw runtimeError(Thrown::UnsupportedOperationException, "cannot assign to an index of a value of type " + typeName(obj));
    }
}

// ---- Operators (the cases the interpreter loop does not handle inline) ----

bool VM::binarySlow(Op op) {
    Value a = sp_[-2], b = sp_[-1];
    guardRead(a);
    guardRead(b);
    ObjString* symbol = opNames_[static_cast<int>(op)];

    // set + x and set - x: a new set, with x or without it, or with or without what a list, a
    // range or another set holds.
    if (a.isObjType(ObjType::Set) && (op == Op::Add || op == Op::Sub)) {
        const ObjSet* from = asSet(a);
        ObjSet* made = newSet(from->type);
        TempRoot keep(*this, Value::object(made));
        for (const Value& item : setItems(from)) setAdd(made, item);
        const bool many = b.isObjType(ObjType::List) || b.isObjType(ObjType::Set) || b.isObjType(ObjType::Range);
        const std::vector<Value> others = many ? itemsOf(b, "set + and -") : std::vector<Value>{b};
        TempVector keepOthers(*this, others);
        for (const Value& item : others) {
            if (op == Op::Add) setAdd(made, item);
            else setRemove(made, item);
        }
        sp_[-2] = Value::object(made);
        sp_--;
        return false;
    }

    // A byte in arithmetic is the int it holds, so byte + byte is an int (D19).
    if ((a.isByte() && (b.isNumber() || b.isByte())) || (b.isByte() && a.isNumber())) {
        if (a.isByte()) sp_[-2] = a = Value::integer(a.as.i);
        if (b.isByte()) sp_[-1] = b = Value::integer(b.as.i);
        return numberArithmetic(op);
    }
    // Arithmetic with characters (D25): char + char is a string, char +- int a char,
    // char - char an int, int +- char an int.
    if ((a.isChar() || b.isChar()) && !a.isString() && !b.isString()) {
        const bool plus = op == Op::Add, minus = op == Op::Sub;
        Value result;
        if (plus && a.isChar() && b.isChar()) {
            std::string joined;
            appendUtf8(joined, static_cast<uint32_t>(a.as.i));
            appendUtf8(joined, static_cast<uint32_t>(b.as.i));
            result = str(std::move(joined));
        } else if ((plus || minus) && a.isChar() && b.isInt()) {
            int64_t c;
            if ((plus ? addOverflows(a.as.i, b.as.i, c) : subOverflows(a.as.i, b.as.i, c)) || !isCharacter(c))
                throw runtimeError(Thrown::IllegalArgumentException, quoteCharacter(static_cast<uint32_t>(a.as.i)) + (plus ? " + " : " - ") + std::to_string(b.as.i) +
                                   " is not a character: characters are numbered 0 to 1114111, leaving out 55296 to 57343");
            result = Value::character(static_cast<uint32_t>(c));
        } else if (minus && a.isChar() && b.isChar()) {
            result = Value::integer(a.as.i - b.as.i);
        } else if ((plus || minus) && a.isInt() && b.isChar()) {
            int64_t n;
            if (plus ? addOverflows(a.as.i, b.as.i, n) : subOverflows(a.as.i, b.as.i, n)) throw overflowError(op, a.as.i, b.as.i);
            result = Value::integer(n);
        } else {
            throw runtimeError(Thrown::UnsupportedOperationException, "unsupported operand types for '" + symbol->utf8() + "': " + typeName(a) + " and " + typeName(b));
        }
        sp_[-2] = result;
        sp_--;
        return false;
    }

    if (classOf(a) != nullptr) {
        invokeOperator(a, symbol, 1);
        return true;
    }

    if (op == Op::Add) {
        if (a.isString() || b.isString()) {
            // Joined at the width of the wider part (D20).
            if (a.isString() && b.isString()) {
                sp_[-2] = Value::object(joinStrings(asString(a), asString(b)));
                sp_--;
                return false;
            }
            Characters joined;
            joined.units.reserve((a.isString() ? asString(a)->units().size() : 16) + (b.isString() ? asString(b)->units().size() : 16));
            for (const Value& part : {a, b}) {
                if (part.isString()) {
                    appendText(joined, asString(part));
                } else {
                    std::string shown;
                    stringifyInto(shown, part, false, 0);
                    joined.appendUtf8Text(shown);
                }
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
            ObjList* left = asList(a);
            ObjList* right = asList(b);
            ObjList* joined;
            if (!left->packed() && !right->packed()) {
                // Two lists of 16-byte values, as most are: their items one after the other.
                joined = alloc<ObjList>();
                std::vector<Value>& items = joined->values();
                items.reserve(left->size() + right->size());
                items.insert(items.end(), left->values().begin(), left->values().end());
                items.insert(items.end(), right->values().begin(), right->values().end());
                account(items.size() * sizeof(Value));
                if (left->type == right->type) joined->type = left->type;
            } else if (left->type != nullptr && left->type == right->type) {
                // Two lists of one type: one of that type, packed as they are.
                joined = listLike(left);
                listCopyRange(joined, left, 0, left->size());
                listCopyRange(joined, right, 0, right->size());
            } else {
                joined = alloc<ObjList>();
                std::vector<Value>& items = joined->values();
                items = listCopy(left);
                const std::vector<Value> more = listCopy(right);
                items.insert(items.end(), more.begin(), more.end());
                account(items.size() * sizeof(Value));
            }
            sp_[-2] = Value::object(joined);
            sp_--;
            return false;
        }
    }

    throw runtimeError(Thrown::UnsupportedOperationException, "unsupported operand types for '" + symbol->utf8() + "': " + typeName(a) + " and " + typeName(b));
}

bool VM::compareSlow(Op op) {
    Value a = sp_[-2], b = sp_[-1];
    guardRead(a);
    guardRead(b);
    ObjString* symbol = opNames_[static_cast<int>(op)];

    if (classOf(a) != nullptr) {
        invokeOperator(a, symbol, 1);
        return true;
    }

    int order = 0;
    const bool numbers = (a.isNumber() || a.isByte()) && (b.isNumber() || b.isByte());
    if (a.isString() && b.isString()) {
        order = compareText(asString(a), asString(b));
    } else if (a.isChar() && b.isChar()) {
        order = a.as.i < b.as.i ? -1 : a.as.i > b.as.i;
    } else if (numbers) {
        // A byte compared is the number it holds.
        const bool whole = !a.isFloat() && !b.isFloat();
        if (whole) order = a.as.i < b.as.i ? -1 : a.as.i > b.as.i;
        else {
            const double x = a.isFloat() ? a.as.d : static_cast<double>(a.as.i), y = b.isFloat() ? b.as.d : static_cast<double>(b.as.i);
            const bool result = op == Op::Lt ? x < y : op == Op::Le ? x <= y : op == Op::Gt ? x > y : x >= y;
            sp_[-2] = Value::boolean(result);
            sp_--;
            return false;
        }
    }
    if (a.isString() && b.isString() || (a.isChar() && b.isChar()) || numbers) {
        bool result = op == Op::Lt ? order < 0 : op == Op::Le ? order <= 0 : op == Op::Gt ? order > 0 : order >= 0;
        sp_[-2] = Value::boolean(result);
        sp_--;
        return false;
    }

    throw runtimeError(Thrown::ClassCastException, "cannot compare " + typeName(a) + " and " + typeName(b) + " with '" + symbol->utf8() + "'");
}

// ---- Declared variable types ----

bool VM::matchesType(const Value& v, uint8_t type, const ObjClass* cls) const {
    if (v.isNull()) return (type & kTypeNullable) != 0 || (type & kTypeMask) == kTypeNull;

    switch (type & kTypeMask) {
        case kTypeNull: return false;  // anything but null
        case kTypeInt: return v.isInt();
        case kTypeFloat: return v.isFloat();
        case kTypeNumber: return v.isNumber();
        case kTypeBool: return v.isBool();
        case kTypeString: return v.isString();
        case kTypeList: return v.isObjType(ObjType::List);
        case kTypeMap: return v.isObjType(ObjType::Map);
        case kTypeSet: return v.isObjType(ObjType::Set) && !asSet(v)->values;
        case kTypeValues: return v.isObjType(ObjType::Set) && asSet(v)->values;
        case kTypeFrozenList: return v.isObjType(ObjType::List) && v.as.obj->frozen != 0;
        case kTypeFrozenMap: return v.isObjType(ObjType::Map) && v.as.obj->frozen != 0;
        case kTypeFrozenSet: return v.isObjType(ObjType::Set) && !asSet(v)->values && v.as.obj->frozen != 0;
        case kTypeRange: return v.isObjType(ObjType::Range);
        case kTypeBytes: return v.isObjType(ObjType::Bytes);
        case kTypeChar: return v.isChar();
        case kTypeByte: return v.isByte();
        case kTypeInt16:
        case kTypeInt32:
        case kTypeUint16:
        case kTypeUint32: {
            if (!v.isInt()) return false;
            int64_t low, high;
            intStorageRange(type & kTypeMask, low, high);
            return v.as.i >= low && v.as.i <= high;
        }
        case kTypeFloat32: return v.isFloat();
        case kTypeIndex: return v.isInt() || v.isFromEnd();
        case kTypeType: return v.isObjType(ObjType::TypeInfo);
        case kTypeFunction:
            return v.isObjType(ObjType::Closure) || v.isObjType(ObjType::Native) ||
                   v.isObjType(ObjType::BoundMethod) || v.isObjType(ObjType::BoundNative);
        case kTypeInstance: return classOf(v) != nullptr && classOf(v)->isSubclassOf(cls);
        default: return false;
    }
}

// For a class type, takes the class off the stack (the compiler pushed it above the value).
ObjClass* VM::popTypeClass(uint8_t type) {
    if ((type & kTypeMask) != kTypeInstance) return nullptr;
    Value cls = *--sp_;
    if (!cls.isObjType(ObjType::Class))
        throw runtimeError(Thrown::ClassCastException, "a type must be a class or a built-in type, not a value of type " + typeName(cls));
    return asClass(cls);
}

std::string VM::declaredTypeText(uint8_t type, const ObjClass* cls) const {
    return typeText(type, cls != nullptr ? cls->name->utf8() : std::string());
}

AstraThrow VM::typeMismatch(const std::string& variable, uint8_t type, const ObjClass* cls, Value v) {
    checkRange(type, v, "'" + variable + "'");
    return runtimeError(Thrown::ClassCastException, "cannot assign a value of type " + typeName(v) + " to '" + variable + "', which is declared as " +
                        declaredTypeText(type, cls));
}

// An int sum, difference, product or quotient whose result does not fit in an int (D27).
AstraThrow VM::overflowError(Op op, int64_t a, int64_t b) {
    const char* symbol = op == Op::Add ? "+" : op == Op::Sub ? "-" : op == Op::Mul ? "*" : "/";
    const std::string sum = op == Op::Sub && a == 0 ? "-(" + std::to_string(b) + ")" : std::to_string(a) + " " + symbol + " " + std::to_string(b);
    std::string message = sum + " does not fit in an int, which holds -9223372036854775808 to 9223372036854775807";
    if (op != Op::Div && !(op == Op::Sub && a == 0)) message += std::string("; write &") + symbol + " for an answer that wraps around";
    return runtimeError(Thrown::ArithmeticException, message);
}

// Arithmetic on two numbers, ints or floats, for an operator the interpreter's quick path did
// not take (a byte among them, made an int).
bool VM::numberArithmetic(Op op) {
    const Value a = sp_[-2], b = sp_[-1];
    switch (op) {
        case Op::Shl:
        case Op::Shr:
        case Op::Ushr: return shift(op);
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor: return bitwise(op);
        default: break;
    }
    Value result;
    if (a.isInt() && b.isInt()) {
        const int64_t x = a.as.i, y = b.as.i;
        int64_t n = 0;
        switch (op) {
            case Op::Add: if (addOverflows(x, y, n)) throw overflowError(op, x, y); break;
            case Op::Sub: if (subOverflows(x, y, n)) throw overflowError(op, x, y); break;
            case Op::Mul: if (mulOverflows(x, y, n)) throw overflowError(op, x, y); break;
            case Op::AddWrap: n = static_cast<int64_t>(static_cast<uint64_t>(x) + static_cast<uint64_t>(y)); break;
            case Op::SubWrap: n = static_cast<int64_t>(static_cast<uint64_t>(x) - static_cast<uint64_t>(y)); break;
            case Op::MulWrap: n = static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y)); break;
            case Op::Div:
            case Op::Mod:
                if (y == 0) throw runtimeError(Thrown::ArithmeticException, "division by zero");
                if (op == Op::Div && y == -1 && x == INT64_MIN) throw overflowError(op, x, y);
                n = y == -1 ? (op == Op::Div ? -x : 0) : op == Op::Div ? x / y : x % y;
                break;
            default: throw runtimeError(Thrown::UnsupportedOperationException, "unsupported operand types for '" + opNames_[static_cast<int>(op)]->utf8() + "': int and int");
        }
        result = Value::integer(n);
    } else {
        const double x = a.toDouble(), y = b.toDouble();
        switch (op) {
            case Op::Add: result = Value::number(x + y); break;
            case Op::Sub: result = Value::number(x - y); break;
            case Op::Mul: result = Value::number(x * y); break;
            case Op::Div: result = Value::number(x / y); break;
            case Op::Mod: result = Value::number(std::fmod(x, y)); break;
            default: throw runtimeError(Thrown::UnsupportedOperationException, "unsupported operand types for '" + opNames_[static_cast<int>(op)]->utf8() + "': " + typeName(a) + " and " + typeName(b));
        }
    }
    sp_[-2] = result;
    sp_--;
    return false;
}

// a << b and a >> b on ints. Returns true if an overloaded operator was called instead.
bool VM::shift(Op op) {
    Value a = sp_[-2], b = sp_[-1];
    if (!a.isInt() || !b.isInt()) return binarySlow(op);

    int64_t count = b.as.i;
    if (count < 0) throw runtimeError(Thrown::ArithmeticException, "cannot shift by a negative amount (" + std::to_string(count) + ")");

    int64_t result;
    if (op == Op::Shl) {
        // Bits shifted past the top are lost; a count of 64 or more leaves nothing.
        result = count >= 64 ? 0 : static_cast<int64_t>(static_cast<uint64_t>(a.as.i) << count);
    } else if (op == Op::Ushr) {
        // `>>>` brings in zeros, the sign bit among the bits shifted (D44).
        result = count >= 64 ? 0 : static_cast<int64_t>(static_cast<uint64_t>(a.as.i) >> count);
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
            case ObjType::Set:
                return Value::object(alloc<ObjIterator>(iterable));
            case ObjType::Range: {
                if (asRange(iterable)->fromEnd()) throw rangeFromEnd(asRange(iterable), "a for loop");
                ObjIterator* it = alloc<ObjIterator>(iterable);
                it->nextValue = asRange(iterable)->start;
                it->remaining = asRange(iterable)->count();
                return Value::object(it);
            }
            case ObjType::Instance:
            case ObjType::Value: {
                // An Iterator itself (hasNext() and next()), or an object with an iterator() that
                // gives one, or gives something else that can be looped over: a list, say.
                if (isIteratorObject(iterable)) return Value::object(alloc<ObjIterator>(iterable));
                const Value* method = classOf(iterable)->methods.find(names.iterator);
                if (method == nullptr) break;
                TempRoot bound(*this, Value::object(alloc<ObjBoundMethod>(iterable, asClosure(*method))));
                Value inner = call(bound.get(), 0, nullptr);
                if (classOf(inner) != nullptr && !isIteratorObject(inner))
                    throw runtimeError(Thrown::IllegalStateException, "iterator() of " + typeName(iterable) + " has to give an Iterator, a list or another built-in collection");
                TempRoot keep(*this, inner);
                return makeIterator(inner);
            }
            default:
                break;
        }
    }
    throw runtimeError(Thrown::UnsupportedOperationException, "a value of type " + typeName(iterable) + " is not iterable");
}

// Part `index` of a value taken apart (D64): a list's item, or a data object's or a value's
// field, in the order of its header.
Value VM::componentOf(Value whole, size_t index) {
    guardRead(whole);
    static const char* const kOrdinals[] = {"first", "second", "third", "fourth", "fifth"};
    const std::string which = index < 5 ? kOrdinals[index] : std::to_string(index + 1) + "th";
    if (whole.isObjType(ObjType::List)) {
        ObjList* list = asList(whole);
        if (index >= list->size()) throw runtimeError(Thrown::IndexOutOfBoundsException, "cannot take a " + which + " part from a list of " + std::to_string(list->size()));
        return listGet(list, index);
    }
    if (classOf(whole) != nullptr && classOf(whole)->isData) {
        const ObjClass* cls = classOf(whole);
        if (index >= cls->components.size())
            throw runtimeError(Thrown::IndexOutOfBoundsException, "cannot take a " + which + " part from " + cls->name->utf8() + ", which has " +
                               std::to_string(cls->components.size()));
        return getProperty(whole, cls->components[index]);
    }
    throw runtimeError(Thrown::UnsupportedOperationException, "cannot take apart a value of type " + typeName(whole) + ": only a list, a data object or a value has parts");
}

// ---- Sets (M6.6 part 8), and views of a map's keys and entries (D77) ----

ObjSet* VM::newSet(ObjTypeInfo* type) {
    ObjSet* set = alloc<ObjSet>();
    set->type = type;
    return set;
}

// A map's keys, or its entries, as a set that shows the map as it is and cannot be changed.
Value VM::mapView(ObjMap* map, bool entries) {
    ObjTypeInfo* item = mapPartType(map, entries ? 2 : 0);
    ObjSet* view = newSet(item != nullptr && !item->args.empty() ? typeOf(kTypeSet, false, nullptr, {item->args[0]}) : nullptr);
    view->viewOf = map;
    view->entries = entries;
    return Value::object(view);
}

bool VM::setContains(const ObjSet* set, Value item) {
    if (!set->view()) return set->items.find(item) != nullptr;
    if (set->values) {
        // A value is there when some key has it: each looked at in turn.
        for (const ValueMap::Entry& entry : set->viewOf->map.entries())
            if (entry.live && equals(entry.value, item)) return true;
        return false;
    }
    if (!set->entries) return set->viewOf->map.find(item) != nullptr;
    // An entry, a MapEntry or a pair, is there when its key is, with its value.
    if (!item.isObjType(ObjType::Value) && !item.isSmall() && !item.isRef()) return false;
    const ObjClass* cls = classOf(item);
    if (cls == nullptr || cls->components.size() != 2) return false;
    const Value key = componentOf(item, 0), value = componentOf(item, 1);
    const Value* held = set->viewOf->map.find(key);
    return held != nullptr && equals(*held, value);
}

AstraThrow VM::viewChanged(const ObjSet* view) {
    return runtimeError(Thrown::UnsupportedOperationException, std::string("a map's ") + (view->values ? "values" : view->entries ? "entries" : "keys") +
                        " show the map and cannot be changed through it: change the map, with remove, removeAll { (k, v) -> } or retainAll");
}

bool VM::setAdd(ObjSet* set, Value item) {
    if (set->view()) throw viewChanged(set);
    if (set->items.find(item) != nullptr) return false;
    set->items.set(own(item), Value::null());
    account(64);
    return true;
}

bool VM::setRemove(ObjSet* set, Value item) {
    if (set->view()) throw viewChanged(set);
    return set->items.remove(item);
}

// ---- Keys and frozen collections (D26) ----

// While a key's == or hashCode runs, nothing is collected (VM::keyCalls_).
struct KeyCall {
    VM& vm;
    explicit KeyCall(VM& v) : vm(v) { vm.keyCalls_++; }
    ~KeyCall() { vm.keyCalls_--; }
};

// Whether the class has an == or a hashCode of its own, asked once.
static bool hasOwnKeys(const ObjClass* cls, const Names& names) {
    if (cls->ownKeys == 0)
        const_cast<ObjClass*>(cls)->ownKeys = cls->methods.find(names.opEq) != nullptr || cls->methods.find(names.hashCode) != nullptr ? 2 : 1;
    return cls->ownKeys == 2;
}

bool VM::hashOf(const ObjClass* cls, const Value& v, size_t& out) {
    if (!hasOwnKeys(cls, names)) return false;
    const Value* method = cls->methods.find(names.hashCode);
    if (method == nullptr || !method->isObjType(ObjType::Closure)) return false;
    TempRoot receiver(*this, boxed(v));
    TempRoot bound(*this, Value::object(alloc<ObjBoundMethod>(receiver.get(), asClosure(*method))));
    KeyCall inKey(*this);
    const Value result = call(bound.get(), 0, nullptr);
    if (!result.isInt()) throw runtimeError(Thrown::ClassCastException, cls->name->utf8() + ".hashCode() gave " + typeName(result) + ", not an int");
    out = static_cast<size_t>(result.as.i);
    return true;
}

bool VM::equalOf(const ObjClass* cls, const Value& a, const Value& b, bool& out) {
    if (!hasOwnKeys(cls, names)) return false;
    const Value* method = cls->methods.find(names.opEq);
    if (method == nullptr || !method->isObjType(ObjType::Closure)) return false;
    TempRoot receiver(*this, boxed(a));
    TempRoot other(*this, boxed(b));
    TempRoot bound(*this, Value::object(alloc<ObjBoundMethod>(receiver.get(), asClosure(*method))));
    const Value given = other.get();
    KeyCall inKey(*this);
    out = call(bound.get(), 1, &given).truthy();
    return true;
}

void VM::refuseKey(const Value& key) {
    if (key.isObj() && key.as.obj->frozen == 2)
        throw runtimeError(Thrown::IllegalArgumentException, "this frozen " + typeName(key) + " holds something that can change, so it cannot be a key");
    if (key.isObjType(ObjType::Bytes))
        throw runtimeError(Thrown::IllegalArgumentException, "bytes can change, so they cannot be a key: a frozen list of them can, key.toList().frozen()");
    if (key.isObjType(ObjType::List) || key.isObjType(ObjType::Map) || key.isObjType(ObjType::Set))
        throw runtimeError(Thrown::IllegalArgumentException, "a " + typeName(key) + " can change, so it cannot be a key: a frozen copy can, key.frozen()");
    const std::string name = typeName(key);
    const bool vowel = !name.empty() && std::string("AEIOUaeiou").find(name[0]) != std::string::npos;
    throw runtimeError(Thrown::IllegalArgumentException, (vowel ? "an " : "a ") + name + " can change in what its == looks at, so it cannot be a key");
}

// A frozen collection's values hold frozen copies of the lists, maps and sets they hold, as the
// collection itself does (G27): what it holds shares nothing with what it was made from. Objects
// of classes are left as they are, by identity (D26).
void VM::freezeInside(uint8_t* at, const ValueLayout& layout, int depth) {
    for (uint32_t offset : layout.objects) {
        Obj* held;
        std::memcpy(&held, at + offset, sizeof held);
        if (held == nullptr || (held->type != ObjType::List && held->type != ObjType::Map && held->type != ObjType::Set && held->type != ObjType::Value)) continue;
        const Value made = frozenCopy(Value::object(held), depth + 1);
        std::memcpy(at + offset, &made.as.obj, sizeof made.as.obj);
    }
    for (uint32_t offset : layout.values) {
        Value held;
        std::memcpy(&held, at + offset, sizeof held);
        if (!held.isObj()) continue;
        const Value made = frozenCopy(held, depth + 1);
        std::memcpy(at + offset, &made, sizeof made);
    }
}

Value VM::frozenCopy(Value v, int depth) {
    if (!v.isObj() || v.as.obj->frozen != 0) return v;
    if (v.isObjType(ObjType::Value)) {
        // A value in a box of its own: a copy of it, what it holds frozen (G27).
        const ObjValue* box = asValue(v);
        const ValueLayout& layout = layoutOf(box->cls);
        if (layout.objects.empty() && layout.values.empty()) return v;
        if (depth >= kMaxEqualsDepth) throw runtimeError(Thrown::IllegalArgumentException, "frozen(): the " + typeName(v) + " holds itself, or is nested too deeply to copy");
        ObjValue* made = newValue(box->cls);
        TempRoot keep(*this, Value::object(made));
        std::memcpy(made->data(), box->data(), layout.size);
        freezeInside(made->data(), layout, depth);
        return keep.get();
    }
    if (v.isObjType(ObjType::Set) && asSet(v)->values) {
        // A map's values (G22), frozen: a list of them as they are.
        ObjList* list = newList(*this);
        TempRoot keep(*this, Value::object(list));
        list->values() = setItems(asSet(v));
        account(list->values().size() * sizeof(Value));
        return frozenCopy(Value::object(list), depth);
    }
    const ObjType type = v.as.obj->type;
    if (type != ObjType::List && type != ObjType::Map && type != ObjType::Set) return v;
    if (depth >= kMaxEqualsDepth) throw runtimeError(Thrown::IllegalArgumentException, "frozen(): the " + typeName(v) + " holds itself, or is nested too deeply to copy");
    guardRead(v);
    // Whether all it holds could be a key as well.
    bool stable = true;
    auto inner = [&](Value item) {
        const Value made = frozenCopy(item, depth + 1);
        stable = stable && stableKey(made);
        return made;
    };
    if (type == ObjType::List) {
        ObjList* list = asList(v);
        ObjList* made = listLike(list);
        TempRoot keep(*this, Value::object(made));
        if (list->packed()) {
            listCopyRange(made, list, 0, list->size());
            if (list->store() == ListStore::Items) {
                if (list->pack->itemClass->unstableKey) stable = false;
                // Each item's lists, maps and sets frozen too (G27), the items as they were copied.
                const ValueLayout& layout = layoutOf(list->pack->itemClass);
                if (!layout.objects.empty() || !layout.values.empty())
                    for (size_t i = 0; i < made->size(); i++) freezeInside(itemAt(made, i), layout, depth);
            }
        } else {
            for (size_t i = 0; i < list->size(); i++) {
                const Value item = inner(list->values()[i]);
                made->values().push_back(item);
            }
        }
        made->frozen = stable ? 1 : 2;
        return keep.get();
    }
    if (type == ObjType::Map) {
        ObjMap* map = asMap(v);
        ObjMap* made = alloc<ObjMap>();
        made->type = map->type;
        TempRoot keep(*this, Value::object(made));
        // By position: a key's hashCode, run as it goes in, may change the map.
        for (size_t i = 0; i < map->map.entries().size(); i++) {
            const ValueMap::Entry entry = map->map.entries()[i];
            if (!entry.live) continue;
            const Value value = inner(entry.value);
            made->map.set(entry.key, value);
        }
        account(made->map.size() * 2 * sizeof(Value));
        made->frozen = stable ? 1 : 2;
        return keep.get();
    }
    const ObjSet* set = asSet(v);
    ObjSet* made = newSet(set->type);
    TempRoot keep(*this, Value::object(made));
    for (const Value& item : setItems(set)) made->items.set(item, Value::null());
    made->frozen = 1;
    return keep.get();
}

// ---- Lazy values (D37) ----

// Tells the threads waiting for a lazy value that it is there, or that working it out failed.
void VM::wakeLazyWaiters(ObjLazy* lazy) {
    if (lazy->waiters == 0) return;
    const Value key = Value::object(lazy);
    monitorEnter(key);
    monitorNotify(key, true);
    monitorExit(key);
}

Value VM::forceLazy(Value marker, const std::function<void(Value)>& store, const std::string& what) {
    static thread_local char here;
    const uintptr_t me = reinterpret_cast<uintptr_t>(&here);
    ObjLazy* lazy = static_cast<ObjLazy*>(marker.as.obj);
    TempRoot keep(*this, Value::object(lazy));
    for (;;) {
        if (lazy->state == 2) {
            store(lazy->value);
            return lazy->value;
        }
        if (lazy->state == 1) {
            if (lazy->owner == me) throw runtimeError(Thrown::IllegalStateException, what + " is read while its value is being worked out: its starting value needs itself");
            if (inParallelSection()) {
                yieldLock();
                continue;
            }
            // Another thread is working it out: wait until it is told the value is there, or
            // that working it out failed.
            const Value key = Value::object(lazy);
            lazy->waiters++;
            bool interrupted = false;
            monitorEnter(key);
            while (lazy->state == 1)
                if (monitorWait(key, -1) == 2) interrupted = true;
            monitorExit(key);
            lazy->waiters--;
            if (interrupted) thread_->interrupted = true;  // for whoever asks next
            continue;
        }
        lazy->state = 1;
        lazy->owner = me;
        Value result;
        try {
            result = call(Value::object(lazy->init), 0, nullptr);
        } catch (...) {
            // A starting value that fails leaves it unset, for the next read to try again.
            lazy->state = 0;
            lazy->owner = 0;
            wakeLazyWaiters(lazy);
            throw;
        }
        lazy->value = own(result);
        lazy->state = 2;
        wakeLazyWaiters(lazy);
        store(lazy->value);
        return lazy->value;
    }
}

Value VM::globalOrLazy(size_t index) {
    const Value value = globals_[index];
    if (!value.isLazy()) throw runtimeError(Thrown::IllegalStateException, "undefined variable '" + program_->globalNames[index] + "'");
    return forceLazy(value, [&](Value v) { globals_[index] = v; }, "'" + program_->globalNames[index] + "'");
}

std::vector<Value> VM::setItems(const ObjSet* set) {
    std::vector<Value> out;
    const ValueMap& from = set->view() ? set->viewOf->map : set->items;
    out.reserve(from.size());
    TempVector keep(*this, out);
    for (size_t i = 0; i < from.entries().size(); i++) {
        const ValueMap::Entry& entry = from.entries()[i];
        if (!entry.live) continue;
        out.push_back(set->entries ? makeEntry(entry.key, entry.value) : set->values ? entry.value : entry.key);
    }
    return out;
}

std::vector<Value> VM::itemsOf(Value from, const char* what) {
    if (from.isObjType(ObjType::List)) return listCopy(asList(from));
    if (from.isObjType(ObjType::Set)) return setItems(asSet(from));
    const bool iterable = from.isObjType(ObjType::Range) || from.isString() || from.isObjType(ObjType::Bytes) || from.isObjType(ObjType::Map);
    if (!iterable) throw runtimeError(Thrown::ClassCastException, std::string(what) + " takes a list, a set, a range, a string, bytes or a map, not a value of type " + typeName(from));
    std::vector<Value> out;
    TempVector keep(*this, out);
    const Value iterator = makeIterator(from);
    TempRoot keepIterator(*this, iterator);
    Value item;
    while (iteratorNext(asIterator(iterator), item)) out.push_back(item);
    return out;
}

bool VM::iteratorNext(ObjIterator* it, Value& out) {
    Value source = it->source;
    switch (source.as.obj->type) {
        case ObjType::List: {
            ObjList* list = asList(source);
            if (it->index >= list->size()) return false;
            out = listGet(list, it->index++);
            return true;
        }
        case ObjType::Bytes: {
            const std::vector<uint8_t>& data = asBytes(source)->data;
            if (it->index >= data.size()) return false;
            out = Value::byte(data[it->index++]);
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
            const ObjString* text = asString(source);
            if (it->index >= text->length) return false;
            out = Value::character(text->at(it->index++));
            return true;
        }
        case ObjType::Set: {
            // A set's items; a view's, the map's keys or entries as they are (D77).
            const ObjSet* set = asSet(source);
            const std::vector<ValueMap::Entry>& entries = set->view() ? set->viewOf->map.entries() : set->items.entries();
            while (it->index < entries.size()) {
                const ValueMap::Entry& entry = entries[it->index++];
                if (!entry.live) continue;
                out = set->entries ? makeEntry(entry.key, entry.value) : set->values ? entry.value : entry.key;
                return true;
            }
            return false;
        }
        case ObjType::Map: {
            const std::vector<ValueMap::Entry>& entries = asMap(source)->map.entries();
            while (it->index < entries.size()) {
                const ValueMap::Entry& entry = entries[it->index++];
                if (entry.live) {
                    out = makeEntry(entry.key, entry.value);  // a map gives its entries (D64)
                    return true;
                }
            }
            return false;
        }
        case ObjType::Instance:
        case ObjType::Value: {
            // An Iterator: hasNext(), then next().
            const Value hasNext = memberGet(source, names.hasNext);
            TempRoot keepHas(*this, hasNext);
            if (!call(hasNext, 0, nullptr).truthy()) return false;
            const Value next = memberGet(source, names.next);
            TempRoot keepNext(*this, next);
            out = call(next, 0, nullptr);
            return true;
        }
        default:
            return false;
    }
}

bool VM::isIteratorObject(Value v) const {
    const ObjClass* cls = classOf(v);
    return cls != nullptr && cls->methods.find(names.hasNext) != nullptr && cls->methods.find(names.next) != nullptr;
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
        upvalue->closed = own(*upvalue->location);  // a variable kept on, past its frame
        upvalue->location = &upvalue->closed;
        upvalue->owner = nullptr;
        openUpvalues_ = upvalue->nextOpen;
        upvalue->nextOpen = nullptr;
    }
}

// ---- Interpreter loop ----

#ifdef ASTRA_OP_COUNTS
#include <intrin.h>
// A build for finding where a program's instructions go (M6.5 part 4): how often each runs,
// and each pair one after the other, shown when the program ends.
namespace {
struct OpCounts {
    uint64_t single[256] = {};
    uint64_t pairs[256][256] = {};
    uint64_t cycles[256] = {};  // the processor's clock from each instruction to the next, by the first
    uint64_t lastTime = 0;
    int last = 255;
    ~OpCounts() {
        uint64_t allCycles = 0;
        std::vector<std::pair<uint64_t, std::string>> slow;
        for (int i = 0; i < 256; i++) {
            allCycles += cycles[i];
            if (cycles[i] != 0) slow.emplace_back(cycles[i], opName(static_cast<Op>(i)));
        }
        std::sort(slow.rbegin(), slow.rend());
        std::fprintf(stderr, "time, by instruction (with what it calls):\n");
        for (size_t i = 0; i < slow.size() && i < 25; i++) {
            int op = 0;
            while (op < 255 && slow[i].second != opName(static_cast<Op>(op))) op++;
            std::fprintf(stderr, "  %-22s %5.1f%%  %8.1f cycles each\n", slow[i].second.c_str(),
                         100.0 * static_cast<double>(slow[i].first) / static_cast<double>(allCycles),
                         static_cast<double>(slow[i].first) / static_cast<double>(single[op] == 0 ? 1 : single[op]));
        }
        uint64_t total = 0;
        std::vector<std::pair<uint64_t, std::string>> rows;
        for (int i = 0; i < 256; i++) {
            total += single[i];
            if (single[i] != 0) rows.emplace_back(single[i], opName(static_cast<Op>(i)));
        }
        std::sort(rows.rbegin(), rows.rend());
        std::fprintf(stderr, "instructions run: %llu\n", static_cast<unsigned long long>(total));
        for (size_t i = 0; i < rows.size() && i < 25; i++)
            std::fprintf(stderr, "  %-22s %12llu  %5.1f%%\n", rows[i].second.c_str(), static_cast<unsigned long long>(rows[i].first),
                         100.0 * static_cast<double>(rows[i].first) / static_cast<double>(total));
        std::vector<std::pair<uint64_t, std::string>> twos;
        for (int a = 0; a < 256; a++)
            for (int b = 0; b < 256; b++)
                if (pairs[a][b] != 0)
                    twos.emplace_back(pairs[a][b], std::string(opName(static_cast<Op>(a))) + " " + opName(static_cast<Op>(b)));
        std::sort(twos.rbegin(), twos.rend());
        std::fprintf(stderr, "pairs:\n");
        for (size_t i = 0; i < twos.size() && i < 25; i++)
            std::fprintf(stderr, "  %-40s %12llu  %5.1f%%\n", twos[i].second.c_str(), static_cast<unsigned long long>(twos[i].first),
                         100.0 * static_cast<double>(twos[i].first) / static_cast<double>(total));
    }
};
OpCounts opCounts;
inline void countOp(uint8_t op) {
    const uint64_t now = __rdtsc();
    if (opCounts.last != 255) opCounts.cycles[opCounts.last] += now - opCounts.lastTime;
    opCounts.lastTime = now;
    opCounts.single[op]++;
    if (opCounts.last != 255) opCounts.pairs[opCounts.last][op]++;
    opCounts.last = op;
}
}  // namespace
#endif

// An instruction's operands, written low byte first, read with one load each where the
// machine keeps its numbers that way (M6.5 part 4).
inline int operandU16(const uint8_t* at) {
    uint16_t v;
    std::memcpy(&v, at, sizeof v);
    if constexpr (std::endian::native == std::endian::big) v = static_cast<uint16_t>((v >> 8) | (v << 8));
    return v;
}
inline int32_t operandI32(const uint8_t* at) {
    uint32_t v;
    std::memcpy(&v, at, sizeof v);
    if constexpr (std::endian::native == std::endian::big)
        v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    return static_cast<int32_t>(v);
}

void VM::run(int baseFrame) {
    // Maps and sets on this thread call this VM for a class's own == and hashCode (D26).
    struct KeysHere {
        KeyMethods* const saved = keyMethods;
        explicit KeysHere(KeyMethods* here) { keyMethods = here; }
        ~KeysHere() { keyMethods = saved; }
    } keysHere(this);
    CallFrame* frame = nullptr;
    const uint8_t* ip = nullptr;
    const uint8_t* code = nullptr;
    Value* base = nullptr;
    const Value* consts = nullptr;
    std::atomic<uint64_t>* caches = nullptr;

#define LOAD_FRAME()                                         \
    do {                                                     \
        frame = &frames_[static_cast<size_t>(frameCount_ - 1)]; \
        ip = frame->ip;                                      \
        base = frame->base;                                  \
        code = frame->closure->proto->code.data();           \
        consts = frame->closure->proto->constants.data();    \
        caches = frame->closure->proto->caches.get();        \
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
                              (classOf(target_) != nullptr &&                                    \
                               ((classOf(target_)->accessBits >> (name->hash & 63) & 1) == 0 ||  \
                                (classOf(target_)->number == proto_->ownClass &&                 \
                                 (classOf(target_)->accessAllOwn ||                              \
                                  (classOf(target_)->accessSharedOwn && (name->restricted & 4) == 0))))); \
            if (!own_ && accessDenied(target_, name, proto_, (write))) {                         \
                SAVE_IP();                                                                       \
                throw accessError(target_, name, proto_);                                        \
            }                                                                                    \
        }                                                                                        \
    } while (0)

#define READ_U8() (*ip++)
#define READ_U16() (ip += 2, operandU16(ip - 2))
#define READ_I32() (ip += 4, operandI32(ip - 4))

// Where the collector may run, and where a thread gives way to another.
#define GC_SAFE_POINT()                                                                    \
    do {                                                                                   \
        if (bytesAllocated_ > nextGC_ || attention_.load(std::memory_order_relaxed)) safePoint(); \
    } while (0)

// The fast path's operands, read where the instruction is, before ip moves past it.
#define FAST_U16(at) operandU16(ip + (at))
#define FAST_I32(at) operandI32(ip + (at))

// A call of a closure that needs nothing done before it starts (M6.5 part 4): as many arguments
// as it has parameters, none gathered into a list, no collection or other thread waiting, and
// room for its frame. Its frame is pushed here and the loop goes on in it; anything else falls
// through to the instruction's own code. `size` is the length of the instruction's operands.
#define QUICK_CALL(callee, argc, size)                                                                        \
    {                                                                                                         \
        ObjClosure* const quick_ = (callee);                                                                  \
        const FunctionProto* const proto_ = quick_->proto;                                                    \
        Value* const base_ = sp - (argc) - 1;                                                                 \
        if ((argc) == proto_->arity && !proto_->chooser && proto_->varargIndex < 0 && pendingEnums_ == 0 &&   \
            (proto_->typeArgs & ~kTakesDestination) == 0 &&                                                   \
            frameCount_ < frameLimit_ && stackEnd_ - base_ >= proto_->maxStack && bytesAllocated_ <= nextGC_ &&\
            !attention_.load(std::memory_order_relaxed)) {                                                    \
            frame->ip = ip + (size);                                                                          \
            CallFrame& next_ = frames_[static_cast<size_t>(frameCount_++)];                                   \
            next_.dropResult = false; next_.repeats = 0;                                                                         \
            next_.closure = quick_;                                                                           \
            next_.ip = proto_->code.data();                                                                   \
            next_.base = base_;                                                                               \
            /* Where its result goes, for a function that takes it; for any other, none (M6.6 part 4). */  \
            if (proto_->typeArgs != 0) frameDestination(frameCount_ - 1) = pendingDestination_;               \
            pendingDestination_ = Value::null();                                                              \
            LOAD_FRAME();                                                                                     \
            continue;                                                                                         \
        }                                                                                                     \
    }

// Whether `v` is certainly of `type`, told at once for the types written most; false means only
// that the full test (matchesType) has to be made.
#define FITS_AT_ONCE(v, type) ((type) == kTypeFloat ? (v).isFloat() : (type) == kTypeInt ? (v).isInt() : (type) == kTypeBool && (v).isBool())

// Two operands, `a` kept where it is, on the stack's top held in a register: two floats or two
// ints are worked out at once, and `done` ends the instruction; anything else falls through to
// the instruction's own code, the stack as it was (M6.5 part 4).
// Two ints whose result does not fit (D27) fall through as well, to be told so.
#define FAST_ARITHMETIC(a, b, op, checked, done)                              \
    if ((a).isFloat() && (b).isFloat()) {                                     \
        (a).setFloat((a).as.d op (b).as.d);                                   \
        done;                                                                 \
    } else if ((a).isInt() && (b).isInt()) {                                  \
        int64_t sum_;                                                         \
        if (!checked((a).as.i, (b).as.i, sum_)) {                             \
            (a).setInt(sum_);                                                 \
            done;                                                             \
        }                                                                     \
    }
#define FAST_FLOAT(a, b, op, done)                                            \
    if ((a).isFloat() && (b).isFloat()) {                                     \
        (a).setFloat((a).as.d op (b).as.d);                                   \
        done;                                                                 \
    }
#define FAST_COMPARISON(a, b, cmp, done)                                      \
    if ((a).isFloat() && (b).isFloat()) {                                     \
        (a).setBool((a).as.d cmp (b).as.d);                                   \
        done;                                                                 \
    } else if ((a).isInt() && (b).isInt()) {                                  \
        (a).setBool((a).as.i cmp (b).as.i);                                   \
        done;                                                                 \
    }

#define ARITHMETIC(op, checked, floatExpr)                                 \
    {                                                                     \
        Value b = sp_[-1], a = sp_[-2];                                   \
        if (a.isInt() && b.isInt()) {                                     \
            int64_t sum_;                                                 \
            if (checked(a.as.i, b.as.i, sum_)) throw overflowError(op, a.as.i, b.as.i); \
            sp_[-2].setInt(sum_);                                         \
            sp_--;                                                        \
        } else if (a.isNumber() && b.isNumber()) {                        \
            double x = a.toDouble(), y = b.toDouble();                    \
            sp_[-2].setFloat(floatExpr);                                  \
            sp_--;                                                        \
        } else {                                                          \
            SAVE_IP();                                                    \
            if (binarySlow(op)) LOAD_FRAME();                             \
        }                                                                 \
        break;                                                            \
    }

// The joined instructions (M6.5 part 4): `right` is the local or constant the second one names.
// Two floats or two ints are worked out here; anything else is pushed, and goes the way of the
// plain instruction at `plain`.
#define JOINED_ARITHMETIC(right, checked, floatExpr, plain)           \
    {                                                                 \
        Value b = (right), a = sp_[-1];                               \
        int64_t sum_;                                                 \
        if (a.isFloat() && b.isFloat()) {                             \
            double x = a.as.d, y = b.as.d;                            \
            sp_[-1].setFloat(floatExpr);                              \
            (void)0;                                                  \
        } else if (a.isInt() && b.isInt() && !checked(a.as.i, b.as.i, sum_)) { \
            sp_[-1].kind = ValueKind::Int;                            \
            sp_[-1].as.i = sum_;                                      \
        } else {                                                      \
            *sp_++ = b;                                               \
            goto plain;                                               \
        }                                                             \
        break;                                                        \
    }

// The same where only floats are worked out here (a division: ints have rules of their own).
#define JOINED_FLOAT(right, floatExpr, plain)                         \
    {                                                                 \
        Value b = (right), a = sp_[-1];                               \
        if (a.isFloat() && b.isFloat()) {                             \
            double x = a.as.d, y = b.as.d;                            \
            sp_[-1].setFloat(floatExpr);                              \
            (void)0;                                                  \
        } else {                                                      \
            *sp_++ = b;                                               \
            goto plain;                                               \
        }                                                             \
        break;                                                        \
    }

#define JOINED_COMPARISON(right, cmp, plain)                          \
    {                                                                 \
        Value b = (right), a = sp_[-1];                               \
        if (a.isFloat() && b.isFloat()) {                             \
            sp_[-1].kind = ValueKind::Bool;                           \
            sp_[-1].as.b = a.as.d cmp b.as.d;                         \
        } else if (a.isInt() && b.isInt()) {                          \
            sp_[-1].kind = ValueKind::Bool;                           \
            sp_[-1].as.b = a.as.i cmp b.as.i;                         \
        } else {                                                      \
            *sp_++ = b;                                               \
            goto plain;                                               \
        }                                                             \
        break;                                                        \
    }


#define COMPARISON(op, cmp)                                               \
    {                                                                     \
        Value b = sp_[-1], a = sp_[-2];                                   \
        if (a.isInt() && b.isInt()) {                                     \
            sp_[-2].setBool(a.as.i cmp b.as.i);                           \
            sp_--;                                                        \
        } else if (a.isNumber() && b.isNumber()) {                        \
            sp_[-2].setBool(a.toDouble() cmp b.toDouble());               \
            sp_--;                                                        \
        } else {                                                          \
            SAVE_IP();                                                    \
            if (compareSlow(op)) LOAD_FRAME();                            \
        }                                                                 \
        break;                                                            \
    }

    // An error the program may catch: the innermost handler that takes it gets it, or, with
    // none in this run, it goes on to whoever called. Returns false in that case.
    // The frame that was running knows where it was: each instruction, as it starts, writes down
    // where it is (M6.5 part 4), so that the loop's own `ip` can stay in a register.
    auto handled = [&](AstraThrow& thrown) {
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

            // The stack's top, held in a register while the loop runs (M6.5 part 4). It is written
            // back to sp_ before each instruction, a store nothing waits on, so that every
            // instruction not given a quick way below works on sp_ as it always has; after one
            // of those, it is read back. The quick ones work on it here and go straight on.
            Value* sp = sp_;
            for (;;) {
#ifdef ASTRA_OP_COUNTS
                countOp(*ip);
#endif
                sp_ = sp;
                frame->ip = ++ip;
                switch (static_cast<Op>(ip[-1])) {
                    case Op::Const:
                        *sp++ = consts[READ_U16()];
                        continue;
                    case Op::Null:
                        *sp++ = Value::null();
                        continue;
                    case Op::True:
                        (sp++)->setBool(true);
                        continue;
                    case Op::False:
                        (sp++)->setBool(false);
                        continue;
                    case Op::Pop:
                        sp--;
                        continue;
                    case Op::Dup:
                        sp[0] = sp[-1];
                        sp++;
                        continue;
                    case Op::Dup2:
                        sp_[0] = sp_[-2];
                        sp_[1] = sp_[-1];
                        sp_ += 2;
                        break;

                    case Op::GetLocal:
                        *sp++ = base[READ_U16()];
                        continue;
                    case Op::SetLocal:
                        base[READ_U16()] = sp[-1];
                        continue;

                    case Op::GetGlobal: {
                        if (const Value global = globals_[static_cast<size_t>(FAST_U16(0))]; global.kind != ValueKind::Undefined) {
                            *sp++ = global;
                            ip += 2;
                            continue;
                        }
                        int index = READ_U16();
                        Value value = globals_[static_cast<size_t>(index)];
                        if (value.kind == ValueKind::Undefined) {
                            SAVE_IP();
                            value = globalOrLazy(static_cast<size_t>(index));  // a lazy one's (D37)
                        }
                        *sp_++ = value;
                        break;
                    }
                    case Op::SetGlobal: {
                        int index = READ_U16();
                        if (coreId_ != 0) throw notOwned("the variable '" + program_->globalNames[static_cast<size_t>(index)] + "' is shared");
                        Value& slot = globals_[static_cast<size_t>(index)];
                        if (slot.kind == ValueKind::Undefined && !slot.isLazy())  // a lazy var may be given one first (D37)
                            throw runtimeError(Thrown::IllegalStateException, "undefined variable '" + program_->globalNames[static_cast<size_t>(index)] + "'");
                        const TypeConstraint& declared = globalTypes_[static_cast<size_t>(index)];
                        if (declared.type != 0 && (!matchesType(sp_[-1], declared.type, declared.cls) && !widenedByte(sp_[-1], declared.type)))
                            throw typeMismatch(program_->globalNames[static_cast<size_t>(index)], declared.type,
                                               declared.cls, sp_[-1]);
                        slot = own(sp_[-1]);
                        break;
                    }
                    case Op::DefineGlobal: {
                        size_t index = static_cast<size_t>(READ_U16());
                        if (coreId_ != 0) throw notOwned("'" + program_->globalNames[index] + "' is declared for the whole program");
                        globals_[index] = own(*--sp_);
                        globalTypes_[index] = TypeConstraint{};
                        break;
                    }

                    case Op::GetImplicit: {
                        ObjString* name = asString(consts[READ_U16()]);
                        size_t index = static_cast<size_t>(READ_U16());
                        Value receiver = implicitReceiver(frame, base, name);
                        if (receiver.kind == ValueKind::Undefined) {
                            Value value = globals_[index];
                            if (value.isLazy()) {
                                SAVE_IP();
                                value = globalOrLazy(index);
                            }
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
                        if (Value field; instance->read(name, field)) {
                            *sp_++ = fieldRead(field);
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
                        if (Value field; instance->read(name, field)) {
                            *sp_++ = fieldRead(field);
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
                            if (callee.isLazy()) callee = globalOrLazy(index);
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
                            if (slot.kind == ValueKind::Undefined && !slot.isLazy()) throw undefinedName(frame, base, name, index);
                            const TypeConstraint& declared = globalTypes_[index];
                            if (declared.type != 0 && (!matchesType(sp_[-1], declared.type, declared.cls) && !widenedByte(sp_[-1], declared.type)))
                                throw typeMismatch(program_->globalNames[index], declared.type, declared.cls, sp_[-1]);
                            slot = own(sp_[-1]);  // as SetGlobal: an item where it is is copied as it is kept
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
                                throw runtimeError(Thrown::IllegalStateException, "property '" + name->utf8() + "' of " + cls->name->utf8() +
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
                        if ((!matchesType(value, type, cls) && !widenedByte(value, type)))
                            throw typeMismatch(program_->globalNames[index], type, cls, value);
                        if (coreId_ != 0) throw notOwned("'" + program_->globalNames[index] + "' is declared for the whole program");
                        globals_[index] = own(value);
                        globalTypes_[index] = TypeConstraint{type, cls};
                        break;
                    }
                    case Op::IndexN:
                    case Op::SetIndexN: {
                        // `grid[x, y]` and `grid[x, y] = v` (D133): only a class's op [] takes several.
                        const bool set = static_cast<Op>(ip[-1]) == Op::SetIndexN;
                        const int count = READ_U8();
                        const Value obj = sp_[-count - 1 - (set ? 1 : 0)];
                        if (classOf(obj) == nullptr)
                            throw runtimeError(Thrown::UnsupportedOperationException,
                                               "a value of type " + typeName(obj) + " takes one index; [x, y] is for a class with op [](x, y)");
                        SAVE_IP();
                        invokeOperator(obj, set ? names.opIndexSet : names.opIndex, count + (set ? 1 : 0));
                        LOAD_FRAME();
                        break;
                    }
                    case Op::BundleValues:
                        bundleValues(READ_U8());
                        break;
                    case Op::MergeBundle:
                        mergeBundles();
                        break;
                    case Op::DupN: {
                        const int count = READ_U8();
                        for (int k = 0; k < count; k++) sp_[k] = sp_[k - count];
                        sp_ += count;
                        break;
                    }
                    case Op::SettleEnum: {
                        ObjClass* cls = popTypeClass(kTypeInstance);
                        if (sp_[-1].kind == ValueKind::EnumName) sp_[-1] = settleEnumName(sp_[-1], cls);
                        break;
                    }
                    case Op::CheckParam: {
                        ObjString* parameter = asString(consts[READ_U16()]);
                        uint8_t type = READ_U8();
                        ObjClass* cls = popTypeClass(type);
                        if (sp_[-1].kind == ValueKind::EnumName) sp_[-1] = settleEnumName(sp_[-1], cls);
                        if ((!matchesType(sp_[-1], type, cls) && !widenedByte(sp_[-1], type))) {
                            checkRange(type, sp_[-1], "argument '" + parameter->utf8() + "' of " + frame->closure->proto->name + "()");
                            throw runtimeError(Thrown::IllegalArgumentException, "argument '" + parameter->utf8() + "' of " + frame->closure->proto->name +
                                               "() must be " + declaredTypeText(type, cls) + ", got " + typeName(sp_[-1]));
                        }
                        break;
                    }
                    case Op::CheckReturn: {
                        if (FITS_AT_ONCE(sp[-1], ip[0])) {
                            ip++;
                            continue;
                        }
                        uint8_t type = READ_U8();
                        ObjClass* cls = popTypeClass(type);
                        if ((!matchesType(sp_[-1], type, cls) && !widenedByte(sp_[-1], type)))
                            throw runtimeError(Thrown::ClassCastException, frame->closure->proto->name + "() must return " + declaredTypeText(type, cls) +
                                               ", but returned " + typeName(sp_[-1]));
                        break;
                    }
                    case Op::CheckType: {
                        ObjString* variable = asString(consts[READ_U16()]);
                        uint8_t type = READ_U8();
                        ObjClass* cls = popTypeClass(type);
                        if ((!matchesType(sp_[-1], type, cls) && !widenedByte(sp_[-1], type))) throw typeMismatch(variable->utf8(), type, cls, sp_[-1]);
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
                            throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' is used before it has a value");
                        *sp_++ = value;
                        break;
                    }
                    case Op::MakeType:
                    case Op::CachedType:
                    case Op::CacheType:
                    case Op::TypeCollection:
                    case Op::IsFull:
                    case Op::CastFull:
                    case Op::WithTypes:
                    case Op::FrameTypes:
                    case Op::TypeArg:
                    case Op::InstanceTypeArg:
                    case Op::SupertypeMaker:
                    case Op::CheckResult:
                    case Op::ClassOf:
                    case Op::ClassMeta:
                    case Op::AtomicField:
                        ip = typeInstruction(static_cast<Op>(ip[-1]), ip, frame);
                        break;

                    case Op::GetCaptured: {
                        const ObjUpvalue* cell = capturedOf(base[0], READ_U16());
                        ObjString* name = asString(consts[READ_U16()]);
                        const Value value = *cell->location;
                        if (value.kind == ValueKind::Undefined)
                            throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' is used before it has a value");
                        *sp_++ = value;
                        break;
                    }
                    case Op::SetCaptured: {
                        ObjUpvalue* cell = capturedOf(base[0], READ_U16());
                        // As SetUpvalue: a variable of the code around a section is nobody's to change there.
                        if (cell->core != coreId_) throw notOwned("a variable from outside the section is shared");
                        *cell->location = sp_[-1];
                        break;
                    }

                    case Op::ObjectClass: {
                        const int global = READ_U16();
                        const int target = READ_I32();
                        if (global != 0xFFFF) {
                            const Value kept = globals_[static_cast<size_t>(global)];
                            if (kept.isObjType(ObjType::Class)) {
                                *sp_++ = kept;
                                ip = code + target;
                            }
                        }
                        break;
                    }
                    case Op::StoreClass: {
                        const int global = READ_U16();
                        // A core of a parallel section makes its own each time: the program's
                        // globals are not its to change.
                        if (global != 0xFFFF && coreId_ == 0) globals_[static_cast<size_t>(global)] = sp_[-1];
                        break;
                    }
                    case Op::Capture: {
                        const int index = READ_U16();
                        const int kind = READ_U8();
                        ObjUpvalue* cell = nullptr;
                        if (kind == 0) {
                            cell = captureUpvalue(base + index);
                        } else if (kind == 1) {
                            if (static_cast<size_t>(index) >= frame->closure->upvalues.size()) throw runtimeError(Thrown::InternalError, "invalid bytecode: no such upvalue");
                            cell = frame->closure->upvalues[static_cast<size_t>(index)];
                        } else if (kind == 2) {
                            cell = capturedOf(base[0], index);
                        } else {
                            // `super`: the class's superclass (null if it has only interfaces), in a cell of its own.
                            const Value made = sp_[-1 - index];
                            if (!made.isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid anonymous object");
                            ObjClass* superclass = asClass(made)->super;
                            cell = alloc<ObjUpvalue>(nullptr);
                            cell->closed = superclass != nullptr ? Value::object(superclass) : Value::null();
                            cell->location = &cell->closed;
                        }
                        *sp_++ = Value::object(cell);
                        break;
                    }
                    case Op::NewObject: {
                        const int count = READ_U8();
                        const Value made = sp_[-count - 1];
                        if (!made.isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid anonymous object");
                        ObjClass* cls = asClass(made);
                        SAVE_IP();
                        if (cls->staticInit != nullptr) ensureClassInit(cls);  // with the captures still on the stack
                        // The captures stay just above the stack's top until the object has them.
                        sp_ -= count;
                        if (construct(cls, 0, false, sp_, static_cast<uint16_t>(count))) LOAD_FRAME();
                        break;
                    }

                    case Op::Unset:
                        *sp_++ = Value::undefined();
                        break;
                    case Op::SetUpvalue: {
                        ObjUpvalue* upvalue = frame->closure->upvalues[static_cast<size_t>(READ_U16())];
                        // A variable of the code around the section is every core's to read, and nobody's to change.
                        if (upvalue->core != coreId_) throw notOwned("a variable from outside the section is shared");
                        // An item of a list where it is is copied as it is kept: in a variable of
                        // the code around, it would outlive what the checker judged of the lambda.
                        *upvalue->location = own(sp_[-1]);
                        break;
                    }

                    case Op::GetOwn:
                    case Op::GetProp:
                    case Op::GetOwnRef:
                    case Op::GetPropRef: {
                        // Where this place has found the field before, in an object of the same
                        // layout, it is there again (M6.5 part 4): nothing else about it has
                        // changed, the right to use it included. A value kept inside the object
                        // (M6.6 part 5) is read where it is only by GetPropRef and GetOwnRef.
                        if (sp[-1].isObjType(ObjType::Instance) && caches != nullptr) {
                            const ObjInstance* instance = asInstance(sp[-1]);
                            const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
                            if (static_cast<uint32_t>(seen >> 32) == instance->shape &&
                                (instance->core == 0 || instance->core == coreId_)) {
                                if ((seen & 0x80) != 0) {
                                    // A field kept as its payload alone (M6.6 part 6).
                                    {
                                        // A value's 8 bytes of contents, and its kind as remembered.
                                        sp[-1].kind = static_cast<ValueKind>(seen & 3);
                                        std::memcpy(&sp[-1].as.i, reinterpret_cast<const uint8_t*>(instance) + ((seen >> 16) & 0xFFFF), 8);
                                        ip += 4;
                                        continue;
                                    }
                                }
                                const Value field = *reinterpret_cast<const Value*>(reinterpret_cast<const uint8_t*>(instance) + ((seen >> 16) & 0xFFFF));
                                if (field.kind == ValueKind::Ref && static_cast<Op>(ip[-1]) < Op::GetPropRef) {
                                    // A value kept inside the object, read through at once with
                                    // the fields read from it next (`comp.bounds.width`); also
                                    // past a `?.`, the value being there (`comp.damage?.width`, G16).
                                    const uint8_t* next = static_cast<Op>(ip[4]) == Op::JumpIfNull ? ip + 4 + 1 + 4 : ip + 4;
                                    if (const Op after = static_cast<Op>(*next); after == Op::GetProp || after == Op::GetOwn) {
                                        ValueField whole;
                                        whole.kind = ValueField::Nested;
                                        whole.cls = refClass(field);
                                        Value read;
                                        ip = readThrough(refBytes(field), &whole, next, consts, read);
                                        sp[-1] = read;
                                        continue;
                                    }
                                } else if (field.kind != ValueKind::Undefined) {
                                    sp[-1] = field;
                                    ip += 4;
                                    continue;
                                }
                            }
                        } else if (sp[-1].isObjType(ObjType::Value) && caches != nullptr) {
                            // A field of a value, where this place found it before (M6.6 part 2).
                            const ObjValue* box = asValue(sp[-1]);
                            const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
                            if (static_cast<uint32_t>(seen >> 32) == box->cls->shape) {
                                const ValueField& field = box->cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
                                if (field.kind == ValueField::Nested) {
                                    // A value held in this one: read through at once with the
                                    // fields read from it next (`sizes.pref.width`), or where it
                                    // is in the box, which never changes.
                                    if (const Op after = static_cast<Op>(ip[4]); after == Op::GetProp || after == Op::GetOwn) {
                                        Value read;
                                        ip = readThrough(box->data() + field.offset, &field, ip + 4, consts, read);
                                        sp[-1] = read;
                                        continue;
                                    }
                                    sp[-1] = Value::ref(sp[-1].as.obj, 0, static_cast<uint16_t>(field.offset), 1);
                                } else {
                                    sp[-1] = readPlainField(box->data() + field.offset, field);
                                }
                                ip += 4;
                                continue;
                            }
                        } else if (sp[-1].isSmall() && caches != nullptr) {
                            // A field of a value kept in the value itself (M6.6 part 5).
                            const ObjClass* cls = smallClassOf(sp[-1]);
                            const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
                            if (static_cast<uint32_t>(seen >> 32) == cls->shape) {
                                const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
                                const Value read = readPlainField(smallBytes(sp[-1]) + field.offset, field);
                                sp[-1] = read;
                                ip += 4;
                                continue;
                            }
                        } else if (sp[-1].isRef() && sp[-1].refDepth == 0 && caches != nullptr) {
                            // A field of a value where it is kept, an item of a list say (M6.6 part 4).
                            const Value r = sp[-1];
                            const ObjClass* cls = refHolderClass(r);
                            const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
                            if (cls != nullptr && static_cast<uint32_t>(seen >> 32) == cls->shape) {
                                if (const uint8_t* at = refQuickly(r, false)) {
                                    const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
                                    if (field.kind == ValueField::Nested) {
                                        if (const Op after = static_cast<Op>(ip[4]); after == Op::GetProp || after == Op::GetOwn) {
                                            Value read;
                                            ip = readThrough(at + field.offset, &field, ip + 4, consts, read);
                                            sp[-1] = read;
                                            continue;
                                        }
                                        sp[-1] = Value::ref(r.as.obj, r.refIndex, static_cast<uint16_t>(r.refOffset + field.offset), 1);
                                    } else {
                                        sp[-1] = readPlainField(at + field.offset, field);
                                    }
                                    ip += 4;
                                    continue;
                                }
                            }
                        }
                        const Op getOp = static_cast<Op>(ip[-1]);
                        const bool own = getOp == Op::GetOwn || getOp == Op::GetOwnRef;
                        ObjString* name = asString(consts[READ_U16()]);
                        const int cache = READ_U16();
                        Value obj = sp_[-1];
                        CHECK_ACCESS(obj, 1, false, !own);
                        if (obj.isObjType(ObjType::Instance)) {
                            ObjInstance* instance = asInstance(obj);
                            guardRead(instance);
                            const int64_t at = instance->fieldIndex(name);
                            if (at >= 0 && instance->fields()[at].kind != ValueKind::Undefined) {
                                const Value slot = instance->fields()[at];
                                if (slot.isRef() && getOp < Op::GetPropRef) {
                                    // A value kept inside the object, read on a copy; or read
                                    // through at once with the fields read from it next.
                                    if (const Op after = static_cast<Op>(*ip); after == Op::GetProp || after == Op::GetOwn ||
                                                                                 after == Op::GetPropRef || after == Op::GetOwnRef) {
                                        ValueField whole;
                                        whole.kind = ValueField::Nested;
                                        whole.cls = refClass(slot);
                                        Value read;
                                        ip = readThrough(refBytes(slot), &whole, ip, consts, read);
                                        sp_[-1] = read;
                                        break;
                                    }
                                    sp_[-1] = ownRef(slot);
                                } else {
                                    sp_[-1] = slot;
                                }
                                const uint64_t from = instance->slotsStart + static_cast<uint64_t>(at) * sizeof(Value);
                                if (caches != nullptr && from <= 0xFFFF)
                                    caches[cache].store(static_cast<uint64_t>(instance->shape) << 32 | from << 16, std::memory_order_relaxed);
                                break;
                            }
                            if (const FieldDecl* decl = instance->declOf(name); decl != nullptr && decl->payload != FieldDecl::NoPayload) {
                                // A field kept as its payload alone (M6.6 part 6), remembered so;
                                // a narrow one (D148) is read here each time.
                                sp_[-1] = instance->payloadOf(*decl);
                                if (caches != nullptr && decl->payload != FieldDecl::NarrowPayload)
                                    caches[cache].store(static_cast<uint64_t>(instance->shape) << 32 |
                                                            static_cast<uint64_t>(sizeof(ObjInstance) + decl->payloadOffset) << 16 |
                                                            static_cast<uint64_t>(decl->payloadBit) << 8 | 0x80u | decl->payloadValueKind(),
                                                        std::memory_order_relaxed);
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
                        } else if (obj.isObjType(ObjType::Value)) {
                            ObjValue* box = asValue(obj);
                            const int64_t at = valueFieldIndex(box, name);
                            if (at >= 0) {
                                const ValueField& field = layoutOf(box->cls).fields[static_cast<size_t>(at)];
                                sp_[-1] = field.kind == ValueField::Nested ? Value::ref(obj.as.obj, 0, static_cast<uint16_t>(field.offset), 1)
                                                                           : readValueField(box->data() + field.offset, field);
                                if (caches != nullptr && at <= 0xFFFF)
                                    caches[cache].store(static_cast<uint64_t>(box->cls->shape) << 32 | static_cast<uint64_t>(at) << 16,
                                                        std::memory_order_relaxed);
                                break;
                            }
                            if (const Value* getter = box->cls->getters.find(name)) {
                                SAVE_IP();
                                callClosure(asClosure(*getter), 0);
                                LOAD_FRAME();
                                break;
                            }
                        } else if (obj.isSmall()) {
                            const ObjClass* cls = smallClassOf(obj);
                            if (const Value* at = cls->declaredIndex.find(name)) {
                                const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[static_cast<size_t>(at->as.i)];
                                sp_[-1] = readValueField(smallBytes(obj) + field.offset, field);
                                if (caches != nullptr && at->as.i <= 0xFFFF)
                                    caches[cache].store(static_cast<uint64_t>(cls->shape) << 32 | static_cast<uint64_t>(at->as.i) << 16,
                                                        std::memory_order_relaxed);
                                break;
                            }
                        } else if (obj.isRef()) {
                            // A field of a value where it is kept (M6.6 part 4).
                            Value read;
                            if (refField(obj, name, read)) {
                                sp_[-1] = read;
                                if (caches != nullptr && obj.refDepth == 0) {
                                    const ObjClass* cls = refHolderClass(obj);
                                    const Value* found = cls != nullptr ? cls->declaredIndex.find(name) : nullptr;
                                    const int64_t at = found != nullptr ? found->as.i : -1;
                                    if (at >= 0 && at <= 0xFFFF)
                                        caches[cache].store(static_cast<uint64_t>(cls->shape) << 32 | static_cast<uint64_t>(at) << 16, std::memory_order_relaxed);
                                }
                                break;
                            }
                            if (const Value* getter = refClass(obj)->getters.find(name)) {
                                SAVE_IP();
                                callClosure(asClosure(*getter), 0);
                                LOAD_FRAME();
                                break;
                            }
                        }
                        sp_[-1] = getProperty(obj, name);
                        break;
                    }
                    case Op::SetOwn:
                    case Op::SetProp: {
                        // As for GetProp: a field found before, which takes any value or one of
                        // a type that is quickly told (never one of a class type). A val (the
                        // low bit) is given its value here only while it has none, as its
                        // object's init does.
                        if (sp[-2].isObjType(ObjType::Instance) && caches != nullptr) {
                            ObjInstance* instance = asInstance(sp[-2]);
                            const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
                            Value* const field = reinterpret_cast<Value*>(reinterpret_cast<uint8_t*>(instance) + ((seen >> 16) & 0xFFFF));
                            if ((seen & 0x80) != 0) {
                                // A field kept as its payload alone (M6.6 part 6): written there,
                                // where the value is of its kind (and a val has none yet).
                                if (static_cast<uint32_t>(seen >> 32) == instance->shape && instance->core == coreId_) {
                                    const Value value = sp[-1];
                                    const uint32_t bit = (seen >> 8) & 0xFF;
                                    const ValueKind kind = static_cast<ValueKind>(seen & 3);
                                    uint8_t* const data = instance->payloadData();
                                    const bool given = (seen & 0x20) != 0 && (data[bit / 8] >> (bit % 8) & 1) != 0;
                                    if (value.kind == kind && ((seen & 0x40) == 0 || !given)) {
                                        const int64_t bits = kind == ValueKind::Bool ? (value.as.b ? 1 : 0) : value.as.i;
                                        std::memcpy(reinterpret_cast<uint8_t*>(instance) + ((seen >> 16) & 0xFFFF), &bits, 8);
                                        if ((seen & 0x20) != 0) data[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
                                        sp[-2] = value;
                                        sp--;
                                        ip += 4;
                                        continue;
                                    }
                                }
                            } else if ((seen & 2) != 0) {
                                // A value kept inside the object, given another of its type (M6.6
                                // part 5): its bytes copied in, from a box or from where it is.
                                if (static_cast<uint32_t>(seen >> 32) == instance->shape && instance->core == coreId_ && field->isRef() && field->as.obj == instance) {
                                    const Value value = sp[-1];
                                    const ObjClass::InlineField* inside = instance->cls->inlineFieldAt(field->refOffset);
                                    const uint8_t* from = value.isObjType(ObjType::Value) && asValue(value)->cls == inside->cls ? asValue(value)->data()
                                                          : value.isRef() && refClass(value) == inside->cls          ? refQuickly(value, false)
                                                                                                                     : nullptr;
                                    if (from != nullptr) {
                                        std::memmove(instance->inlineData() + inside->offset, from, inside->size);
                                        sp[-2] = value;
                                        sp--;
                                        ip += 4;
                                        continue;
                                    }
                                }
                            } else if (static_cast<uint32_t>(seen >> 32) == instance->shape && instance->core == coreId_ &&
                                ((seen & 1) == 0 || field->kind == ValueKind::Undefined)) {
                                Value value = sp[-1];
                                const uint8_t type = static_cast<uint8_t>(seen >> 8);
                                if (type == 0 || (type == kTypeFloat ? value.isFloat()
                                                  : type == kTypeInt ? value.isInt()
                                                                     : (matchesType(value, type, nullptr) || widenedByte(value, type)))) {
                                    *field = value;
                                    sp[-2] = value;
                                    sp--;
                                    ip += 4;
                                    continue;
                                }
                            }
                        } else if (sp[-2].isRef() && sp[-2].refDepth == 0 && caches != nullptr) {
                            // A var field of a value where it is kept (M6.6 part 4): only an int, a
                            // float or a bool field is remembered, which takes its own kind only.
                            const Value r = sp[-2];
                            const ObjClass* cls = refHolderClass(r);
                            const uint64_t seen = caches[FAST_U16(2)].load(std::memory_order_relaxed);
                            if (cls != nullptr && static_cast<uint32_t>(seen >> 32) == cls->shape) {
                                const ValueField& field = cls->layout.load(std::memory_order_relaxed)->fields[(seen >> 16) & 0xFFFF];
                                const Value value = sp[-1];
                                uint8_t* at = (field.kind == ValueField::Float ? value.isFloat()
                                               : field.kind == ValueField::Int ? value.isInt()
                                                                               : value.isBool())
                                                  ? refQuickly(r, true)
                                                  : nullptr;
                                if (at != nullptr) {
                                    // The kind matched: written as it is, with no call.
                                    if (field.kind == ValueField::Bool) at[field.offset] = value.as.b ? 1 : 0;
                                    else std::memcpy(at + field.offset, &value.as, 8);
                                    sp[-2] = value;
                                    sp--;
                                    ip += 4;
                                    continue;
                                }
                            }
                        }
                        const bool own = static_cast<Op>(ip[-1]) == Op::SetOwn;
                        ObjString* name = asString(consts[READ_U16()]);
                        const int cache = READ_U16();
                        Value value = sp_[-1];
                        SAVE_IP();
                        CHECK_ACCESS(sp_[-2], 3, true, !own);
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
                                throw runtimeError(Thrown::IllegalStateException, "property '" + name->utf8() + "' of " + cls->name->utf8() +
                                                   " cannot be assigned: it has no setter");
                        }
                        setProperty(sp_[-2], name, value);
                        if (caches != nullptr && sp_[-2].isRef() && sp_[-2].refDepth == 0) {
                            const ObjClass* cls = refHolderClass(sp_[-2]);
                            const Value* found = cls != nullptr ? cls->declaredIndex.find(name) : nullptr;
                            const int64_t at = found != nullptr ? found->as.i : -1;
                            const ValueField::Kind kind = at >= 0 ? cls->layout.load(std::memory_order_relaxed)->fields[static_cast<size_t>(at)].kind : ValueField::Any;
                            if (at >= 0 && at <= 0xFFFF && (kind == ValueField::Int || kind == ValueField::Float || kind == ValueField::Bool))
                                caches[cache].store(static_cast<uint64_t>(cls->shape) << 32 | static_cast<uint64_t>(at) << 16, std::memory_order_relaxed);
                        } else if (caches != nullptr && sp_[-2].isObjType(ObjType::Instance)) {
                            // Remembered for next time, where the next time can be done quickly.
                            const ObjInstance* instance = asInstance(sp_[-2]);
                            const FieldDecl* decl = instance->declOf(name);
                            const int64_t at = decl != nullptr && decl->payload == FieldDecl::NoPayload ? decl->slot : -1;
                            if (decl != nullptr && decl->payload != FieldDecl::NoPayload && decl->payload != FieldDecl::NarrowPayload &&
                                !(decl->isVal && decl->overrides)) {
                                // A payload (M6.6 part 6): its offset, its bit and its kind.
                                caches[cache].store(static_cast<uint64_t>(instance->shape) << 32 |
                                                        static_cast<uint64_t>(sizeof(ObjInstance) + decl->payloadOffset) << 16 |
                                                        static_cast<uint64_t>(decl->payloadBit) << 8 | 0x80u | (decl->isVal ? 0x40u : 0u) |
                                                        (decl->tracked ? 0x20u : 0u) | decl->payloadValueKind(),
                                                    std::memory_order_relaxed);
                            } else if (at >= 0 && instance->slotsStart + static_cast<uint64_t>(at) * sizeof(Value) <= 0xFFFF) {
                                const FieldDecl& field = *decl;
                                const uint64_t from = instance->slotsStart + static_cast<uint64_t>(at) * sizeof(Value);
                                if (!(field.isVal && field.overrides) && (field.type & kTypeMask) != kTypeInstance)
                                    caches[cache].store(static_cast<uint64_t>(instance->shape) << 32 | from << 16 |
                                                            static_cast<uint64_t>(field.type) << 8 | (field.isVal ? 1u : 0u),
                                                        std::memory_order_relaxed);
                                else if (field.inlineOffset >= 0 && instance->inlined && !field.isVal)
                                    caches[cache].store(static_cast<uint64_t>(instance->shape) << 32 | from << 16 | 2u,
                                                        std::memory_order_relaxed);  // kept inside the object (the second bit)
                            }
                        }
                        sp_[-2] = value;
                        sp_--;
                        break;
                    }

                    case Op::GetIndexRef: {
                        if (sp_[-2].isObjType(ObjType::List) && sp_[-1].isInt()) {
                            ObjList* list = asList(sp_[-2]);
                            size_t i;
                            if (list->store() == ListStore::Items && resolveIndex(sp_[-1].as.i, list->size(), i) &&
                                !(list->pack->nullable && list->pack->nulls[i / 64] >> (i % 64) & 1)) {
                                guardRead(list);
                                sp_[-2] = Value::ref(list, static_cast<uint32_t>(i), 0, 0);  // the item, where it is
                                sp_--;
                                break;
                            }
                        }
                        goto getIndex;
                    }
                    case Op::OwnLocal: {
                        Value& slot = base[READ_U16()];
                        if (slot.isRef()) slot = ownRef(slot);
                        break;
                    }
                    case Op::GetIndex:
                    getIndex: {
                        Value index = sp_[-1], obj = sp_[-2];
                        if (classOf(obj) != nullptr) {
                            SAVE_IP();
                            invokeOperator(obj, names.opIndex, 1);
                            LOAD_FRAME();
                            break;
                        }
                        if (obj.isObjType(ObjType::List) && index.isInt()) {
                            // A list's item by its position, in range, read at once where nothing
                            // stands in the way: as getIndex reads it (G25).
                            const ObjList* list = asList(obj);
                            const uint64_t i = static_cast<uint64_t>(index.as.i);
                            if (i < list->size() && obj.as.obj != bandObject_ && (obj.as.obj->core == coreId_ || obj.as.obj->core == 0)) {
                                if (!list->packed()) {
                                    sp_[-2] = list->values()[i];
                                    sp_--;
                                    break;
                                }
                                if (list->pack->store != ListStore::Items) {
                                    sp_[-2] = packedPlain(*list->pack, i);
                                    sp_--;
                                    break;
                                }
                            }
                        }
                        if (obj.isObjType(ObjType::List) && asList(obj)->store() == ListStore::Items && index.isInt()) {
                            // An item of a list of values (M6.6 part 3): read with one of its fields
                            // at once, `pts[i].x`, the field is read where it is; otherwise a copy.
                            ObjList* list = asList(obj);
                            size_t i;
                            if (resolveIndex(index.as.i, list->size(), i) && !(list->pack->nullable && list->pack->nulls[i / 64] >> (i % 64) & 1)) {
                                guardRead(obj.as.obj);
                                if (obj.as.obj == bandObject_ && (i < bandFrom_ || i >= bandTo_)) [[unlikely]] throw outsideBand(i);
                                ValueField whole;
                                whole.kind = ValueField::Nested;
                                whole.cls = list->pack->itemClass;
                                Value read;
                                ip = readThrough(itemAt(list, i), &whole, ip, consts, read);
                                sp_[-2] = read;
                                sp_--;
                                break;
                            }
                        }
                        sp_[-2] = getIndex(obj, index);
                        sp_--;
                        break;
                    }
                    case Op::SetIndex: {
                        Value value = sp_[-1], index = sp_[-2], obj = sp_[-3];
                        if (obj.isObjType(ObjType::List) && index.isInt() && obj.as.obj->core == coreId_ && obj.as.obj->frozen == 0 &&
                            static_cast<uint64_t>(index.as.i) < asList(obj)->size()) {
                            // An int, a float or a bool put into a packed list of its kind (G25): written
                            // into the block, its null bit cleared; anything else as setIndex does it.
                            ObjList* list = asList(obj);
                            const size_t i = static_cast<size_t>(index.as.i);
                            PackedItems* pack = list->pack.get();
                            if (pack != nullptr && ((pack->store == ListStore::Ints && value.isInt()) || (pack->store == ListStore::Floats && value.isFloat()) ||
                                                    (pack->store == ListStore::Bools && value.isBool()))) {
                                uint8_t* at = pack->block.data() + i * pack->width;
                                if (pack->store == ListStore::Bools) *at = value.as.b ? 1 : 0;
                                else std::memcpy(at, &value.as.i, 8);
                                if (pack->nullable) pack->nulls[i / 64] &= ~(uint64_t{1} << (i % 64));
                                sp_[-3] = value;
                                sp_ -= 2;
                                break;
                            }
                        }
                        if (classOf(obj) != nullptr) {
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

                    case Op::SetSuper: {
                        ObjString* name = asString(consts[READ_U16()]);
                        const Value super = sp_[-2];
                        CHECK_ACCESS(sp_[-3], 1, false, true);
                        if (!super.isObjType(ObjType::Class))
                            throw runtimeError(Thrown::UnsupportedOperationException, "there is no '" + name->utf8() + "' for super to mean: this class has no superclass");
                        if (const Value* setter = asClass(super)->setters.find(name)) {
                            // The setter, run on this object with the value.
                            sp_[-2] = sp_[-1];
                            sp_--;
                            SAVE_IP();
                            callClosure(asClosure(*setter), 1);
                            LOAD_FRAME();
                            break;
                        }
                        // A field the superclass declares, which the object has: written as it is.
                        if (asClass(super)->declaredIndex.find(name) == nullptr)
                            throw runtimeError(Thrown::UnsupportedOperationException, "undefined property '" + name->utf8() + "' in superclass " + asClass(super)->name->utf8());
                        SAVE_IP();
                        const Value value = sp_[-1];
                        setProperty(sp_[-3], name, value);
                        sp_ -= 2;
                        sp_[-1] = value;
                        break;
                    }
                    case Op::GetSuper: {
                        ObjString* name = asString(consts[READ_U16()]);
                        Value super = *--sp_;
                        CHECK_ACCESS(sp_[-1], 1, false, true);
                        const Value* method = superMethod(super, sp_[-1], name);
                        if (method == nullptr && super.isNull())
                            throw runtimeError(Thrown::UnsupportedOperationException, "there is no '" + name->utf8() + "' for super to mean: this class has no superclass, " +
                                               std::string("and its interfaces come with none"));
                        if (method == nullptr && !super.isObjType(ObjType::Class)) throw runtimeError(Thrown::ClassCastException, "superclass must be a class");
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
                            Value field;
                            const bool found = sp_[-1].isObjType(ObjType::Instance) && asInstance(sp_[-1])->read(name, field);
                            if (found && asClass(super)->declaredIndex.find(name) != nullptr) {
                                sp_[-1] = fieldRead(field);
                                break;
                            }
                            throw runtimeError(Thrown::UnsupportedOperationException, "undefined method or property '" + name->utf8() + "' in superclass " +
                                               asClass(super)->name->utf8());
                        }
                        sp_[-1] = Value::object(alloc<ObjBoundMethod>(sp_[-1], asClosure(*method)));
                        break;
                    }

                    case Op::Add:
                        FAST_ARITHMETIC(sp[-2], sp[-1], +, addOverflows, sp--; continue)
                    plainAdd:
                        ARITHMETIC(Op::Add, addOverflows, x + y)
                    case Op::Sub:
                        FAST_ARITHMETIC(sp[-2], sp[-1], -, subOverflows, sp--; continue)
                    plainSub:
                        ARITHMETIC(Op::Sub, subOverflows, x - y)
                    case Op::Mul:
                        FAST_ARITHMETIC(sp[-2], sp[-1], *, mulOverflows, sp--; continue)
                    plainMul:
                        ARITHMETIC(Op::Mul, mulOverflows, x * y)
                    case Op::AddLocal:
                        FAST_ARITHMETIC(sp[-1], base[FAST_U16(0)], +, addOverflows, ip += 2; continue)
                        JOINED_ARITHMETIC(base[READ_U16()], addOverflows, x + y, plainAdd)
                    case Op::SubLocal:
                        FAST_ARITHMETIC(sp[-1], base[FAST_U16(0)], -, subOverflows, ip += 2; continue)
                        JOINED_ARITHMETIC(base[READ_U16()], subOverflows, x - y, plainSub)
                    case Op::MulLocal:
                        FAST_ARITHMETIC(sp[-1], base[FAST_U16(0)], *, mulOverflows, ip += 2; continue)
                        JOINED_ARITHMETIC(base[READ_U16()], mulOverflows, x * y, plainMul)
                    case Op::DivLocal:
                        FAST_FLOAT(sp[-1], base[FAST_U16(0)], /, ip += 2; continue)
                        JOINED_FLOAT(base[READ_U16()], x / y, plainDiv)
                    case Op::ModLocal: JOINED_FLOAT(base[READ_U16()], std::fmod(x, y), plainMod)
                    case Op::LtLocal:
                        FAST_COMPARISON(sp[-1], base[FAST_U16(0)], <, ip += 2; continue)
                        JOINED_COMPARISON(base[READ_U16()], <, plainLt)
                    case Op::LeLocal:
                        FAST_COMPARISON(sp[-1], base[FAST_U16(0)], <=, ip += 2; continue)
                        JOINED_COMPARISON(base[READ_U16()], <=, plainLe)
                    case Op::GtLocal:
                        FAST_COMPARISON(sp[-1], base[FAST_U16(0)], >, ip += 2; continue)
                        JOINED_COMPARISON(base[READ_U16()], >, plainGt)
                    case Op::GeLocal:
                        FAST_COMPARISON(sp[-1], base[FAST_U16(0)], >=, ip += 2; continue)
                        JOINED_COMPARISON(base[READ_U16()], >=, plainGe)
                    case Op::EqLocal:
                        FAST_COMPARISON(sp[-1], base[FAST_U16(0)], ==, ip += 2; continue)
                        JOINED_COMPARISON(base[READ_U16()], ==, plainEq)
                    case Op::AddConst:
                        FAST_ARITHMETIC(sp[-1], consts[FAST_U16(0)], +, addOverflows, ip += 2; continue)
                        JOINED_ARITHMETIC(consts[READ_U16()], addOverflows, x + y, plainAdd)
                    case Op::SubConst:
                        FAST_ARITHMETIC(sp[-1], consts[FAST_U16(0)], -, subOverflows, ip += 2; continue)
                        JOINED_ARITHMETIC(consts[READ_U16()], subOverflows, x - y, plainSub)
                    case Op::MulConst:
                        FAST_ARITHMETIC(sp[-1], consts[FAST_U16(0)], *, mulOverflows, ip += 2; continue)
                        JOINED_ARITHMETIC(consts[READ_U16()], mulOverflows, x * y, plainMul)
                    case Op::DivConst:
                        FAST_FLOAT(sp[-1], consts[FAST_U16(0)], /, ip += 2; continue)
                        JOINED_FLOAT(consts[READ_U16()], x / y, plainDiv)
                    case Op::ModConst: JOINED_FLOAT(consts[READ_U16()], std::fmod(x, y), plainMod)
                    case Op::LtConst:
                        FAST_COMPARISON(sp[-1], consts[FAST_U16(0)], <, ip += 2; continue)
                        JOINED_COMPARISON(consts[READ_U16()], <, plainLt)
                    case Op::LeConst:
                        FAST_COMPARISON(sp[-1], consts[FAST_U16(0)], <=, ip += 2; continue)
                        JOINED_COMPARISON(consts[READ_U16()], <=, plainLe)
                    case Op::GtConst:
                        FAST_COMPARISON(sp[-1], consts[FAST_U16(0)], >, ip += 2; continue)
                        JOINED_COMPARISON(consts[READ_U16()], >, plainGt)
                    case Op::GeConst:
                        FAST_COMPARISON(sp[-1], consts[FAST_U16(0)], >=, ip += 2; continue)
                        JOINED_COMPARISON(consts[READ_U16()], >=, plainGe)
                    case Op::EqConst:
                        FAST_COMPARISON(sp[-1], consts[FAST_U16(0)], ==, ip += 2; continue)
                        JOINED_COMPARISON(consts[READ_U16()], ==, plainEq)
                    case Op::StoreLocal:
                        base[READ_U16()] = *--sp;
                        continue;
                    case Op::CopyLocal:
                        base[FAST_U16(2)] = base[FAST_U16(0)];
                        ip += 4;
                        continue;
                    case Op::AddLocals:
                        sp[0] = base[FAST_U16(0)];
                        FAST_ARITHMETIC(sp[0], base[FAST_U16(2)], +, addOverflows, sp++; ip += 4; continue)
                        *sp_++ = base[READ_U16()];
                        *sp_++ = base[READ_U16()];
                        goto plainAdd;
                    case Op::SubLocals:
                        sp[0] = base[FAST_U16(0)];
                        FAST_ARITHMETIC(sp[0], base[FAST_U16(2)], -, subOverflows, sp++; ip += 4; continue)
                        *sp_++ = base[READ_U16()];
                        *sp_++ = base[READ_U16()];
                        goto plainSub;
                    case Op::MulLocals:
                        sp[0] = base[FAST_U16(0)];
                        FAST_ARITHMETIC(sp[0], base[FAST_U16(2)], *, mulOverflows, sp++; ip += 4; continue)
                        *sp_++ = base[READ_U16()];
                        *sp_++ = base[READ_U16()];
                        goto plainMul;
                    case Op::DivLocals:
                        sp[0] = base[FAST_U16(0)];
                        FAST_FLOAT(sp[0], base[FAST_U16(2)], /, sp++; ip += 4; continue)
                        *sp_++ = base[READ_U16()];
                        *sp_++ = base[READ_U16()];
                        goto plainDiv;
                    case Op::LtLocals:
                        sp[0] = base[FAST_U16(0)];
                        FAST_COMPARISON(sp[0], base[FAST_U16(2)], <, sp++; ip += 4; continue)
                        *sp_++ = base[READ_U16()];
                        *sp_++ = base[READ_U16()];
                        goto plainLt;
                    case Op::LeLocals:
                        sp[0] = base[FAST_U16(0)];
                        FAST_COMPARISON(sp[0], base[FAST_U16(2)], <=, sp++; ip += 4; continue)
                        *sp_++ = base[READ_U16()];
                        *sp_++ = base[READ_U16()];
                        goto plainLe;
                    case Op::GtLocals:
                        sp[0] = base[FAST_U16(0)];
                        FAST_COMPARISON(sp[0], base[FAST_U16(2)], >, sp++; ip += 4; continue)
                        *sp_++ = base[READ_U16()];
                        *sp_++ = base[READ_U16()];
                        goto plainGt;
                    case Op::GeLocals:
                        sp[0] = base[FAST_U16(0)];
                        FAST_COMPARISON(sp[0], base[FAST_U16(2)], >=, sp++; ip += 4; continue)
                        *sp_++ = base[READ_U16()];
                        *sp_++ = base[READ_U16()];
                        goto plainGe;
                    case Op::EqLocals:
                        sp[0] = base[FAST_U16(0)];
                        FAST_COMPARISON(sp[0], base[FAST_U16(2)], ==, sp++; ip += 4; continue)
                        *sp_++ = base[READ_U16()];
                        *sp_++ = base[READ_U16()];
                        goto plainEq;

                    case Op::CheckLocal: {
                        if ((matchesType(base[FAST_U16(0)], ip[4], nullptr) || widenedByte(base[FAST_U16(0)], ip[4]))) {
                            ip += 5;
                            continue;
                        }
                        const int slot = READ_U16();
                        ObjString* parameter = asString(consts[READ_U16()]);
                        const uint8_t type = READ_U8();
                        if (!matchesType(base[slot], type, nullptr)) {
                            checkRange(type, base[slot], "argument '" + parameter->utf8() + "' of " + frame->closure->proto->name + "()");
                            throw runtimeError(Thrown::IllegalArgumentException, "argument '" + parameter->utf8() + "' of " + frame->closure->proto->name +
                                               "() must be " + declaredTypeText(type, nullptr) + ", got " + typeName(base[slot]));
                        }
                        break;
                    }
                    case Op::Div:
                    plainDiv: {
                        Value b = sp_[-1], a = sp_[-2];
                        if (a.isInt() && b.isInt()) {
                            if (b.as.i == 0) throw runtimeError(Thrown::ArithmeticException, "division by zero");
                            if (b.as.i == -1 && a.as.i == INT64_MIN) throw overflowError(Op::Div, a.as.i, b.as.i);  // (D27)
                            int64_t result = b.as.i == -1 ? -a.as.i : a.as.i / b.as.i;
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
                    case Op::Mod:
                    plainMod: {
                        Value b = sp_[-1], a = sp_[-2];
                        if (a.isInt() && b.isInt()) {
                            if (b.as.i == 0) throw runtimeError(Thrown::ArithmeticException, "division by zero");
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

                    case Op::AddWrap:
                    case Op::SubWrap:
                    case Op::MulWrap: {
                        // `&+`, `&-` and `&*`: an int sum, difference or product that wraps around (D27).
                        const Op op = static_cast<Op>(ip[-1]);
                        const Value b = sp_[-1], a = sp_[-2];
                        if (!a.isInt() || !b.isInt())
                            throw runtimeError(Thrown::ArithmeticException, std::string(op == Op::AddWrap ? "&+" : op == Op::SubWrap ? "&-" : "&*") +
                                               " works on ints, not on " + typeName(a) + " and " + typeName(b));
                        const uint64_t x = static_cast<uint64_t>(a.as.i), y = static_cast<uint64_t>(b.as.i);
                        sp_[-2] = Value::integer(static_cast<int64_t>(op == Op::AddWrap ? x + y : op == Op::SubWrap ? x - y : x * y));
                        sp_--;
                        break;
                    }
                    case Op::Shl:
                    case Op::Shr:
                    case Op::Ushr:
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
                        if (a.isInt() || a.isByte()) {
                            sp_[-1] = Value::integer(~a.as.i);
                        } else if (classOf(a) != nullptr) {
                            SAVE_IP();
                            invokeOperator(a, names.opBitNot, 0);
                            LOAD_FRAME();
                        } else {
                            throw runtimeError(Thrown::UnsupportedOperationException, "cannot apply '~' to a value of type " + typeName(a));
                        }
                        break;
                    }

                    case Op::Neg: {
                        Value a = sp_[-1];
                        if (a.isInt()) {
                            if (a.as.i == INT64_MIN) throw overflowError(Op::Sub, 0, a.as.i);  // (D27)
                            sp_[-1] = Value::integer(-a.as.i);
                        } else if (a.isFloat()) {
                            sp_[-1] = Value::number(-a.as.d);
                        } else if (a.isByte()) {
                            sp_[-1] = Value::integer(-a.as.i);  // a byte negated is an int (D19)
                        } else if (classOf(a) != nullptr) {
                            SAVE_IP();
                            invokeOperator(a, names.opNeg, 0);
                            LOAD_FRAME();
                        } else {
                            throw runtimeError(Thrown::UnsupportedOperationException, "cannot negate a value of type " + typeName(a));
                        }
                        break;
                    }
                    case Op::Not:
                        sp[-1].setBool(!sp[-1].truthy());
                        continue;

                    case Op::Eq:
                    plainEq: {
                        Value b = sp_[-1], a = sp_[-2];
                        if (const ObjClass* k = classOf(a); k != nullptr && k->methods.find(names.opEq) != nullptr) {
                            SAVE_IP();
                            invokeOperator(a, names.opEq, 1);
                            LOAD_FRAME();
                            break;
                        }
                        if ((a.isRef() || b.isRef() || a.isSmall() || b.isSmall()) ||
                            (a.isObj() && b.isObj() && a.as.obj != b.as.obj && a.as.obj->type != ObjType::String)) {
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
                        if (sp_[-1].isRef() || sp_[-2].isRef() || sp_[-1].isSmall() || sp_[-2].isSmall()) {
                            SAVE_IP();
                            sp_[-1] = boxed(sp_[-1]);
                            sp_[-2] = boxed(sp_[-2]);
                        }
                        Value b = sp_[-1], a = sp_[-2];
                        const bool objects = a.isObj() && b.isObj() && !a.isString();
                        // Nor has a value (M6.6 part 2): two of the same fields are the same.
                        if (objects && a.isObjType(ObjType::Value) && b.isObjType(ObjType::Value)) {
                            SAVE_IP();
                            sp_[-2] = Value::boolean(a.as.obj == b.as.obj || valuesSame(asValue(a), asValue(b), 0));
                        } else {
                            sp_[-2] = Value::boolean(objects ? a.as.obj == b.as.obj : valuesEqual(a, b));
                        }
                        sp_--;
                        break;
                    }
                    case Op::Lt:
                        FAST_COMPARISON(sp[-2], sp[-1], <, sp--; continue)
                    plainLt:
                        COMPARISON(Op::Lt, <)
                    case Op::Le:
                        FAST_COMPARISON(sp[-2], sp[-1], <=, sp--; continue)
                    plainLe:
                        COMPARISON(Op::Le, <=)
                    case Op::Gt:
                        FAST_COMPARISON(sp[-2], sp[-1], >, sp--; continue)
                    plainGt:
                        COMPARISON(Op::Gt, >)
                    case Op::Ge:
                        FAST_COMPARISON(sp[-2], sp[-1], >=, sp--; continue)
                    plainGe:
                        COMPARISON(Op::Ge, >=)

                    case Op::Is: {
                        Value type = *--sp_;
                        if (!type.isObjType(ObjType::Class)) throw runtimeError(Thrown::ClassCastException, "right operand of 'is' must be a class");
                        Value value = sp_[-1];
                        sp_[-1] = Value::boolean(classOf(value) != nullptr && classOf(value)->isSubclassOf(asClass(type)));
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
                        if (!(*--sp).truthy()) ip = code + target;
                        continue;
                    }
                    case Op::And: {
                        int target = READ_I32();
                        if (!sp[-1].truthy()) ip = code + target;
                        else sp--;
                        continue;
                    }
                    case Op::Or: {
                        int target = READ_I32();
                        if (sp[-1].truthy()) ip = code + target;
                        else sp--;
                        continue;
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
                        if (sp_[-1].isNull()) throw runtimeError(Thrown::NullPointerException, "'!!' was used on a value that is null");
                        break;
                    case Op::Swap:
                        std::swap(sp[-1], sp[-2]);
                        continue;
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
                            throw runtimeError(Thrown::ClassCastException, "cannot cast a value of type " + typeName(sp_[-1]) + " to " +
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
                        if (sp[-ip[0] - 1].isObjType(ObjType::Closure)) QUICK_CALL(asClosure(sp[-ip[0] - 1]), ip[0], 1)
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
                            throw runtimeError(Thrown::IllegalArgumentException, "'*' in a call needs a list to spread, not a value of type " + typeName(added));
                        ObjList* list = asList(sp_[-1]);
                        guardRead(added);
                        const std::vector<Value> items = listCopy(asList(added));
                        listInsert(list, list->size(), items.data(), items.size());
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
                        // A method of the object's class, open to this code.
                        if (sp[-ip[2] - 1].isObjType(ObjType::Instance)) {
                            const ObjInstance* instance = asInstance(sp[-ip[2] - 1]);
                            const ObjString* name = asString(consts[FAST_U16(0)]);
                            if (((name->restricted & 1) == 0 || static_cast<Op>(ip[-1]) == Op::InvokeOwn) &&
                                (instance->core == 0 || instance->core == coreId_)) {
                                if (const Value* method = instance->cls->methods.find(name)) QUICK_CALL(asClosure(*method), ip[2], 3)
                            }
                        } else if (sp[-ip[2] - 1].isSmall()) {
                            // A method of a small value's type, run on the value itself (M6.6 part 5).
                            const ObjClass* cls = smallClassOf(sp[-ip[2] - 1]);
                            const ObjString* name = asString(consts[FAST_U16(0)]);
                            if ((name->restricted & 1) == 0 || static_cast<Op>(ip[-1]) == Op::InvokeOwn)
                                if (const Value* method = cls->methods.find(name)) QUICK_CALL(asClosure(*method), ip[2], 3)
                        } else if (sp[-ip[2] - 1].isObjType(ObjType::Value)) {
                            // A method of a value's type (M6.6 part 2): a value never changes, so
                            // any core may run one on it.
                            const ObjValue* box = asValue(sp[-ip[2] - 1]);
                            const ObjString* name = asString(consts[FAST_U16(0)]);
                            if ((name->restricted & 1) == 0 || static_cast<Op>(ip[-1]) == Op::InvokeOwn)
                                if (const Value* method = box->cls->methods.find(name)) QUICK_CALL(asClosure(*method), ip[2], 3)
                        }
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
                            throw runtimeError(Thrown::UnsupportedOperationException, "there is no '" + name->utf8() + "' for super to mean: this class has no superclass, " +
                                               std::string("and its interfaces come with none"));
                        if (method == nullptr && !super.isObjType(ObjType::Class)) throw runtimeError(Thrown::ClassCastException, "superclass must be a class");
                        if (method == nullptr)
                            throw runtimeError(Thrown::UnsupportedOperationException, "undefined method '" + name->utf8() + "' in superclass " +
                                               asClass(super)->name->utf8());
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
                            if (argc != 0) throw runtimeError(Thrown::UnsupportedOperationException, "there is no superclass to pass arguments to: only a class takes them");
                            break;
                        }
                        if (!super.isObjType(ObjType::Class)) throw runtimeError(Thrown::ClassCastException, "superclass must be a class");
                        ObjClosure* init = asClass(super)->init;
                        if (init == nullptr) {
                            if (argc != 0)
                                throw runtimeError(Thrown::IllegalArgumentException, asClass(super)->name->utf8() + "() takes no arguments but got " +
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
                        if ((proto->typeArgs & kTypeArgsKept) != 0) closure->typeArgs = frameTypeArgs(frame - frames_);
                        account(proto->upvalues.size() * sizeof(ObjUpvalue*));
                        for (size_t i = 0; i < proto->upvalues.size(); i++) {
                            const UpvalueDesc& uv = proto->upvalues[i];
                            closure->upvalues[i] = uv.isLocal       ? captureUpvalue(base + uv.index)
                                                   : uv.fromCapture ? capturedOf(base[0], uv.index)
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
                            if (!sp_[-i].isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid set of forms");
                            set->forms.push_back(asClosure(sp_[-i]));
                        }
                        sp_ -= count;
                        *sp_++ = Value::object(set);
                        break;
                    }
                    case Op::MethodForms: {
                        const FunctionProto* chooser = frame->closure->proto->protos[static_cast<size_t>(READ_U16())];
                        const int count = READ_U8();
                        if (!sp_[-count - 1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
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
                                throw runtimeError(Thrown::UnsupportedOperationException, "class " + cls->name->utf8() + " has no form " + name->utf8());
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
                        if (!sp_[-count - 1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        ObjClass* cls = asClass(sp_[-count - 1]);
                        ObjForms* set = alloc<ObjForms>(chooser);
                        cls->statics.set(intern(chooser->name), Value::object(set));
                        for (int i = count; i > 0; i--) {
                            const Value* method = cls->statics.find(asString(sp_[-i]));
                            if (method == nullptr || !method->isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                            set->forms.push_back(asClosure(*method));
                        }
                        account(set->forms.size() * sizeof(ObjClosure*));
                        sp_ -= count;
                        break;
                    }
                    case Op::GetIndexOr: {
                        const Value start = sp_[-1], key = sp_[-2], map = sp_[-3];
                        if (!map.isObjType(ObjType::Map))
                            throw runtimeError(Thrown::IllegalArgumentException, "only a map's index takes a default, not that of a value of type " + typeName(map));
                        guardRead(map);
                        const Value* found = asMap(map)->map.find(own(key));
                        sp_ -= 2;
                        sp_[-1] = found != nullptr ? *found : start;
                        break;
                    }
                    case Op::CloseUpvalue:
                        closeUpvalues(sp_ - 1);
                        sp_--;
                        break;
                    case Op::CallOn: {
                        const int argc = READ_U8();
                        Value* const at = sp_ - argc - 2;
                        const Value callee = at[0];
                        SAVE_IP();
                        if (!callee.isObjType(ObjType::Closure)) {
                            // Only a function written in Astra runs on an object: any other is given the arguments alone.
                            for (int i = 0; i < argc; i++) at[1 + i] = at[2 + i];
                            sp_--;
                            if (callValue(callee, argc)) LOAD_FRAME();
                            break;
                        }
                        for (int i = 0; i <= argc; i++) at[i] = at[1 + i];
                        sp_--;
                        callClosure(asClosure(callee), argc);
                        LOAD_FRAME();
                        break;
                    }
                    case Op::ImplicitFirst: {
                        const int slot = READ_U16();
                        const int flags = READ_U8();
                        if (implicitFirstCount_ < 8) implicitFirst_[implicitFirstCount_++] = base[slot];
                        if ((flags & 1) != 0) implicitOnly_ = true;
                        break;
                    }
                    case Op::TailLoop: {
                        // The function calls itself as its last step (D29): what a closure kept of
                        // this round keeps its values, the arguments become the parameters, and it
                        // starts again in this frame.
                        const int count = READ_U8();
                        Value* const given = sp_ - count;
                        closeUpvalues(base + 1);
                        for (int i = 0; i < count; i++) base[1 + i] = given[i];
                        sp_ = base + 1 + count;
                        frame->repeats++;
                        ip = code;
                        GC_SAFE_POINT();
                        break;
                    }

                    case Op::ReturnInit:
                        // The object is made once its own class's initializer has returned.
                        if (base[0].isObjType(ObjType::Instance) && asInstance(base[0])->cls->init == frame->closure)
                            asInstance(base[0])->constructing = false;
                        else if (base[0].isObjType(ObjType::Value) && asValue(base[0])->cls->init == frame->closure) {
                            ObjValue* box = asValue(base[0]);
                            box->constructing = false;  // and from now on never changes
                            if (box->cls->smallId != 0) {
                                // Small: given back in the value itself (M6.6 part 5).
                                const Value made = smallFrom(box->cls, box->data(), box->cls->layout.load(std::memory_order_relaxed)->size);
                                if (sp[-1].kind == ValueKind::Obj && sp[-1].as.obj == box) sp[-1] = made;
                                base[0] = made;
                            }
                        }
                        // Slot 0 is the new instance: its required fields must be set by now.
                        if (base[0].isObjType(ObjType::Instance) && asInstance(base[0])->cls->anyRequired) {
                            SAVE_IP();
                            checkRequiredFields(asInstance(base[0]), frame->closure);
                        }
                        [[fallthrough]];
                    case Op::Return: {
                        // Back to the caller, where there is nothing else to see to: no variable
                        // of the frame captured, no catch block of its own left, and its result
                        // wanted (M6.5 part 4).
                        if ((openUpvalues_ == nullptr || openUpvalues_->location < base) && !frame->dropResult &&
                            frameCount_ - 1 != baseFrame && (handlers_.empty() || handlers_.back().frameIndex < frameCount_ - 1) &&
                            (sp[-1].kind != ValueKind::Ref || keptAsItIs(frame))) {
                            Value* const top = base;
                            *top = sp[-1];
                            sp = top + 1;
                            frameCount_--;
                            LOAD_FRAME();
                            continue;
                        }
                        Value result = sp_[-1];
                        if (result.isRef()) [[unlikely]] result = returnedRef(result, frameCount_ - 1);
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
                        if (!sub.isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        ObjClass* subclass = asClass(sub);

                        // Of the supertypes, at most one is a class: the superclass.
                        ObjClass* superclass = nullptr;
                        for (int i = 0; i < count; i++) {
                            if (!types[i].isObjType(ObjType::Class))
                                throw runtimeError(Thrown::ClassCastException, "superclass must be a class, got " + typeName(types[i]));
                            ObjClass* type = asClass(types[i]);
                            if (type->isInterface) continue;
                            if (type->isEnum) throw runtimeError(Thrown::IllegalStateException, "cannot inherit from enum " + type->name->utf8());
                            if (type->isValue)
                                throw runtimeError(Thrown::IllegalStateException, "'" + subclass->name->utf8() + "' cannot extend value '" + type->name->utf8() +
                                                   "': a value type has no subclasses");
                            if (subclass->isValue)
                                throw runtimeError(Thrown::IllegalStateException, "value '" + subclass->name->utf8() + "' cannot extend class '" + type->name->utf8() +
                                                   "': a value type may only implement interfaces");
                            if (subclass->isInterface)
                                throw runtimeError(Thrown::IllegalStateException, "interface " + subclass->name->utf8() + " cannot extend class " + type->name->utf8());
                            if (superclass != nullptr)
                                throw runtimeError(Thrown::IllegalStateException, "class " + subclass->name->utf8() + " can have only one superclass, but " +
                                                   superclass->name->utf8() + " and " + type->name->utf8() + " are both classes");
                            superclass = type;
                        }
                        if (superclass != nullptr) {
                            subclass->super = superclass;
                            subclass->methods.addAll(superclass->methods);
                            subclass->init = superclass->init;
                            subclass->declared = superclass->declared;  // laid out again for the subclass's objects
                            subclass->declaredIndex.addAll(superclass->declaredIndex);
                            subclass->shape = newShape();
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
                            throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        asClass(cls)->staticInit = asClosure(init);
                        sp_--;
                        break;
                    }
                    case Op::EnsureClassInit:
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        SAVE_IP();
                        ensureClassInit(asClass(sp_[-1]));
                        break;
                    case Op::MissingArg: {
                        ObjString* parameter = asString(consts[READ_U16()]);
                        throw runtimeError(Thrown::IllegalArgumentException, frame->closure->proto->name + "() is missing argument '" + parameter->utf8() + "'");
                    }
                    case Op::DataField: {
                        ObjString* name = asString(consts[READ_U16()]);
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        asClass(sp_[-1])->isData = true;
                        if (name->length > 0) asClass(sp_[-1])->components.push_back(name);  // "": a data class of no values
                        break;
                    }
                    case Op::Extend: {
                        // extend Person : Weighed (D33): the class fits the interface, and the
                        // ones above it, and has what they bring that it has not.
                        if (!sp_[-1].isObjType(ObjType::Class) || !asClass(sp_[-1])->isInterface)
                            throw runtimeError(Thrown::IllegalArgumentException, "an extend fits a class to an interface");
                        if (!sp_[-2].isObjType(ObjType::Class)) throw runtimeError(Thrown::IllegalArgumentException, "an extend fits a class to an interface");
                        ObjClass* iface = asClass(sp_[-1]);
                        ObjClass* cls = asClass(sp_[-2]);
                        auto fit = [&](ObjClass* to) {
                            if (std::find(cls->extendedTo.begin(), cls->extendedTo.end(), to) == cls->extendedTo.end())
                                cls->extendedTo.push_back(to);
                        };
                        fit(iface);
                        for (ObjClass* above : iface->interfaces) fit(above);
                        for (uint32_t i = 0; i < iface->methods.capacity(); i++) {
                            const Table::Entry& e = iface->methods.entries()[i];
                            if (e.key != nullptr && cls->methods.find(e.key) == nullptr) cls->extendMethods.set(e.key, e.value);
                        }
                        for (uint32_t i = 0; i < iface->getters.capacity(); i++) {
                            const Table::Entry& e = iface->getters.entries()[i];
                            if (e.key != nullptr && cls->getters.find(e.key) == nullptr) cls->extendGetters.set(e.key, e.value);
                        }
                        sp_--;
                        break;
                    }
                    case Op::Bridge: {
                        ObjString* name = asString(consts[READ_U16()]);
                        if (!sp_[-2].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid extend");
                        asClass(sp_[-2])->extendGetters.set(name, sp_[-1]);
                        sp_--;
                        break;
                    }
                    case Op::Lazy: {
                        if (!sp_[-1].isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid lazy value");
                        ObjLazy* lazy = alloc<ObjLazy>();
                        lazy->init = asClosure(sp_[-1]);
                        sp_[-1] = Value::lazy(lazy);
                        break;
                    }
                    case Op::UnstableKey:
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        asClass(sp_[-1])->unstableKey = true;
                        break;
                    case Op::Component: {
                        const size_t index = READ_U8();
                        SAVE_IP();
                        sp_[-1] = componentOf(sp_[-1], index);
                        break;
                    }
                    case Op::IterNextPair: {
                        const int slot = READ_U16();
                        const int target = READ_I32();
                        const Value iterator = base[slot];
                        if (!iterator.isObjType(ObjType::Iterator)) throw runtimeError(Thrown::InternalError, "invalid loop state");
                        ObjIterator* it = asIterator(iterator);
                        SAVE_IP();
                        if (it->source.isObjType(ObjType::Map)) {
                            // A map's key and value, as they are: no entry made (D73).
                            const std::vector<ValueMap::Entry>& entries = asMap(it->source)->map.entries();
                            while (it->index < entries.size() && !entries[it->index].live) it->index++;
                            if (it->index >= entries.size()) {
                                ip = code + target;
                                break;
                            }
                            const ValueMap::Entry& entry = entries[it->index++];
                            sp_[0] = entry.key;
                            sp_[1] = entry.value;
                            sp_ += 2;
                            break;
                        }
                        Value item;
                        if (!iteratorNext(it, item)) {
                            ip = code + target;
                            break;
                        }
                        // Its parts read with the item where the collector sees it.
                        sp_[0] = item;
                        sp_[1] = item;
                        sp_ += 2;
                        sp_[-2] = componentOf(item, 0);
                        sp_[-1] = componentOf(item, 1);
                        break;
                    }
                    case Op::MarkInterface:
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        asClass(sp_[-1])->isInterface = true;
                        break;
                    case Op::Extension: {
                        ObjString* name = asString(consts[READ_U16()]);
                        const int type = READ_U8();
                        Value function = *--sp_;
                        if (!function.isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid extension function");
                        if (coreId_ != 0) throw notOwned("an extension function is added for the whole program");
                        const int id = type & kTypeMask;
                        if (id == kTypeInstance) {
                            Value target = *--sp_;
                            if (!target.isObjType(ObjType::Class))
                                throw runtimeError(Thrown::IllegalArgumentException, "an extension function is added to a class or a built-in type, not to a value of type " +
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
                            // Under its own name as well, which a call the checker could tell names.
                            if (const std::string& signature = asClosure(function)->proto->signature; !signature.empty())
                                nullExtensions_.set(intern(name->utf8() + signature), function);
                        }
                        hasExtensions_ = true;
                        break;
                    }
                    case Op::MarkAbstract:
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        asClass(sp_[-1])->isAbstract = true;
                        break;
                    case Op::MarkValue: {
                        const bool fromHeader = READ_U8() != 0;
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        asClass(sp_[-1])->isValue = true;
                        asClass(sp_[-1])->madeFromHeader = fromHeader;
                        break;
                    }
                    case Op::Numeric: {
                        const uint8_t which = READ_U8();
                        const Value a = sp_[-1];
                        if (which == static_cast<uint8_t>(NumericOp::ToInt) && a.isFloat() && a.as.d > -9.2e18 && a.as.d < 9.2e18) {
                            sp_[-1] = Value::integer(static_cast<int64_t>(a.as.d));  // the whole part, as toInt() gives
                            break;
                        }
                        SAVE_IP();
                        const int arity = numericArity(which);
                        const Value result = numeric(which, sp_ - arity);
                        sp_ -= arity - 1;
                        sp_[-1] = result;
                        break;
                    }
                    case Op::WithRefResult:
                        pendingDestination_ = Value::undefined();  // taken by a function that gives a value
                        break;
                    case Op::FieldDestination: {
                        ObjString* name = asString(consts[READ_U16()]);
                        const Value obj = sp_[-1];
                        Value place = Value::null();
                        if (obj.isObjType(ObjType::Instance) && asInstance(obj)->inlined) {
                            ObjInstance* instance = asInstance(obj);
                            const int64_t at = instance->fieldIndex(name);
                            if (at >= 0 && instance->fields()[at].isRef() && instance->fields()[at].as.obj == instance) place = instance->fields()[at];
                        }
                        sp_[-1] = place;
                        break;
                    }
                    case Op::PlaceLocal:
                    case Op::PlaceUpvalue:
                    case Op::PlaceGlobal:
                    case Op::PlaceField:
                    case Op::PlaceIndex:
                    case Op::InsertNew:
                        ip = placeInstruction(static_cast<Op>(ip[-1]), ip, frame);
                        break;
                    case Op::AddNew: {
                        const int argc = READ_U8();
                        const int target = READ_I32();
                        if (addNew(argc)) ip = code + target;
                        break;
                    }
                    case Op::WithDestinationAt:
                        pendingDestination_ = sp_[-READ_U8() - 1];
                        break;
                    case Op::WithFrameDestination:
                        pendingDestination_ = (frame->closure->proto->typeArgs & kTakesDestination) != 0 ? frameDestination(frame - frames_)
                                                                                                       : Value::null();
                        break;

                    case Op::CheckOverride: {
                        ObjString* name = asString(consts[READ_U16()]);
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        const int flags = READ_U8();
                        const bool marked = (flags & 1) != 0, property = (flags & 2) != 0;
                        const ObjClass* cls = asClass(sp_[-1]);

                        // What the class gets from above under this name: a method for a method,
                        // a field or property for a field or property; or something an interface
                        // or an abstract class asks for, which either kind supplies.
                        auto has = [&](const ObjClass* from) {
                            if (!property && from->methods.find(name) == nullptr && name->utf8().find('(') != std::string::npos) {
                                // A form of a name with several (D56): the one above may have been
                                // compiled where the name had one form, under the name alone.
                                const Value* plain = from->methods.find(intern(name->utf8().substr(0, name->utf8().find('('))));
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
                                throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' in class " + cls->name->utf8() +
                                                   " is marked override, but there is nothing of that name for it to replace");
                        } else if (askedBy != nullptr) {
                            throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' in class " + cls->name->utf8() + " is what " +
                                               (askedBy->isInterface ? "interface " : "abstract class ") + askedBy->name->utf8() +
                                               " asks for; write override in front of it");
                        } else if (inherited) {
                            throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' in class " + cls->name->utf8() +
                                               " takes the place of the one it inherits; write override in front of it");
                        }
                        break;
                    }
                    case Op::Require: {
                        ObjString* name = asString(consts[READ_U16()]);
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        asClass(sp_[-1])->required.emplace_back(name, asClass(sp_[-1]));
                        break;
                    }
                    case Op::ClassNumber: {
                        const int number = READ_U16();
                        const int library = READ_U16();
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
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
                        if (!sp_[-1].isObjType(ObjType::Class) || kind > kAccessAsAbove) throw runtimeError(Thrown::InternalError, "invalid class declaration");
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
                                throw runtimeError(Thrown::IllegalStateException, "'" + name->utf8() + "' in class " + cls->name->utf8() + " cannot be " +
                                                   (kind == kAccessPrivate ? "private" : kind == kAccessProtected ? "protected" : "internal") +
                                                   ": it is open in " + (open->isInterface ? "interface " : "class ") + open->name->utf8());
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
                            throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        declareAccess(asClass(sp_[-1]), asClass(sp_[-1])->staticAccessMarks, name, id, kind, (flags & kAccessOfSetter) != 0);
                        break;
                    }
                    case Op::FinishClass: {
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        ObjClass* cls = asClass(sp_[-1]);
                        // Whether the class's own code may use every member its objects have:
                        // then nothing has to be looked up for it (see CHECK_ACCESS).
                        classFinished();
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
                            throw runtimeError(Thrown::IllegalStateException, "class " + cls->name->utf8() + " must have '" + name->utf8() + "', which " +
                                               (interface->isInterface ? "interface " : "abstract class ") + interface->name->utf8() +
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
                            throw runtimeError(Thrown::InternalError, "invalid class declaration");
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
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid enum declaration");
                        declareEntryFields(asClass(sp_[-1]));
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
                        if (!target.isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid enum declaration");
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
                            throw runtimeError(Thrown::InternalError, "invalid enum declaration");
                        nameEntry(asInstance(entry), name, ordinal);
                        listAdd(asList(*list), entry);
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
                            throw runtimeError(Thrown::InternalError, "invalid class declaration");
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
                            if (!sp_[-2].isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                            field.typeThunk = asClosure(sp_[-2]);
                        }
                        if (!target.isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
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
                            if (!thunk.isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                            field.typeThunk = asClosure(thunk);
                        }
                        if (!sp_[-1].isObjType(ObjType::Class)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
                        ObjClass* cls = asClass(sp_[-1]);
                        if (const Value* inherited = cls->declaredIndex.find(name)) {
                            if ((flags & kFieldOverride) == 0)
                                throw runtimeError(Thrown::IllegalStateException, "field '" + name->utf8() + "' of class " + cls->name->utf8() +
                                                   " is already declared by a superclass; write override to declare it again");
                            field.overrides = true;
                            {
                                // Kept where the field it stands in for was (M6.6 part 6).
                                const FieldDecl& was = cls->declared[static_cast<size_t>(inherited->as.i)];
                                field.slot = was.slot;
                                field.payload = was.payload;
                                field.payloadOffset = was.payloadOffset;
                                field.payloadBit = was.payloadBit;
                                field.inlineOffset = was.inlineOffset;
                            }
                            cls->declared[static_cast<size_t>(inherited->as.i)] = field;
                            cls->shape = newShape();
                            break;
                        }
                        cls->declaredIndex.set(name, Value::integer(static_cast<int64_t>(cls->declared.size())));
                        cls->declared.push_back(field);
                        fieldAdded(cls, cls->declared.size() - 1);
                        cls->shape = newShape();
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
                            throw runtimeError(Thrown::InternalError, "invalid class declaration");
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
                            throw runtimeError(Thrown::InternalError, "invalid class declaration");
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
                        ownAll(sp_ - count, sp_);
                        list->values().assign(sp_ - count, sp_);
                        account(static_cast<size_t>(count) * sizeof(Value));
                        sp_ -= count;
                        *sp_++ = Value::object(list);
                        break;
                    }
                    case Op::TagCollection: {
                        // A list or map literal of a type that is made once (M6.6 part 1): given
                        // it, and on past the code that makes it, the first time only.
                        const uint64_t kept = caches != nullptr ? caches[FAST_U16(0)].load(std::memory_order_acquire) : 0;
                        if (kept == 0) {
                            ip += 6;
                            break;
                        }
                        ObjTypeInfo* const type = reinterpret_cast<ObjTypeInfo*>(static_cast<uintptr_t>(kept));
                        const Value made = sp_[-1];
                        if (made.isObjType(ObjType::List)) {
                            ObjList* const list = asList(made);
                            if (list->type == nullptr && type->packs.load(std::memory_order_relaxed) == 0) list->type = type;
                            else typeList(list, type);
                        }
                        else if (made.isObjType(ObjType::Map) && type->id == kTypeMap && asMap(made)->type == nullptr) asMap(made)->type = type;
                        else if (made.isObjType(ObjType::Set) && type->id == kTypeSet && asSet(made)->type == nullptr) asSet(made)->type = type;
                        ip = code + FAST_I32(2);
                        break;
                    }
                    case Op::BuildMap: {
                        int count = READ_U16();
                        ObjMap* map = alloc<ObjMap>();
                        ownAll(sp_ - 2 * count, sp_);
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
                        // At the width of the widest part (D20), room made for all of it at once.
                        Characters text;
                        size_t characters = 0;
                        for (int i = 0; i < count; i++) {
                            const Value part = sp_[i - count];
                            if (part.isString()) {
                                characters += asString(part)->length;
                                text.width = std::max(text.width, asString(part)->width);
                            } else {
                                characters += 20;
                            }
                        }
                        text.units.reserve(characters * text.width);
                        std::string shown;
                        for (int i = 0; i < count; i++) {
                            Value part = sp_[i - count];
                            if (part.isString()) {
                                appendText(text, asString(part));
                            } else {
                                shown.clear();
                                stringifyInto(shown, part, false, 0);
                                text.appendUtf8Text(shown);
                            }
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
                                throw runtimeError(Thrown::IllegalArgumentException, "the step of a range must be a positive int");
                            step = given.as.i;
                        }
                        Value end = sp_[-1], start = sp_[-2];
                        // An end may count from the end, `2..^1`, for a slice (D146, D147).
                        if (!(start.isInt() || start.isFromEnd()) || !(end.isInt() || end.isFromEnd()))
                            throw runtimeError(Thrown::ClassCastException, "range bounds must be ints, got " + typeName(start) + " and " + typeName(end));
                        if ((start.isFromEnd() || end.isFromEnd()) && (flags & 6) != 0)
                            throw runtimeError(Thrown::IllegalStateException, "a range with an end counted from the end (^) has no step and does not count down");
                        sp_[-2] = Value::object(alloc<ObjRange>(start.as.i, end.as.i, (flags & 2) ? -step : step, (flags & 1) != 0,
                                                                start.isFromEnd(), end.isFromEnd()));
                        sp_--;
                        break;
                    }
                    case Op::FromEnd: {
                        // `^k`: k from the end; ^1 is the last (D146).
                        const Value k = sp_[-1];
                        if (!k.isInt()) throw runtimeError(Thrown::ClassCastException, "^ takes an int, not " + typeName(k));
                        if (k.as.i < 1)
                            throw runtimeError(Thrown::IndexOutOfBoundsException, "^" + std::to_string(k.as.i) + " is no position: ^1 is the last, ^2 the one before it");
                        sp_[-1] = Value::fromEnd(k.as.i);
                        break;
                    }

                    case Op::IterInit:
                        SAVE_IP();
                        sp_[-1] = makeIterator(sp_[-1]);
                        break;
                    case Op::RangeLoop: {
                        const int flags = READ_U8();
                        int64_t step = 1;
                        if (flags & 4) {
                            Value given = *--sp_;
                            if (!given.isInt() || given.as.i <= 0)
                                throw runtimeError(Thrown::IllegalArgumentException, "the step of a range must be a positive int");
                            step = given.as.i;
                        }
                        Value end = sp_[-1], start = sp_[-2];
                        if (!start.isInt() || !end.isInt())
                            throw runtimeError(Thrown::ClassCastException, "range bounds must be ints, got " + typeName(start) + " and " + typeName(end));
                        if (flags & 2) step = -step;
                        sp_[-1] = Value::integer(rangeCount(start.as.i, end.as.i, step, (flags & 1) != 0));
                        *sp_++ = Value::integer(step);
                        break;
                    }
                    case Op::RangeNext: {
                        Value* const counted = base + READ_U16();  // the next value, how many are left, the step
                        const int target = READ_I32();
                        if (counted[1].as.i > 0) {
                            counted[1].as.i--;
                            *sp_++ = Value::integer(counted[0].as.i);
                            counted[0].as.i = static_cast<int64_t>(static_cast<uint64_t>(counted[0].as.i) + static_cast<uint64_t>(counted[2].as.i));
                        } else {
                            ip = code + target;
                        }
                        break;
                    }
                    case Op::DefaultArg: {
                        int slot = READ_U16();
                        int target = READ_I32();
                        if (base[slot].kind != ValueKind::Undefined) ip = code + target;
                        break;
                    }
                    case Op::IterNextRef: {
                        // As IterNext, but an item of a list of values is the item itself,
                        // where it is (M6.6 part 4).
                        const Value iterator = base[FAST_U16(0)];
                        if (iterator.isObjType(ObjType::Iterator) && asIterator(iterator)->source.isObjType(ObjType::List) &&
                            asList(asIterator(iterator)->source)->store() == ListStore::Items) {
                            ObjIterator* it = asIterator(iterator);
                            ObjList* list = asList(it->source);
                            const int target = FAST_I32(2);
                            ip += 6;
                            if (it->index < list->size()) {
                                const size_t i = it->index++;
                                if (list->pack->nullable && (list->pack->nulls[i / 64] >> (i % 64) & 1)) *sp_++ = Value::null();
                                else *sp_++ = Value::ref(list, static_cast<uint32_t>(i), 0, 0);
                            } else {
                                ip = code + target;
                            }
                            break;
                        }
                        goto iterNext;
                    }
                    case Op::IterNext:
                    iterNext: {
                        // The next item of a list or a range.
                        if (base[FAST_U16(0)].isObjType(ObjType::Iterator)) {
                            ObjIterator* it = asIterator(base[FAST_U16(0)]);
                            const Obj* source = it->source.as.obj;
                            if (source->type == ObjType::Range) {
                                if (it->remaining > 0) {
                                    it->remaining--;
                                    (sp++)->setInt(it->nextValue);
                                    it->nextValue = static_cast<int64_t>(static_cast<uint64_t>(it->nextValue) +
                                                                         static_cast<uint64_t>(static_cast<const ObjRange*>(source)->step));
                                    ip += 6;
                                } else {
                                    ip = code + FAST_I32(2);
                                }
                                continue;
                            }
                            if (source->type == ObjType::List) {
                                const ObjList* list = static_cast<const ObjList*>(source);
                                if (!list->packed()) {
                                    const std::vector<Value>& items = list->values();
                                    if (it->index < items.size()) {
                                        *sp++ = items[it->index++];
                                        ip += 6;
                                    } else {
                                        ip = code + FAST_I32(2);
                                    }
                                    continue;
                                }
                                // Packed ints, floats, bools or narrow items, null or not (G69): read from
                                // the block, but where a parallel section's band says what may be read.
                                if (list->pack->store != ListStore::Items && source != bandObject_) {
                                    if (it->index < list->pack->count) {
                                        *sp++ = packedPlain(*list->pack, it->index++);
                                        ip += 6;
                                    } else {
                                        ip = code + FAST_I32(2);
                                    }
                                    continue;
                                }
                            }
                        }
                        int slot = READ_U16();
                        int target = READ_I32();
                        Value iterator = base[slot];
                        if (!iterator.isObjType(ObjType::Iterator)) throw runtimeError(Thrown::InternalError, "invalid loop state");
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
                        // Only a Throwable is thrown (D35).
                        const Value value = sp_[-1];
                        const ObjClass* throwable = thrownClasses_[static_cast<size_t>(Thrown::Throwable)];
                        if (throwable != nullptr && (classOf(value) == nullptr || !classOf(value)->isSubclassOf(throwable)))
                            throw runtimeError(Thrown::ClassCastException, "only a Throwable can be thrown, not a value of type " + typeName(value));
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
                        if (proto == nullptr) throw runtimeError(Thrown::InternalError, "imported module failed to compile");
                        ObjClosure* closure = alloc<ObjClosure>(proto, proto->upvalues.size());
                        *sp_++ = Value::object(closure);
                        SAVE_IP();
                        callClosure(closure, 0);
                        LOAD_FRAME();
                        break;
                    }

                    case Op::Count_:
                        throw runtimeError(Thrown::InternalError, "invalid instruction");
                }
                sp = sp_;
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

// What a function returns that is a Ref (M6.6 part 4): one into a list, or into an object
// (part 5), goes no further than the function that made it, unless it is where the caller asked
// for the result to go.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
Value VM::returnedRef(Value result, ptrdiff_t frameIndex) {
    if ((frames_[frameIndex].closure->proto->typeArgs & kTakesDestination) != 0) {
        const Value& dest = frameDestination(frameIndex);
        if (dest.kind == ValueKind::Undefined) return result;  // the caller keeps it as it is (Op::WithRefResult)
        if (dest.isRef() && dest.as.obj == result.as.obj && dest.refIndex == result.refIndex && dest.refOffset == result.refOffset &&
            dest.refDepth == result.refDepth)
            return result;
    }
    if (result.as.obj->type != ObjType::List && result.as.obj->type != ObjType::Instance) return result;
    return ownRef(result);
}

// The instructions that give a place (M6.6 part 4), and InsertNew, which makes an item in its
// place (G7), kept out of run() so that what they need does not weigh on the instructions run
// most. Gives where the next instruction is.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
const uint8_t* VM::placeInstruction(Op op, const uint8_t* ip, CallFrame* frame) {
    const Value* const consts = frame->closure->proto->constants.data();
    switch (op) {
        case Op::PlaceLocal: {
            Value& slot = frame->base[(ip += 2, operandU16(ip - 2))];
            const Value place = placeIn(slot);
            *sp_++ = place;
            break;
        }
        case Op::PlaceUpvalue: {
            ObjUpvalue* upvalue = frame->closure->upvalues[static_cast<size_t>((ip += 2, operandU16(ip - 2)))];
            if (upvalue->core != coreId_) throw notOwned("a variable from outside the section is shared");
            const Value place = placeIn(*upvalue->location);
            *sp_++ = place;
            break;
        }
        case Op::PlaceGlobal: {
            const int index = (ip += 2, operandU16(ip - 2));
            if (coreId_ != 0) throw notOwned("the variable '" + program_->globalNames[static_cast<size_t>(index)] + "' is shared");
            if (globals_[static_cast<size_t>(index)].isLazy()) globalOrLazy(static_cast<size_t>(index));
            const Value place = placeIn(globals_[static_cast<size_t>(index)]);
            *sp_++ = place;
            break;
        }
        case Op::PlaceField: {
            ObjString* name = asString(consts[(ip += 2, operandU16(ip - 2))]);
            const Value obj = sp_[-1];
            if (obj.isRef() || obj.isObjType(ObjType::Value)) {
                // A field of a value that is itself a place: where it is in it.
                Value read;
                if (obj.isRef() ? refField(obj, name, read) : false) {
                    sp_[-1] = read;
                    break;
                }
            }
            if (!obj.isObjType(ObjType::Instance)) throw runtimeError(Thrown::UnsupportedOperationException, "cannot change a field of a value of type " + typeName(obj));
            ObjInstance* instance = asInstance(obj);
            guardWrite(instance);
            const FieldDecl* decl = instance->declOf(name);
            if (decl == nullptr) throw runtimeError(Thrown::UnsupportedOperationException, "undefined property '" + name->utf8() + "' on " + instance->cls->name->utf8());
            if (decl->isVal) throw runtimeError(Thrown::UnsupportedOperationException, "cannot assign to val field '" + name->utf8() + "' of " + instance->cls->name->utf8());
            if (decl->payload != FieldDecl::NoPayload) {
                Value kept = Value::null();
                instance->read(name, kept);
                throw runtimeError(Thrown::NullPointerException, "cannot change a field of " + (kept.isNull() ? std::string("null") : "a value of type " + typeName(kept)) +
                                   " through its place");
            }
            sp_[-1] = placeIn(instance->fields()[decl->slot]);
            break;
        }
        case Op::PlaceIndex: {
            const Value index = sp_[-1];
            const Value obj = sp_[-2];
            if (!obj.isObjType(ObjType::List)) throw runtimeError(Thrown::UnsupportedOperationException, "cannot change an item of a value of type " + typeName(obj));
            ObjList* list = asList(obj);
            guardWrite(list);
            size_t i;
            if (!position(index, list->size(), i, "list index")) throw outOfRange("list index", index, list->size());
            sp_--;
            if (list->store() == ListStore::Items) {
                if (list->pack->nullable && (list->pack->nulls[i / 64] >> (i % 64) & 1)) sp_[-1] = Value::null();  // for `pts[i]?.move()` to pass by
                else sp_[-1] = Value::ref(list, static_cast<uint32_t>(i), 0, 0);  // the item, where it is
            } else {
                sp_[-1] = placeIn(list->values()[i]);
            }
            break;
        }
        case Op::InsertNew: {
            const int argc = *ip;
            const int target = operandI32(ip + 1);
            ip += 5;
            if (insertNew(argc & 0x7F, (argc & 0x80) != 0)) ip = frame->closure->proto->code.data() + target;
            break;
        }
        default:
            break;
    }
    return ip;
}

// `pts.insert(i, Point(1, 2))` and `pts.add(i, Point(1, 2))` (Op::InsertNew, G7): as addNew, the
// position under the class, an int from 0 to the list's size; the item is made at the end and
// moved to its place, as an insert moves the items after it. Its result is null, as insert
// gives, or true, as add(i, item) gives. False, with the stack as it was, where addNew does
// nothing or the position is not one, and the call and the insert or add follow and say why.
// Kept after run(), as addNew is before it, so that run() stays where it was (G7: numeric was 14%
// slower with run() moved).
bool VM::insertNew(int argc, bool givesTrue) {
    Value* const args = sp_ - argc;
    if (!args[-3].isObjType(ObjType::List)) return false;
    ObjList* list = asList(args[-3]);
    const Value position = args[-2];
    if (list->store() != ListStore::Items || !position.isInt() || position.as.i < 0 || static_cast<uint64_t>(position.as.i) > list->pack->count)
        return false;
    const size_t at = static_cast<size_t>(position.as.i);
    // The position taken from under the class for addNew, and put back if it does nothing.
    std::memmove(args - 2, args - 1, static_cast<size_t>(argc + 1) * sizeof(Value));
    sp_--;
    if (!addNew(argc)) {
        std::memmove(args - 1, args - 2, static_cast<size_t>(argc + 1) * sizeof(Value));
        args[-2] = position;
        sp_++;
        return false;
    }
    PackedItems& pack = *list->pack;
    const size_t width = pack.width;
    // The new item kept aside, the items after `at` moved on by one in one move, and the item put
    // in the gap (std::rotate swaps byte by byte: twice as slow at the front of a long list).
    uint8_t* const base = pack.block.data();
    const size_t last = (pack.count - 1) * width;
    uint8_t small[256];
    std::vector<uint8_t> large;
    uint8_t* const made = width <= sizeof small ? small : (large.resize(width), large.data());
    std::memcpy(made, base + last, width);
    std::memmove(base + (at + 1) * width, base + at * width, last - at * width);
    std::memcpy(base + at * width, made, width);
    if (pack.nullable) {
        for (size_t i = pack.count - 1; i > at; i--) setNullAt(list, i, nullAt(list, i - 1));
        setNullAt(list, at, false);
    }
    if (!givesTrue) sp_[-1] = Value::null();
    return true;
}

// The instructions of types when the program runs (M6.6 part 1), kept out of run()'s loop: they
// are seldom run, and code in the loop costs every other instruction there. Gives where the
// next instruction is.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
const uint8_t* VM::typeInstruction(Op op, const uint8_t* ip, CallFrame* frame) {
    const uint8_t* const code = frame->closure->proto->code.data();
    const Value* const consts = frame->closure->proto->constants.data();
    std::atomic<uint64_t>* const caches = frame->closure->proto->caches.get();
    switch (op) {
        case Op::MakeType: {
            const uint8_t* site = ip - 1;
            const std::string& shape = asString(consts[(ip += 2, operandU16(ip - 2))])->utf8();
            const int count = shapeOperands(shape);
            const Value* operand = sp_ - count;
            // The same types as the last time here: the same type, with no look-up in the table
            // of types, which a core takes a lock for (G3). Only types are keys: they are never
            // freed, where a class may be and its address taken again.
            MadeTypeCache* kept = nullptr;
            if (count >= 1 && count <= MadeTypeCache::kMost) {
                bool types = true;
                for (int k = 0; k < count; k++) types = types && operand[k].isObjType(ObjType::TypeInfo);
                if (types) {
                    kept = &madeTypes_[(reinterpret_cast<uintptr_t>(site) >> 2) % kMadeTypeCaches];
                    const uint64_t age = (root_ != nullptr ? root_ : this)->collections_;
                    bool same = kept->site == site && kept->age == age;
                    for (int k = 0; same && k < count; k++) same = kept->operands[k] == operand[k].as.obj;
                    if (same) {
                        sp_ -= count;
                        *sp_++ = Value::object(kept->made);
                        break;
                    }
                }
            }
            size_t at = 0;
            const Value* read = operand;  // typeFromShape moves it past what it reads
            ObjTypeInfo* made = typeFromShape(shape, at, read);
            if (kept != nullptr) {
                kept->site = site;
                kept->age = (root_ != nullptr ? root_ : this)->collections_;
                for (int k = 0; k < count; k++) kept->operands[k] = operand[k].as.obj;
                kept->made = made;
            }
            sp_ -= count;
            *sp_++ = Value::object(made);
            break;
        }
        case Op::FrameTypes:
            pendingTypeArgs_ = frameTypeArgs(frame - frames_);
            break;
        case Op::CachedType: {
            const int cache = (ip += 2, operandU16(ip - 2));
            const int target = (ip += 4, operandI32(ip - 4));
            const uint64_t kept = caches != nullptr ? caches[cache].load(std::memory_order_acquire) : 0;
            if (kept != 0) {
                *sp_++ = Value::object(reinterpret_cast<ObjTypeInfo*>(static_cast<uintptr_t>(kept)));
                ip = code + target;
            }
            break;
        }
        case Op::CacheType: {
            const int cache = (ip += 2, operandU16(ip - 2));
            if (caches != nullptr && sp_[-1].isObjType(ObjType::TypeInfo))
                caches[cache].store(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sp_[-1].as.obj)), std::memory_order_release);
            break;
        }
        case Op::TypeCollection: {
            if (!sp_[-1].isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
            ObjTypeInfo* type = static_cast<ObjTypeInfo*>(sp_[-1].as.obj);
            const Value made = sp_[-2];
            // What it was made to hold, said once: a list a C++ function gave with a type of
            // its own keeps it.
            if (made.isObjType(ObjType::List)) typeList(asList(made), type);
            else if (made.isObjType(ObjType::Map) && type->id == kTypeMap && asMap(made)->type == nullptr) asMap(made)->type = type;
                        else if (made.isObjType(ObjType::Set) && type->id == kTypeSet && asSet(made)->type == nullptr) asSet(made)->type = type;
            sp_--;
            break;
        }
        case Op::IsFull: {
            if (!sp_[-1].isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
            const bool fits = fitsType(sp_[-2], static_cast<const ObjTypeInfo*>(sp_[-1].as.obj));
            sp_[-2].setBool(fits);
            sp_--;
            break;
        }
        case Op::WithTypes: {
            if (!sp_[-1].isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
            pendingTypeArgs_ = static_cast<const ObjTypeInfo*>(sp_[-1].as.obj);
            sp_--;
            break;
        }
        case Op::TypeArg: {
            const int index = (*ip++);
            *sp_++ = Value::object(typeArgument(frameTypeArgs(frame - frames_), index));
            break;
        }
        case Op::InstanceTypeArg: {
            // The type argument of the class that declares the code, which the object may extend.
            const int index = (*ip++);
            const ObjTypeInfo* type = nullptr;
            if (sp_[-2].isObjType(ObjType::Instance) && sp_[-1].isObjType(ObjType::Class)) {
                ObjInstance* instance = asInstance(sp_[-2]);
                type = instance->cls == asClass(sp_[-1]) ? instance->type() : typeAs(instance->cls, instance->type(), asClass(sp_[-1]));
            }
            sp_--;
            sp_[-1] = Value::object(typeArgument(type, index));
            break;
        }
        case Op::CheckResult: {
            const std::string& name = asString(consts[(ip += 2, operandU16(ip - 2))])->utf8();
            if (!sp_[-1].isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
            const ObjTypeInfo* type = static_cast<const ObjTypeInfo*>(sp_[-1].as.obj);
            if (!fitsType(sp_[-2], type))
                throw runtimeError(Thrown::InternalError, "the C++ function " + name + "() gave a value of type " + valueTypeText(sp_[-2]) + ", where its declaration says " +
                                   typeInfoText(type));
            sp_--;
            break;
        }
        case Op::ClassOf: {
            const int how = *ip++;
            Value& top = sp_[-1];
            ObjTypeInfo* type = nullptr;
            if (how == 0) {
                if (!top.isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
                type = static_cast<ObjTypeInfo*>(top.as.obj);
            } else {
                frame->ip = ip;
                type = typeOfValue(top);
            }
            top = Value::object(type);
            *sp_++ = Value::object(frame->closure);
            break;
        }
        case Op::AtomicField: {
            ObjString* name = asString(frame->closure->proto->constants[static_cast<size_t>(operandU16(ip))]);
            ip += 2;
            const int kind = *ip++;
            frame->ip = ip;
            const int operands = kind == 4 ? 2 : 1;
            const Value result = atomicField(sp_[-operands - 1], name, kind, sp_ - operands);
            sp_ -= operands;
            sp_[-1] = result;
            break;
        }
        case Op::ClassMeta: {
            const FunctionProto* proto = frame->closure->proto->protos[operandU16(ip)];
            ip += 2;
            const int which = *ip++;
            if (!sp_[-1].isObjType(ObjType::Class) || which > 1) throw runtimeError(Thrown::InternalError, "invalid class declaration");
            ObjClass* cls = asClass(sp_[-1]);
            cls->metaProto[which] = proto;
            cls->meta[which] = nullptr;
            break;
        }
        case Op::SupertypeMaker: {
            if (!sp_[-2].isObjType(ObjType::Class) || !sp_[-1].isObjType(ObjType::Closure)) throw runtimeError(Thrown::InternalError, "invalid class declaration");
            asClass(sp_[-2])->supertypeMakers.push_back(asClosure(sp_[-1]));
            sp_--;
            break;
        }
        case Op::CastFull: {
            const bool safe = (*ip++) != 0;
            if (!sp_[-1].isObjType(ObjType::TypeInfo)) throw runtimeError(Thrown::InternalError, "invalid type");
            const ObjTypeInfo* type = static_cast<const ObjTypeInfo*>(sp_[-1].as.obj);
            if (!fitsType(sp_[-2], type)) {
                if (!safe) {
                    frame->ip = ip;
                    throw runtimeError(Thrown::ClassCastException, "cannot cast a value of type " + valueTypeText(sp_[-2]) + " to " + typeInfoText(type));
                }
                sp_[-2] = Value::null();
            }
            sp_--;
            break;
        }
        default:
            break;
    }
    return ip;
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
    if (v.isRef() || v.isSmall()) v = boxed(v);  // printed as the value it is
    TempRoot keepValue(*this, v);
    switch (v.kind) {
        case ValueKind::Null: out += "null"; return;
        case ValueKind::Bool: out += v.as.b ? "true" : "false"; return;
        case ValueKind::Int:
        case ValueKind::Byte: out += std::to_string(v.as.i); return;
        case ValueKind::Float: out += formatFloat(v.as.d); return;
        case ValueKind::Char:
            if (quote) out += quoteCharacter(static_cast<uint32_t>(v.as.i));
            else appendUtf8(out, static_cast<uint32_t>(v.as.i));
            return;
        case ValueKind::FromEnd: out += "^" + std::to_string(v.as.i); return;
        case ValueKind::Undefined: out += "undefined"; return;
        case ValueKind::Obj: break;
    }

    guardRead(v.as.obj);
    switch (v.as.obj->type) {
        case ObjType::String:
            if (quote) out += quoteString(asString(v)->utf8());
            else out += asString(v)->utf8();
            return;

        case ObjType::List: {
            if (depth > 16) {
                out += "[...]";
                return;
            }
            ObjList* list = asList(v);
            out += '[';
            // Indexed and re-checked each time round: a toString() may change the list.
            for (size_t i = 0; i < list->size(); i++) {
                if (i > 0) out += ", ";
                const Value item = listGet(list, i);
                TempRoot keep(*this, item);
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

        case ObjType::Set: {
            // A map's values (G22) are written as a list is: they may repeat, and have an order.
            const bool values = asSet(v)->values;
            if (depth > 16) {
                out += values ? "[...]" : "{...}";
                return;
            }
            out += values ? '[' : '{';
            const std::vector<Value> items = setItems(asSet(v));
            TempVector keep(*this, const_cast<std::vector<Value>&>(items));
            for (size_t i = 0; i < items.size(); i++) {
                if (i > 0) out += ", ";
                stringifyInto(out, items[i], true, depth + 1);
            }
            out += values ? ']' : '}';
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
            out += (range->startFromEnd ? "^" : "") + std::to_string(range->start);
            out += range->inclusive ? ".." : "..<";
            out += (range->endFromEnd ? "^" : "") + std::to_string(range->end);
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
            out += "<class " + asClass(v)->name->utf8() + ">";
            return;

        case ObjType::Value: {
            // A value prints as a data object does, Point(x=1, y=2), unless it says otherwise.
            ObjValue* box = asValue(v);
            const Value* method = box->cls->methods.find(names.toString);
            if (method != nullptr && asClosure(*method)->proto->arity == 0) {
                Value result = callWithReceiver(v, asClosure(*method));
                if (result.isString()) {
                    out += asString(result)->utf8();
                } else {
                    TempRoot root(*this, result);
                    stringifyInto(out, result, false, depth + 1);
                }
                return;
            }
            out += box->cls->name->utf8() + "(";
            if (depth > 20) {
                out += "...)";
                return;
            }
            bool first = true;
            for (ObjString* part : box->cls->components) {
                if (!first) out += ", ";
                first = false;
                out += part->utf8() + "=";
                const int64_t at = valueFieldIndex(box, part);
                const Value field = at >= 0 ? valueField(box, static_cast<size_t>(at)) : Value::null();
                TempRoot keep(*this, field);
                stringifyInto(out, field, true, depth + 1);
            }
            out += ")";
            return;
        }
        case ObjType::Instance: {
            ObjInstance* instance = asInstance(v);
            const Value* method = instance->cls->methods.find(names.toString);
            if (method != nullptr && asClosure(*method)->proto->arity == 0) {
                Value bound = Value::object(alloc<ObjBoundMethod>(v, asClosure(*method)));
                Value result = call(bound, 0, nullptr);
                if (result.isString()) {
                    out += asString(result)->utf8();
                } else {
                    TempRoot root(*this, result);
                    stringifyInto(out, result, false, depth + 1);
                }
                return;
            }
            if (instance->cls->isData) {
                // A data object prints as its class and header fields: Point(x=1, y=2).
                out += instance->cls->name->utf8() + "(";
                if (depth > 20) {
                    out += "...)";
                    return;
                }
                bool first = true;
                for (ObjString* part : instance->cls->components) {
                    if (!first) out += ", ";
                    first = false;
                    out += part->utf8() + "=";
                    Value field = Value::null();
                    instance->read(part, field);
                    stringifyInto(out, field, true, depth + 1);
                }
                out += ")";
                return;
            }
            if (instance->cls->isEnum) {
                // An enum entry prints as its name.
                Value entryName = Value::null();
                instance->read(names.entryName, entryName);
                if (entryName.isString()) {
                    out += asString(entryName)->utf8();
                    return;
                }
            }
            out += "<" + instance->cls->name->utf8() + " instance>";
            return;
        }

        case ObjType::Fiber:
            out += "<task>";
            return;

        case ObjType::Resource:
            out += std::string("<") + static_cast<const ObjResource*>(v.as.obj)->kind + ">";
            return;

        case ObjType::Upvalue:
        case ObjType::Iterator:
            out += "<internal>";
            return;

        case ObjType::TypeInfo:
            out += typeInfoText(static_cast<const ObjTypeInfo*>(v.as.obj));
            return;
    }
}

// ---- Garbage collector (mark and sweep) ----

void VM::collectGarbage() {
    liveBytes_ = 0;
    collections_++;  // what MadeType kept may name code freed now

    // The context that is running, whose state is in the VM's own members...
    for (Value* slot = stack_; slot < sp_; slot++) markValue(*slot);
    for (int i = 0; i < frameCount_; i++) {
        markObject(frames_[static_cast<size_t>(i)].closure);
        if ((frames_[static_cast<size_t>(i)].closure->proto->typeArgs & kTakesDestination) != 0) markValue(frameDestination(i));
    }
    for (ObjUpvalue* upvalue = openUpvalues_; upvalue != nullptr; upvalue = upvalue->nextOpen) markObject(upvalue);
    markObject(context_->fiber);
    markValue(pendingDestination_);
    for (ObjClass* cls : smallKept_) markObject(cls);
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
    for (const auto& [key, type] : types_) markObject(type);  // what the types name, their classes
    markTable(nullExtensions_);
    markTable(nullExtensionOwners_);
    markTable(anyExtensions_);
    for (const auto& builtin : builtins_) markValue(builtin.second);
    for (const Value& root : tempRoots) markValue(root);
    for (const Value& root : pinned) markValue(root);
    for (const std::vector<Value>* values : tempVectors)
        for (const Value& value : *values) markValue(value);
    markObject(errorClass_);
    for (ObjClass* cls : thrownClasses_) markObject(cls);
    markObject(mapEntryClass_);
    markObject(viewClass_);
    for (const TypeConstraint& declared : globalTypes_) markObject(declared.cls);

    while (!gray_.empty()) {
        Obj* obj = gray_.back();
        gray_.pop_back();
        blacken(obj);
    }

    releaseUnreachedBlocks();
    sweep();
    bytesAllocated_ = liveBytes_;
    nextGC_ = gcStress_ ? 0 : std::max(liveBytes_ * 2, kMinHeap);
}

void VM::markContext(const Context& context) {
    for (const Value* slot = context.stack; slot < context.sp; slot++) markValue(*slot);
    for (int i = 0; i < context.frameCount; i++) {
        markObject(context.frames[i].closure);
        if ((context.frames[i].closure->proto->typeArgs & kTakesDestination) != 0)
            markValue(reinterpret_cast<const Value*>(reinterpret_cast<const ObjTypeInfo* const*>(context.frames + context.maxFrames) +
                                                     context.maxFrames)[i]);
    }
    for (ObjUpvalue* upvalue = context.openUpvalues; upvalue != nullptr; upvalue = upvalue->nextOpen) markObject(upvalue);
    markObject(context.fiber);
}

void VM::markObject(Obj* obj) {
    if (obj == nullptr || obj->marked) return;
    if (obj->outside) [[unlikely]] {
        noteOutside(obj);  // another heap's to free: only noted as reached (D97)
        return;
    }
    obj->marked = true;
    if (obj->type == ObjType::String) {
        // Strings have no references to trace. Interned ones are not counted as collectable heap.
        if (!obj->immortal) liveBytes_ += sizeof(ObjString) + static_cast<ObjString*>(obj)->heldBytes();
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
            liveBytes_ += sizeof(ObjList) + list->values().capacity() * sizeof(Value);
            if (list->packed())
                liveBytes_ += sizeof(PackedItems) + list->pack->block.capacity() + list->pack->nulls.capacity() * sizeof(uint64_t);
            for (const Value& item : list->values()) markValue(item);
            markItems(list);
            break;
        }
        case ObjType::Lazy: {
            ObjLazy* lazy = static_cast<ObjLazy*>(obj);
            liveBytes_ += sizeof(ObjLazy);
            markObject(lazy->init);
            markValue(lazy->value);
            break;
        }
        case ObjType::Set: {
            ObjSet* set = static_cast<ObjSet*>(obj);
            liveBytes_ += sizeof(ObjSet) + set->items.entries().size() * 64;
            for (const ValueMap::Entry& entry : set->items.entries())
                if (entry.live) markValue(entry.key);
            markObject(set->viewOf);
            markObject(set->type);
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
            // While it is open its variable lives on a stack. If that is the stack of a task,
            // the task is kept, and marks the stack itself if it has been set aside.
            if (upvalue->owner != nullptr) markObject(upvalue->owner);
            else markValue(*upvalue->location);
            break;
        }
        case ObjType::Resource:
            liveBytes_ += sizeof(ObjResource);
            break;
        case ObjType::TypeInfo: {
            ObjTypeInfo* type = static_cast<ObjTypeInfo*>(obj);
            markObject(type->cls);
            for (ObjTypeInfo* arg : type->args) markObject(arg);
            break;
        }
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
            for (ObjClosure* maker : cls->supertypeMakers) markObject(maker);
            markObject(cls->meta[0]);
            markObject(cls->meta[1]);
            for (const auto& described : cls->described)
                for (const auto& [type, made] : described) markValue(made);
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
            for (ObjClass* fitted : cls->extendedTo) markObject(fitted);
            markTable(cls->extendMethods);
            markTable(cls->extendGetters);
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
        case ObjType::Value: {
            // What a value holds of other objects, where its type's layout says.
            ObjValue* box = static_cast<ObjValue*>(obj);
            markObject(box->cls);
            const ValueLayout* layout = box->cls->layout.load(std::memory_order_acquire);
            liveBytes_ += sizeof(ObjValue) + (layout != nullptr ? layout->size : 0);
            if (layout == nullptr) break;
            for (uint32_t offset : layout->objects) {
                Obj* held;
                std::memcpy(&held, box->data() + offset, sizeof held);
                markObject(held);
            }
            for (uint32_t offset : layout->values) {
                Value held;
                std::memcpy(&held, box->data() + offset, sizeof held);
                markValue(held);
            }
            break;
        }
        case ObjType::Instance: {
            ObjInstance* instance = static_cast<ObjInstance*>(obj);
            liveBytes_ += instance->slotsStart + (instance->fieldCount + instance->captureCount) * sizeof(Value);  // its payloads too (M6.6 part 6)
            markObject(instance->cls);
            const Value* fields = instance->fields();
            for (uint32_t i = 0; i < instance->fieldCount + instance->captureCount; i++) markValue(fields[i]);
            if (instance->inlined) {
                // The values kept inside it (M6.6 part 5): what they hold, where their layouts say.
                liveBytes_ += instance->cls->inlineSlots * sizeof(Value);
                for (const ObjClass::InlineField& inside : instance->cls->inlineFields) {
                    const ValueLayout* layout = inside.cls->layout.load(std::memory_order_acquire);
                    const uint8_t* at = instance->inlineData() + inside.offset;
                    for (uint32_t offset : layout->objects) {
                        Obj* held;
                        std::memcpy(&held, at + offset, sizeof held);
                        if (held != nullptr) markObject(held);
                    }
                    for (uint32_t offset : layout->values) {
                        Value held;
                        std::memcpy(&held, at + offset, sizeof held);
                        markValue(held);
                    }
                }
            }
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
            if (!sharedAs.empty()) [[unlikely]] {
                // Frozen data shared with workers: what it was shared as is let go of with it.
                if (auto shared = sharedAs.find(obj); shared != sharedAs.end()) {
                    reachedBlocks.erase(shared->second);
                    sharedAs.erase(shared);
                }
            }
            delete obj;
        }
    }
}

}  // namespace astra
