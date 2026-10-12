#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bytecode.h"
#include "value.h"

namespace astra {

// A value thrown by an Astra program (or a runtime error) travelling up the host stack.
struct AstraThrow {
    Value value;

    // Call stack at the throw site, innermost first. Only captured when nothing will catch it.
    std::vector<std::string> trace;
    bool hasTrace = false;
};

// Raised by the exit() builtin.
struct AstraExit {
    int code;
};

struct CallFrame {
    ObjClosure* closure;
    const uint8_t* ip;
    Value* base;  // slot 0: the receiver in methods, the callee itself otherwise
};

// Interned names the VM looks up by pointer.
struct Names {
    ObjString* init;
    ObjString* toString;
    ObjString* message;
    ObjString* length;
    ObjString* start;
    ObjString* end;
    ObjString* step;
    ObjString* name;
    ObjString* opAdd;
    ObjString* opSub;
    ObjString* opMul;
    ObjString* opDiv;
    ObjString* opMod;
    ObjString* opEq;
    ObjString* opLt;
    ObjString* opLe;
    ObjString* opGt;
    ObjString* opGe;
    ObjString* opNeg;
    ObjString* opIndex;
    ObjString* opIndexSet;
    ObjString* opShl;
    ObjString* opShr;
    ObjString* opBitAnd;
    ObjString* opBitOr;
    ObjString* opBitXor;
    ObjString* opBitNot;
};

enum class BuiltinType { String, List, Map, Range, Count_ };

class VM {
public:
    VM();
    ~VM();
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    // ---- Heap ----

    // Allocates a garbage-collected object. Collection only ever runs at safe points inside
    // the interpreter loop (calls and loop back-edges), never here, so a native function may
    // hold raw object pointers freely until it calls back into the VM.
    template <class T, class... A>
    T* alloc(A&&... args) {
        T* obj = new T(std::forward<A>(args)...);
        obj->next = objects_;
        objects_ = obj;
        bytesAllocated_ += sizeof(T);
        return obj;
    }

    // Tells the collector about memory owned by an object beyond its own size.
    void account(size_t bytes) { bytesAllocated_ += bytes; }

    ObjString* intern(std::string_view s);
    ObjString* newString(std::string s);
    Value str(std::string s) { return Value::object(newString(std::move(s))); }

    // ---- Running programs ----

    // Makes `program` current. May be called again after the program has grown (REPL).
    void load(Program& program);

    // Runs a module's top-level code and returns its result. Throws AstraThrow or AstraExit.
    Value runModule(int index);

    // Calls an Astra value from native code. Anything the caller holds that is not on the VM
    // stack must be protected with TempRoot / TempVector across this call.
    Value call(Value callee, int argc, const Value* args);

    // ---- Helpers for native code ----

    AstraThrow runtimeError(const std::string& message);
    std::string stringify(Value v);  // as print() shows it
    std::string repr(Value v);       // with strings quoted
    std::string typeName(Value v) const;

    int64_t toInt(Value v, const char* what);
    double toNumber(Value v, const char* what);
    const std::string& toStr(Value v, const char* what);

    void defineGlobal(const std::string& name, Value value) { builtins_[name] = value; }
    void defineNative(const std::string& name, int minArgs, int maxArgs, NativeFn fn);
    void defineMethod(BuiltinType type, const std::string& name, int minArgs, int maxArgs, NativeMethodFn fn);

    std::ostream* output;
    std::istream* input;
    std::vector<std::string> scriptArgs;
    std::mt19937_64 rng;
    Names names{};

    std::vector<Value> tempRoots;

    // Values that native code needs kept alive for the rest of the program.
    std::vector<Value> pinned;
    std::vector<const std::vector<Value>*> tempVectors;

private:
    struct Handler {
        int frameIndex;
        Value* stackTop;
        const uint8_t* ip;
    };

    static constexpr size_t kStackSize = 1 << 20;
    static constexpr int kMaxFrames = 20000;
    static constexpr int kMaxNativeDepth = 200;
    static constexpr size_t kMinHeap = 4 * 1024 * 1024;

    // Execution state
    Value* stack_;
    Value* stackEnd_;
    Value* sp_;
    std::vector<CallFrame> frames_;
    int frameCount_ = 0;
    std::vector<Handler> handlers_;
    ObjUpvalue* openUpvalues_ = nullptr;
    int nativeDepth_ = 0;
    std::vector<Value> namedScratch_;  // see callNamed

