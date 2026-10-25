// Reflection (M6.6 part 8, group 6; D38, D48 to D50): what the standard library's Class,
// Field and Method (src/std/reflect.astra) read of the program's classes as it runs.
#include <algorithm>

#include "builtins.h"

namespace astra {

namespace {

const ObjTypeInfo* typeGiven(VM& vm, const Value& v) {
    if (!v.isObjType(ObjType::TypeInfo)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a Type is needed, not " + vm.typeName(v));
    return static_cast<const ObjTypeInfo*>(v.as.obj);
}

ObjTypeInfo* typeType(VM& vm) { return vm.typeOf(kTypeType, false, nullptr, {}); }

// A List<Type> of the types given.
Value typeList(VM& vm, const std::vector<const ObjTypeInfo*>& types) {
    std::vector<Value> items;
    items.reserve(types.size());
    for (const ObjTypeInfo* t : types) items.push_back(Value::object(const_cast<ObjTypeInfo*>(t)));
    ObjList* list = newList(vm, std::move(items));
    vm.typeList(list, vm.typeOf(kTypeList, false, nullptr, {typeType(vm)}));
    return Value::object(list);
}

// `target`, a class `type`'s class extends or implements, with the type arguments `type` gives it.
const ObjTypeInfo* seenAs(VM& vm, const ObjTypeInfo* type, ObjClass* target) {
    const ObjTypeInfo* made = vm.typeAs(type->cls, type->args.empty() ? nullptr : type, target);
    return made != nullptr ? made : vm.typeOf(kTypeInstance, false, target, {});
}

// ---- What a class says of itself ----
//
// The compiler gives each class two functions describing it (Op::ClassMeta, compileMeta), which
// VM::classDescription calls with the class's type arguments. The first gives
//   [fields, the class's annotations, an enum class's kinds]
// each field [name, type, flags (1 it may be set, 2 worked out by a getter), annotations]; the
// second
//   [methods, constructors]
// each method [name, the name the class keeps it under, parameters, result (null for none),
// annotations], each constructor [parameters, annotations], and each parameter [name, type,
// whether it may be left out, annotations]. A class lists only what it declares itself; the
// members it has are gathered here from it and the classes above it.

// The function a Class was made in, whose rights to members it has; null for none.
const FunctionProto* askerOf(const Value& asker) { return asker.isObjType(ObjType::Closure) ? asClosure(asker)->proto : nullptr; }

// Part `i` of what a description or an entry of one gives.
Value partOf(VM& vm, const Value& list, size_t i) {
    if (!list.isObjType(ObjType::List) || asList(list)->size() <= i) throw vm.runtimeError(Thrown::InternalError, "a class's description is malformed");
    return vm.listGet(asList(list), i);
}

// The classes whose members an object of the type has, the highest first, each with the type
// it is seen as (null where it has no type arguments); with `interfaces`, its interfaces first.
std::vector<std::pair<ObjClass*, const ObjTypeInfo*>> lineage(VM& vm, const ObjTypeInfo* t, bool interfaces) {
    std::vector<std::pair<ObjClass*, const ObjTypeInfo*>> classes;
    auto add = [&](ObjClass* k) {
        const ObjTypeInfo* own = k == t->cls ? t : seenAs(vm, t, k);
        classes.emplace_back(k, own->args.empty() ? nullptr : own);
    };
    if (interfaces)
        for (ObjClass* iface : t->cls->interfaces) add(iface);
    std::vector<ObjClass*> chain;
    for (ObjClass* k = t->cls; k != nullptr; k = k->super) chain.push_back(k);
    for (auto k = chain.rbegin(); k != chain.rend(); ++k) add(*k);
    return classes;
}

// The fields (`which` 0) or methods (1) an object of the type has, as far as the code of
// `asker` may use them: those of the classes above first, one a class below declares again in
// their place. A field the code may not assign to is listed as one it may not set.
Value members(VM& vm, const ObjTypeInfo* t, const FunctionProto* asker, int which) {
    ObjList* out = newList(vm);
    TempRoot keep(vm, Value::object(out));
    if (t->id != kTypeInstance) return Value::object(out);
    std::vector<Value> found;
    std::unordered_map<ObjString*, size_t> at;
    for (const auto& [k, own] : lineage(vm, t, which == 1)) {
        const Value description = vm.classDescription(k, own, which);
        if (description.isNull()) continue;
        const Value entries = partOf(vm, description, 0);
        for (size_t i = 0; i < asList(entries)->size(); i++) {
            const Value entry = vm.listGet(asList(entries), i);
            ObjString* key = asString(partOf(vm, entry, which == 0 ? 0 : 1));
            auto [place, added] = at.emplace(key, found.size());
            if (added) found.push_back(entry);
            else found[place->second] = entry;
        }
    }
    for (const Value& entry : found) {
        ObjString* name = asString(partOf(vm, entry, 0));
        if (vm.memberDenied(t->cls, name, asker, false)) continue;
        if (which == 0 && (partOf(vm, entry, 2).as.i & 1) != 0 && vm.memberDenied(t->cls, name, asker, true)) {
            // Listed as one that cannot be set here.
            std::vector<Value> parts = asList(entry)->values();
            parts[2] = Value::integer(parts[2].as.i & ~int64_t{1});
            out->values().push_back(Value::object(newList(vm, std::move(parts))));
            continue;
        }
        out->values().push_back(entry);
    }
    return Value::object(out);
}

// Part `part` of what the class itself says (`which` 0 or 1), not those above it: an empty
// list where it says nothing.
Value ownPart(VM& vm, const ObjTypeInfo* t, int which, size_t part) {
    if (t->id == kTypeInstance) {
        const Value description = vm.classDescription(t->cls, t->args.empty() ? nullptr : t, which);
        if (!description.isNull()) return partOf(vm, description, part);
    }
    return Value::object(newList(vm));
}

// The values a call by name is given, from the list it was given them in.
std::vector<Value> argumentsOf(VM& vm, const Value& list) {
    if (!list.isObjType(ObjType::List)) throw vm.runtimeError(Thrown::IllegalArgumentException, "the arguments are given as a list");
    std::vector<Value> values;
    for (size_t i = 0; i < asList(list)->size(); i++) values.push_back(vm.listGet(asList(list), i));
    if (values.size() > 255) throw vm.runtimeError(Thrown::IllegalArgumentException, "a call cannot have more than 255 arguments");
    return values;
}

}  // namespace

void registerReflection(VM& vm) {
    // The fields and the methods an object of the type has, as far as the code that made the
    // Class may use them.
    vm.defineNative("__typeFields", 2, 2, [](VM& vm, int, Value* args) -> Value {
        return members(vm, typeGiven(vm, args[0]), askerOf(args[1]), 0);
    });
    vm.defineNative("__typeMethods", 2, 2, [](VM& vm, int, Value* args) -> Value {
        return members(vm, typeGiven(vm, args[0]), askerOf(args[1]), 1);
    });
    // The class's own constructors, annotations and kinds.
    vm.defineNative("__typeConstructors", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return ownPart(vm, typeGiven(vm, args[0]), 1, 1);
    });
    vm.defineNative("__typeAnnotations", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return ownPart(vm, typeGiven(vm, args[0]), 0, 1);
    });
    vm.defineNative("__typeKinds", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return ownPart(vm, typeGiven(vm, args[0]), 0, 2);
    });

    // A member used by its name, whoever may use it: what a Field or a Method listed.
    vm.defineNative("__memberGet", 2, 2, [](VM& vm, int, Value* args) -> Value {
        return vm.memberGet(args[0], vm.intern(vm.toStr(args[1], "a member's name")));
    });
    vm.defineNative("__memberSet", 3, 3, [](VM& vm, int, Value* args) -> Value {
        vm.memberSet(args[0], vm.intern(vm.toStr(args[1], "a member's name")), args[2]);
        return Value::null();
    });
    vm.defineNative("__memberCall", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const Value method = vm.memberGet(args[0], vm.intern(vm.toStr(args[1], "a method's name")));
        TempRoot keep(vm, method);
        const std::vector<Value> values = argumentsOf(vm, args[2]);
        return vm.call(method, static_cast<int>(values.size()), values.data());
    });
    // A new object of the class, made with its type arguments.
    vm.defineNative("__typeConstruct", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const ObjTypeInfo* t = typeGiven(vm, args[0]);
        if (t->id != kTypeInstance) throw vm.runtimeError(Thrown::IllegalArgumentException, vm.typeInfoText(t) + " is made by no constructor");
        const std::vector<Value> values = argumentsOf(vm, args[1]);
        return vm.callWithTypes(Value::object(t->cls), t->args, static_cast<int>(values.size()), values.data());
    });

    // The library a type comes from: "astra" for a built-in type, a library's class its library's
    // name; null for a class of the program's own, whose origin is the program's name (D184).
    vm.defineNative("__typeOrigin", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const ObjTypeInfo* t = typeGiven(vm, args[0]);
        if (t->id != kTypeInstance) return vm.str("astra");
        return t->cls->origin != nullptr ? Value::object(t->cls->origin) : Value::null();
    });

    // A class's name, or a built-in type's without what it holds: "Circle", "List".
    vm.defineNative("__typeName", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const ObjTypeInfo* t = typeGiven(vm, args[0]);
        if (t->id == kTypeInstance) return Value::object(t->cls->name);
        return Value::object(vm.intern(vm.typeInfoText(vm.typeOf(t->id, false, nullptr, {}))));
    });

    // The superclass, with the type arguments the class gives it; null for none.
    vm.defineNative("__typeSuper", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const ObjTypeInfo* t = typeGiven(vm, args[0]);
        if (t->id != kTypeInstance || t->cls->super == nullptr) return Value::null();
        return Value::object(const_cast<ObjTypeInfo*>(seenAs(vm, t, t->cls->super)));
    });

    // Every interface the class implements, those above it and those an extend fitted it to
    // (D33) included, each once.
    vm.defineNative("__typeInterfaces", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const ObjTypeInfo* t = typeGiven(vm, args[0]);
        std::vector<const ObjTypeInfo*> found;
        if (t->id == kTypeInstance) {
            std::vector<ObjClass*> classes = t->cls->interfaces;
            for (const ObjClass* k = t->cls; k != nullptr; k = k->super)
                for (ObjClass* fitted : k->extendedTo)
                    if (std::find(classes.begin(), classes.end(), fitted) == classes.end()) classes.push_back(fitted);
            for (ObjClass* iface : classes) found.push_back(seenAs(vm, t, iface));
        }
        return typeList(vm, found);
    });

    // What the type was given: `[int]` for a Box<int>, a List<int>'s `[int]`.
    vm.defineNative("__typeArguments", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const ObjTypeInfo* t = typeGiven(vm, args[0]);
        return typeList(vm, std::vector<const ObjTypeInfo*>(t->args.begin(), t->args.end()));
    });

    // Whether a value is of the type, its type arguments included.
    vm.defineNative("__typeHas", 2, 2, [](VM& vm, int, Value* args) -> Value {
        return Value::boolean(vm.fitsType(args[1], typeGiven(vm, args[0])));
    });
}

}  // namespace astra