    // Program state
    Program* program_ = nullptr;
    std::vector<Value> globals_;

    // Declared type of each global variable; `type` 0 means untyped.
    struct TypeConstraint {
        uint8_t type = 0;
        ObjClass* cls = nullptr;  // for kTypeInstance
    };
    std::vector<TypeConstraint> globalTypes_;
    std::vector<bool> loaded_;
    std::unordered_map<std::string, Value> builtins_;
    std::unordered_map<const ObjString*, NativeMethod> methods_[static_cast<int>(BuiltinType::Count_)];
    ObjClass* errorClass_ = nullptr;
    ObjString* opNames_[static_cast<int>(Op::Count_)] = {};

    // Heap state
    Obj* objects_ = nullptr;
    size_t bytesAllocated_ = 0;
    size_t nextGC_ = kMinHeap;
    bool gcStress_ = false;  // ASTRA_GC_STRESS: collect at every safe point, to expose rooting bugs
    size_t liveBytes_ = 0;
    std::vector<Obj*> gray_;
    std::unordered_map<std::string_view, ObjString*> interned_;

    Value execute(const FunctionProto* proto);
    void run(int baseFrame);
    void resetState();

    bool callValue(Value callee, int argc);
    void callClosure(ObjClosure* closure, int argc);
    bool invoke(ObjString* name, int argc);
    bool callNamed(int positional, int named);
    void invokeOperator(Value receiver, ObjString* name, int argc);
    Value callNative(const std::string& name, int minArgs, int maxArgs, int argc);
    void checkArity(const std::string& name, int minArgs, int maxArgs, int argc);

    Value callWithReceiver(Value receiver, ObjClosure* method);
    void checkFieldAssignment(ObjInstance* instance, ObjString* name, Value value);
    void checkRequiredFields(ObjInstance* instance, const ObjClosure* init);
    Value getProperty(Value obj, ObjString* name);
    void setProperty(Value obj, ObjString* name, Value value);
    Value getIndex(Value obj, Value index);
    void setIndex(Value obj, Value index, Value value);
    bool binarySlow(Op op);
    bool compareSlow(Op op);
    bool shift(Op op);
    bool bitwise(Op op);

    bool matchesType(Value v, uint8_t type, const ObjClass* cls) const;
    ObjClass* popTypeClass(uint8_t type);
    std::string declaredTypeText(uint8_t type, const ObjClass* cls) const;
    AstraThrow typeMismatch(const std::string& variable, uint8_t type, const ObjClass* cls, Value v);
    const NativeMethod* findNativeMethod(Value receiver, const ObjString* name) const;
    Value makeIterator(Value iterable);
    bool iteratorNext(ObjIterator* it, Value& out);

    ObjUpvalue* captureUpvalue(Value* slot);
    void closeUpvalues(Value* last);

    bool tryHandle(Value exception, int baseFrame);
    void unwindTo(int baseFrame);
    std::vector<std::string> captureTrace() const;

    void stringifyInto(std::string& out, Value v, bool quote, int depth);

    void collectGarbage();
    void markValue(const Value& v) {
        if (v.kind == ValueKind::Obj) markObject(v.as.obj);
    }
    void markObject(Obj* obj);
    void markTable(const Table& table);
    void blacken(Obj* obj);
    void sweep();
};

// Keeps a value alive while native code calls back into the VM.
class TempRoot {
public:
    TempRoot(VM& vm, Value v) : vm_(vm), index_(vm.tempRoots.size()) { vm.tempRoots.push_back(v); }
    ~TempRoot() { vm_.tempRoots.resize(index_); }
    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;

    void set(Value v) { vm_.tempRoots[index_] = v; }
    Value get() const { return vm_.tempRoots[index_]; }

private:
    VM& vm_;
    size_t index_;
};

// Keeps every value in a native-side vector alive while native code calls back into the VM.
class TempVector {
public:
    TempVector(VM& vm, const std::vector<Value>& values) : vm_(vm) { vm.tempVectors.push_back(&values); }
    ~TempVector() { vm_.tempVectors.pop_back(); }
    TempVector(const TempVector&) = delete;
    TempVector& operator=(const TempVector&) = delete;

private:
    VM& vm_;
};

void registerBuiltins(VM& vm);

// The native half of the GUI library (lib/gui.astra). Defined in gui.cpp.
void registerGui(VM& vm);

}  // namespace astra
