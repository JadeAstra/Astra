#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
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
    // The call's result is not wanted: what was under it on the stack is the result instead.
    // This is how `apply` and `also` give back the value they were called on.
    bool dropResult;
};
// The type arguments a frame's generic function was called with (M6.6 part 1) are kept beside
// the frames, not in them, which stay small for the calls that have none: VM::frameTypeArgs.

// A catch block that is waiting for an error.
struct Handler {
    int frameIndex;
    Value* stackTop;
    const uint8_t* ip;
};

struct ObjFiber;

// What belongs to one line of execution: its value stack, the calls in progress and their
// catch blocks. Every thread has one, and so has every flow. The VM works on the one that is
// running through members of its own (sp_, frames_ and so on); the others keep their state
// here until it is their turn.
struct Context {
    Value* stack = nullptr;
    Value* stackEnd = nullptr;
    Value* sp = nullptr;
    CallFrame* frames = nullptr;
    int frameCount = 0;
    int maxFrames = 0;
    std::vector<Handler> handlers;
    ObjUpvalue* openUpvalues = nullptr;
    int nativeDepth = 0;
    Value* nativeBase = nullptr;
    Context* parent = nullptr;  // the one waiting for this one to finish or be set aside
    ObjFiber* fiber = nullptr;  // the flow this is the stack of; null for a thread's own

    Context(size_t stackSize, int frameLimit);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    void release();  // gives the memory back; the context is finished with
};

// A flow: a function running on a stack of its own, so that it can be set aside in the
// middle of a call and taken up again later (Kotlin's coroutine). See threads.cpp.
enum class FiberState : uint8_t { Fresh, Running, Suspended, Done };

struct ObjFiber final : Obj {
    static constexpr size_t kStackSize = 1 << 16;
    static constexpr int kMaxFrames = 4000;

    Context context;
    FiberState state = FiberState::Fresh;
    Value entry;                  // the function it runs
    Value* pendingSlot = nullptr; // where the call it is waiting in will leave its result
    int64_t waitToken = 0;        // the wait it is at, or was last at
    int64_t resumedToken = -1;    // the last wait an answer has been given for
    int64_t waitingAt = 0;        // the wait it is set aside at; 0 if it is set aside for something else
    bool hasEarly = false;        // an answer came before the flow got as far as waiting
    bool earlyOk = false;
    Value early;

    explicit ObjFiber(Value function) : Obj(ObjType::Fiber), context(kStackSize, kMaxFrames), entry(function) {
        context.fiber = this;
    }
};

inline ObjFiber* asFiber(const Value& v) { return static_cast<ObjFiber*>(v.as.obj); }

// Thrown by a built-in function to set the running flow aside. It passes through the
// interpreter loop untouched, leaving the flow's calls as they are, to where the flow was
// started or last taken up.
struct FiberSuspend {};

// Thrown at a safe point in a worker that has been told to stop. Like FiberSuspend it is not
// an Astra error: no catch block sees it, and it ends whatever the worker was doing.
struct WorkerStop {};

struct WorkerLink;    // worker.cpp: what a worker and the program that started it share
struct ParallelPool;  // parallel.cpp: the cores of a VM's parallel sections
struct ParallelJob;

// One operating-system thread that runs Astra code.
struct ThreadState {
    Context base;
    Context* current;  // the innermost context running on this thread
    int nesting = 0;   // how many flows are running inside one another on it

    // The thread's temporaries, while another thread has the lock (see VM::tempRoots).
    std::vector<Value> tempRoots;
    std::vector<const std::vector<Value>*> tempVectors;

    int64_t id = 0;
    std::string name;
    bool daemon = false;
    bool finished = false;
    Value function;  // what it runs
    Value self;      // the Thread object a program holds, if one was made for it
    std::atomic<bool> interrupted{false};
    std::vector<ThreadState*> joiners;

    // park() and unpark(): how a thread waits for something another thread will do.
    std::mutex parkMutex;
    std::condition_variable parkCv;
    bool parkToken = false;
    bool notified = false;  // set by notify() for a thread in wait()

    ThreadState(size_t stackSize, int frameLimit) : base(stackSize, frameLimit), current(&base) {}
};

// Interned names the VM looks up by pointer.
struct Names {
    ObjString* init;
    ObjString* toString;
    ObjString* message;
    ObjString* length;
    ObjString* size;
    ObjString* indices;
    ObjString* lastIndex;
    ObjString* keys;
    ObjString* itemType;
    ObjString* keyType;
    ObjString* valueType;
    ObjString* values;
    ObjString* key;
    ObjString* value;
    ObjString* code;
    ObjString* iterator;
    ObjString* first;
    ObjString* last;
    ObjString* absoluteValue;
    ObjString* sign;
    ObjString* start;
    ObjString* end;
    ObjString* step;
    ObjString* name;
    ObjString* className;
    ObjString* ordinal;
    ObjString* entries;
    ObjString* entryName;    // an entry's own name, kept apart from a `name` property that replaces it
    ObjString* entryOrdinal; // and its place in the list, likewise
    ObjString* enumEntries;  // where an enum's own list is kept, whatever `entries` gives
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

enum class BuiltinType { String, List, Map, Range, Bytes, Number, Count_ };

class VM {
public:
    // `worker`: this VM is a worker's, running beside the one that started it. It has no
    // windows: what belongs to the one user interface stays with the main program.
    explicit VM(bool worker = false);
    // The VM of one core of `root`'s parallel sections (parallel.cpp). It has an interpreter
    // of its own and works on root's objects.
    VM(VM& root, uint8_t coreId);
    ~VM();
    bool isWorker() const { return isWorker_; }
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    // ---- Heap ----

    // Allocates a garbage-collected object. Collection only ever runs at safe points inside
    // the interpreter loop (calls and loop back-edges), never here, so a native function may
    // hold raw object pointers freely until it calls back into the VM.
    template <class T, class... A>
    T* alloc(A&&... args) {
        T* obj = new T(std::forward<A>(args)...);
        obj->core = coreId_;  // what a core makes is its own until the section is over
        obj->next = objects_;
        objects_ = obj;
        bytesAllocated_ += sizeof(T);
        return obj;
    }

    // An object of `cls`, with room for the fields the class declares, none of them given a
    // value yet (M6.5 part 4).
    ObjInstance* newInstance(ObjClass* cls, uint16_t captures = 0, bool typed = false) {
        const uint32_t count = static_cast<uint32_t>(cls->declared.size());
        const uint32_t slots = count + captures + (typed ? 1u : 0u);
        ObjInstance* obj = new (slots) ObjInstance(cls, count, captures, typed);
        obj->core = coreId_;
        obj->next = objects_;
        objects_ = obj;
        bytesAllocated_ += sizeof(ObjInstance) + slots * sizeof(Value);
        return obj;
    }
    // Makes an object of `cls` in the slot under its `argc` arguments and starts its
    // initializer; with `captures`, an anonymous object that keeps them (Op::NewObject).
    // Returns whether a frame was pushed.
    bool construct(ObjClass* cls, int argc, bool packed, const Value* captures = nullptr, uint16_t captureCount = 0);

    // ---- Lists (M6.6 part 3): see "Lists" in vm.cpp ----
    ListStore storeFor(const ObjTypeInfo* item, uint32_t& width, ObjClass*& itemClass);
    void typeList(ObjList* list, ObjTypeInfo* type);
    uint8_t* itemAt(ObjList* list, size_t i);
    // Item `i` of a list, `i` already checked against its size: a value type's item copied into a box.
    Value listGet(ObjList* list, size_t i) { return !list->packed() ? list->values()[i] : packedGet(list, i); }
    Value packedGet(ObjList* list, size_t i);
    void checkItem(const ObjList* list, Value v);
    void writeItem(ObjList* list, size_t i, Value v);
    void listSet(ObjList* list, size_t i, Value v);
    void listInsert(ObjList* list, size_t at, const Value* items, size_t n);
    void listAdd(ObjList* list, Value v);
    void listErase(ObjList* list, size_t at, size_t n);
    void listClear(ObjList* list);
    std::vector<Value> listCopy(ObjList* list);
    ObjList* listLike(const ObjList* like);
    void listCopyRange(ObjList* to, const ObjList* from, size_t start, size_t count);
    void listSwap(ObjList* list, size_t i, size_t j);
    void markItems(ObjList* list);
    bool addNew(int argc);
    void makeFromHeader(ObjClass* cls, int argc, Value dest);
    void listArrange(ObjList* list, const std::vector<size_t>& order);

    // ---- References (M6.6 part 4) ----
    // A value as it is kept: a Ref replaced by a box with a copy, anything else as it is. What
    // is stored anywhere but a thread's stack goes through this.
    Value own(Value v) { return v.kind == ValueKind::Ref ? ownRef(v) : v; }
    Value ownRef(Value v);
    void ownAll(Value* from, Value* to) {
        for (Value* at = from; at < to; at++)
            if (at->kind == ValueKind::Ref) *at = ownRef(*at);
    }
    // Where what a Ref points at is, checked: an error for an item no longer in its list, and
    // for `write`, for a value that may not be changed there.
    uint8_t* refAt(Value r, bool write);
    // The bytes of a Ref of depth 0 where this core may read them (`write`: and change them);
    // null where it may not, or the item is gone, which the slow way says (M6.6 part 4).
    uint8_t* refQuickly(Value r, bool write) const {
        if (r.as.obj == bandObject_) {
            if (r.refIndex < bandFrom_ || r.refIndex >= bandTo_) return nullptr;
        } else if (write && (r.as.obj->type == ObjType::List ? r.as.obj->core != coreId_ : !static_cast<const ObjValue*>(r.as.obj)->constructing)) {
            return nullptr;
        }
        return refBytes(r);
    }
    // Field `name` of what a Ref points at: in place, a value held in it a Ref of its own.
    bool refField(Value r, const ObjString* name, Value& out);
    void setRefField(Value r, ObjString* name, Value value);
    // The place a variable, field or item holding a value is: its value given a box of its own,
    // writable, in `slot`, and a Ref to it (M6.6 part 4).
    Value placeIn(Value& slot);

    // ---- Value types (M6.6 part 2) ----
    ObjClass* fieldClass(FieldDecl& field);
    const ValueLayout& layoutOf(ObjClass* cls);
    ObjValue* newValue(ObjClass* cls);
    Value readValueField(const uint8_t* at, const ValueField& field);
    // An int, float or bool field read with no call, for the interpreter's quick paths.
    Value readPlainField(const uint8_t* at, const ValueField& field) {
        switch (field.kind) {
            case ValueField::Float: {
                double d;
                std::memcpy(&d, at, sizeof d);
                return Value::number(d);
            }
            case ValueField::Int: {
                int64_t i;
                std::memcpy(&i, at, sizeof i);
                return Value::integer(i);
            }
            case ValueField::Bool: return Value::boolean(*at != 0);
            default: return readValueField(at, field);
        }
    }
    void writeValueField(uint8_t* at, const ValueField& field, const ObjClass* owner, Value value);
    int64_t valueFieldIndex(const ObjValue* box, const ObjString* name) const;
    Value valueField(const ObjValue* box, size_t index);
    void setValueField(ObjValue* box, ObjString* name, Value value);
    bool valuesSame(const ObjValue* a, const ObjValue* b, int depth);
    const uint8_t* readThrough(const uint8_t* at, const ValueField* field, const uint8_t* next, const Value* consts, Value& out);
    // Capture `k` of the object a member of an anonymous class runs on, checked.
    ObjUpvalue* capturedOf(Value receiver, int k);
    ObjUpvalue* receiverUpvalue(const CallFrame* frame, const Value* base, uint16_t entry) const;

    // Tells the collector about memory owned by an object beyond its own size.
    void account(size_t bytes) { bytesAllocated_ += bytes; }

    ObjString* intern(std::string_view s);

    // ---- Types when the program runs (M6.6 part 1) ----
    // The one object for a type: `id` a TypeId (0 any), for a class `cls`, with its arguments.
    ObjTypeInfo* typeOf(uint8_t id, bool nullable, ObjClass* cls, const std::vector<ObjTypeInfo*>& args);
    // A type as the program writes it: `List<Point?>`.
    std::string typeInfoText(const ObjTypeInfo* type) const;
    // Whether every value of `a` is one of `b`. A list or map read through a type is read as
    // any type its items fit (a List<Circle> is a List<Shape>), as Kotlin's read-only ones are.
    bool isSubtype(const ObjTypeInfo* a, const ObjTypeInfo* b);
    // Whether a value is of a type, its type arguments included.
    bool fitsType(const Value& v, const ObjTypeInfo* type);
    // An object of `cls` whose own type is `own` (null where it was made without one), seen as
    // one of `target`, a class it extends or an interface it implements: `Base<List<int>>` for a
    // `Pair<int>` of `class Pair<A> : Base<List<A>>()`. Null where that has no type arguments.
    const ObjTypeInfo* typeAs(ObjClass* cls, const ObjTypeInfo* own, const ObjClass* target);
    // The supertypes of `cls` written with type arguments, made for `own` (ObjClass::supertypeMakers).
    const std::vector<ObjTypeInfo*>& supertypesOf(ObjClass* cls, const ObjTypeInfo* own);
    // What type a value is, as the program writes it: `List<string>` for a list made to hold strings.
    std::string valueTypeText(const Value& v) const;
    // Runs one of the instructions of types (MakeType to InstanceTypeArg); gives where the next one is.
    const uint8_t* typeInstruction(Op op, const uint8_t* ip, CallFrame* frame);
    const uint8_t* placeInstruction(Op op, const uint8_t* ip, CallFrame* frame);
    Value returnedRef(Value result, ptrdiff_t frameIndex);
    // The type a MakeType shape describes, from `at`, its classes and types taken from `operand` on.
    ObjTypeInfo* typeFromShape(const std::string& shape, size_t& at, const Value*& operand);
    // The type `any`, made once.
    ObjTypeInfo* anyType() {
        if (anyType_ == nullptr) anyType_ = typeOf(0, false, nullptr, {});
        return anyType_;
    }
    // The type arguments of frame `index`, a kTypeArguments type, null where there are none: a
    // generic function's are those WithTypes gave its call, and a lambda's those of where it was
    // made. Kept only for a function whose FunctionProto::typeArgs says it reads them. They are
    // in the block of the frames, after them (Context).
    const ObjTypeInfo*& frameTypeArgs(ptrdiff_t index) {
        return reinterpret_cast<const ObjTypeInfo**>(frames_ + maxFrames_)[index];
    }
    // Where frame `index`'s result goes (M6.6 part 4): a place, or null; kept only for a function
    // whose FunctionProto::typeArgs has kTakesDestination. In the block of the frames, after the
    // type arguments.
    Value& frameDestination(ptrdiff_t index) {
        return reinterpret_cast<Value*>(reinterpret_cast<const ObjTypeInfo**>(frames_ + maxFrames_) + maxFrames_)[index];
    }
    Value pendingDestination_;  // given by WithDestination to the call that follows
    // Type argument `i` of what is in `args` (a frame's, or an object's type), or `any` where
    // that is not known.
    ObjTypeInfo* typeArgument(const ObjTypeInfo* args, int i) {
        return args != nullptr && static_cast<size_t>(i) < args->args.size() ? args->args[static_cast<size_t>(i)] : anyType();
    }
    ObjString* newString(std::string s);
    Value str(std::string s) { return Value::object(newString(std::move(s))); }

    // ---- Running programs ----

    // Makes `program` current. May be called again after the program has grown (shell).
    void load(Program& program);

    // Runs a module's top-level code and returns its result. Throws AstraThrow or AstraExit.
    Value runModule(int index);

    // Calls an Astra value from native code. Anything the caller holds that is not on the VM
    // stack must be protected with TempRoot / TempVector across this call.
    Value call(Value callee, int argc, const Value* args);

    // The == operator: by value, and for lists, maps and ranges by what they hold.
    bool equals(Value a, Value b, int depth = 0);
    static constexpr int kMaxEqualsDepth = 200;

    // Runs a function with `receiver` as the object it runs on: inside it, a name that is not
    // one of its variables is looked up on that object first (see Op::GetImplicit).
    Value callOn(Value receiver, Value callee, int argc, const Value* args);

    // ---- Helpers for native code ----

    AstraThrow runtimeError(const std::string& message);
    // An error of a class the standard library declares (IOException, say), by its name.
    AstraThrow errorOf(const char* className, const std::string& message);
    // The globals the VM itself finds by name: kept in a built program whether or not its code
    // names them (M6.5 part 5). errorOf takes no other name.
    static const std::vector<std::string>& globalsFoundByName();
    // The member names the VM and the built-in functions look up themselves (the names table,
    // and a few more): an extension function of one of them is kept in a built program.
    std::vector<std::string> membersFoundByName() const;
    AstraThrow outOfMemory();
    void checkRoomFor(uint64_t bytes);
    std::string stringify(Value v);  // as print() shows it
    std::string repr(Value v);       // with strings quoted
    std::string typeName(Value v) const;

    int64_t toInt(Value v, const char* what);
    double toNumber(Value v, const char* what);
    const std::string& toStr(Value v, const char* what);

    void defineGlobal(const std::string& name, Value value) { builtins_[name] = value; }
    bool hasBuiltin(const std::string& name) const { return builtins_.count(name) != 0; }
    // What the C++ provides, for `external` declarations to be checked against: whether a global
    // function, or a built-in type's method, exists, and how many arguments it takes (-1: any).
    bool builtinArity(const std::string& name, int& minArgs, int& maxArgs) const;
    // A static of a class the C++ provides (`Math.sqrt`): its own static, or null.
    ObjNative* builtinStatic(const std::string& cls, const std::string& name);
    bool methodArity(BuiltinType type, const std::string& name, int& minArgs, int& maxArgs);
    void defineNative(const std::string& name, int minArgs, int maxArgs, NativeFn fn);
    void defineMethod(BuiltinType type, const std::string& name, int minArgs, int maxArgs, NativeMethodFn fn);
    // Says what the parameters of a built-in method are called, so that it takes them by name.
    // What a built-in method's or function's parameters are called, from its declaration in
    // the standard library (D116). A name with several forms keeps the longest list.
    void nameParams(BuiltinType type, const std::string& method, const std::vector<std::string>& params);
    void nameFunctionParams(const std::string& function, const std::vector<std::string>& params);
    static bool builtinTypeNamed(const std::string& name, BuiltinType& type);
    // Everything a list, map, string, range, bytes or object with an iterator() holds, in order.
    void collect(Value iterable, std::vector<Value>& out);

    std::ostream* output;
    std::istream* input;
    std::vector<std::string> scriptArgs;
    std::mt19937_64 rng;
    Names names{};

    // The temporaries of the thread that is running (see TempRoot). Each thread has its own;
    // they change places with these when the lock changes hands.
    std::vector<Value> tempRoots;

    // Values that native code needs kept alive for the rest of the program.
    std::vector<Value> pinned;
    std::vector<const std::vector<Value>*> tempVectors;

    // ---- Threads, the event loop and flows (threads.cpp) ----
    //
    // Threads share one heap, and one of them runs Astra code at a time: whichever holds the
    // lock. A thread gives the lock up at a safe point when another has waited long enough,
    // and around anything that blocks.

    // Starts a thread that calls `function`. Returns its id.
    int64_t startThread(Value function, Value self, const std::string& name, bool daemon);
    ThreadState* currentThread() const { return thread_; }
    ThreadState* findThread(int64_t id) const;
    bool onLoopThread() const { return thread_ == &mainThread_; }
    // Waits for a thread to finish; false if `timeoutMs` (negative: no limit) ran out first.
    bool joinThread(int64_t id, int64_t timeoutMs);
    void interruptThread(int64_t id);

    // Gives up the lock, with this thread's state put away, and takes it back. Between the
    // two the thread must not touch the VM or any object.
    void releaseLock();
    void acquireLock(ThreadState* thread);
    // Blocks the calling thread until unpark(), an interrupt or the time limit (negative:
    // none), with the lock released meanwhile. Wake-ups can come early: callers re-check.
    void park(int64_t timeoutMs);
    void unpark(ThreadState* thread);

    // An object used as a lock (`synchronized`), with wait and notify as Java has them.
    void monitorEnter(Value lock);
    void monitorExit(Value lock);
    bool monitorTryEnter(Value lock);
    int monitorWait(Value lock, int64_t timeoutMs);  // 0 notified, 1 timed out, 2 interrupted
    void monitorNotify(Value lock, bool all);
    bool monitorHeld(Value lock) const;

    // The event loop: tasks posted from any thread and timers, run on the main thread.
    void post(Value task);
    int64_t addTimer(Value task, int64_t delayMs, int64_t periodMs);
    void cancelTimer(int64_t id);
    // Something to do on the loop thread, handed over by a thread that does not hold the
    // lock (a worker finishing a read, say). `pendingJobs` counts work that will post one.
    void postNative(std::function<void(VM&)> work);
    void jobStarted() { pendingJobs_++; }
    void jobFinished() { pendingJobs_--; }
    // Runs what is ready. With `wait`, goes on until there is nothing left to wait for: no
    // task, no timer, no work in progress and no thread that is not a daemon.
    // Without `wait` it returns when nothing is ready, or, given a budget in milliseconds,
    // once that long has passed and at least one thing has been run.
    void runEventLoop(bool wait, int64_t budgetMs = -1);
    // True if threads are still running, in which case the process has to end without
    // taking the VM apart under them.
    bool threadsAlive() const { return threads_.size() > 1 || pendingJobs_ > 0 || liveWorkers_ > 0; }
    // How long until the next timer is due, in milliseconds: 0 if something is ready now,
    // -1 if there is nothing to wait for. For a loop of another kind (a window's) that
    // has to wake up in time to call runEventLoop(false).
    int64_t millisUntilLoopWork();
    // Called, from whatever thread, when the loop has been given something to do.
    std::atomic<void (*)()> onLoopWake{nullptr};
    // For a program with windows, whose events come through the same loop. `loopPump`
    // handles the window system's events that are waiting, calling into the program for
    // each. `loopWait` waits, with the lock released, for one to arrive or for onLoopWake,
    // for at most that many milliseconds (-1: for as long as it takes).
    void (*loopPump)(VM&) = nullptr;
    void (*loopWait)(int64_t) = nullptr;

    // ---- Parallel sections (parallel.cpp) ----
    // Several cores running Astra code on the same objects at the same time. Each core has a
    // VM of its own for the interpreter's working state, and they share the heap of the VM
    // that started the section, which waits meanwhile with its lock held. What keeps them
    // apart is ownership (Obj::core): a core looks only into what is shared or its own, and
    // changes only what is its own. Every place that looks into an object asks.

    bool inParallelSection() const { return coreId_ != 0; }
    // Throws if this core may not look into, or may not change, the object.
    void guardRead(const Obj* obj) const {
        if (obj->core != coreId_ && obj->core != 0) [[unlikely]] throw otherCores(obj);
    }
    void guardRead(Value v) const {
        if (v.kind == ValueKind::Obj) guardRead(v.as.obj);
    }
    void guardWrite(const Obj* obj) const {
        if (obj->core != coreId_) [[unlikely]] throw notOwned(obj);
    }
    // Says which of a type's built-in methods change the value they are called on, and which
    // built-in functions are not for a parallel section (by the object they belong to).
    void markMutating(BuiltinType type, std::initializer_list<const char*> methods);
    void markNotParallel(const std::string& global, std::initializer_list<const char*> except = {});

    // Runs `function` on every item of `source` (a list, a range, anything a loop takes), the
    // items dealt out to the cores. With `keep`, gives the list of what it returned, in order.
    // With `replace`, each item of the list `source` is replaced by what the function gives for it.
    Value runParallel(Value source, Value function, bool keep, bool replace = false);
    // Runs `function` on each of `parts` ranges that together cover a `bytes`, one core to
    // a range, each allowed to write that part of it.
    void runParallelChunks(Value buffer, Value function);
    int parallelCores();
    // One core's share of a section. Called on that core's thread, in its own VM.
    void runParallelShare(ParallelJob& job, size_t share);

    // ---- Workers (worker.cpp) ----
    // A worker is a second VM on a thread of its own, with a heap and a lock of its own, so
    // it runs at the same time as this one, on another core. The two share nothing and
    // exchange messages, which are copied.

    // The program as bytes, for a worker to load its own copy from. Set by whoever loaded it.
    std::function<std::string()> programImage;
    // In a worker: the link to the program that started it.
    std::shared_ptr<WorkerLink> workerLink;
    // In a worker: the object its messages are delivered to (Worker.parent).
    Value workerSelf;
    // The Worker objects of the workers this VM has started, by id.
    std::unordered_map<int64_t, Value> workerObjects;
    std::unordered_map<int64_t, std::shared_ptr<WorkerLink>> workerLinks;
    int64_t nextWorkerId = 1;
    void workerStarted() { liveWorkers_++; }
    void workerEnded() { liveWorkers_--; }
    // Asks this VM, from another thread, to stop what it is doing: every one of its threads
    // gets a WorkerStop at its next safe point.
    void requestStop();

    // For a built-in function that ends by calling an Astra function: sets that call going
    // in its own place, so that the function runs as part of the program rather than inside
    // the built-in (where it could not be set aside). The built-in returns what this returns.
    // `receiver` becomes the object the function runs on.
    Value tailCallOn(Value receiver, Value function);

    // Flows.
    ObjFiber* currentFiber() const { return context_->fiber; }
    // Starts or takes up a flow, giving it `value` as the result of what it was waiting for
    // (or, with `ok` false, raising it there as an error). Returns when the flow finishes or
    // is set aside again; an error the flow does not catch comes out of this call.
    void runFiber(ObjFiber* fiber, bool ok, Value value);
    // Whether a built-in function called now could set the running flow aside.
    bool canSuspend() const { return context_->fiber != nullptr && nativeDepth_ == 0 && nativeBase_ != nullptr; }
    // Sets the running flow aside: does not return. The flow goes on when runFiber is called.
    [[noreturn]] void suspendFiber();

    // Runs `work`, which blocks and must not touch the VM, and gives what `finish` makes of
    // it. In a flow on the loop thread the flow is set aside while a worker thread does the
    // work, and the loop stays live; anywhere else the calling thread waits, with the lock
    // released. A built-in function has to return what this returns straight away.
    Value blocking(std::function<void()> work, std::function<Value(VM&)> finish);

    // Called for an error that nothing caught on a thread other than the main one.
    std::function<void(VM&, AstraThrow&, const std::string& threadName)> onThreadError;
    // Called when exit() is used on such a thread.
    std::function<void(int code)> onThreadExit;

private:
    static constexpr size_t kStackSize = 1 << 20;
    static constexpr int kMaxFrames = 20000;
    static constexpr size_t kThreadStackSize = 1 << 18;
    static constexpr int kThreadMaxFrames = 8000;
    static constexpr int kMaxNativeDepth = 200;
    static constexpr int kMaxFlowNesting = 60;
    static constexpr size_t kMinHeap = 4 * 1024 * 1024;

    // Execution state: that of the context that is running (see Context).
    Value* stack_;
    Value* stackEnd_;
    Value* sp_;
    CallFrame* frames_;
    int frameCount_ = 0;
    int maxFrames_ = 0;
    std::vector<Handler> handlers_;
    ObjUpvalue* openUpvalues_ = nullptr;
    int nativeDepth_ = 0;
    Value* nativeBase_ = nullptr;  // under the arguments of the built-in function being called
    Context* context_ = nullptr;
    std::vector<Value> namedScratch_;  // see callNamed

    void saveContext();
    void loadContext(Context* context);
    void markContext(const Context& context);

    // Threads
    ThreadState mainThread_{kStackSize, kMaxFrames};
    ThreadState* thread_ = &mainThread_;  // the one holding the lock
    std::vector<ThreadState*> threads_;   // all that are running, the main one first
    int64_t nextThreadId_ = 1;
    std::mutex lockMutex_;
    std::condition_variable lockFree_;
    std::condition_variable lockTaken_;
    bool lockHeld_ = true;
    uint64_t lockSwitches_ = 0;
    std::atomic<int> lockWaiters_{0};
    std::atomic<bool> attention_{false};  // a safe point has something to do besides collecting
    bool threadStress_ = false;           // ASTRA_THREAD_STRESS: change threads at every safe point
    void safePoint();
    void yieldLock();
    void threadMain(ThreadState* thread);

    struct Monitor {
        ThreadState* owner = nullptr;
        int count = 0;
        std::vector<ThreadState*> entering;
        std::vector<ThreadState*> waiting;
    };
    std::unordered_map<const Obj*, Monitor> monitors_;
    Monitor& heldMonitor(Value lock, const char* what);

    // The event loop
    struct Timer {
        int64_t id;
        std::chrono::steady_clock::time_point due;
        int64_t periodMs;
        Value task;
    };
    std::deque<Value> loopTasks_;
    std::vector<Timer> timers_;
    int64_t nextTimerId_ = 1;
    int pendingJobs_ = 0;
    std::mutex loopMutex_;
    std::condition_variable loopCv_;
    bool loopWake_ = false;
    std::deque<std::function<void(VM&)>> nativeTasks_;
    void wakeLoop();
    std::vector<ObjFiber*> waitingFibers_;  // flows set aside until work in progress is done
    bool isWorker_ = false;
    int liveWorkers_ = 0;

    // Parallel sections
    VM* root_ = nullptr;           // for a core's VM: the VM whose objects it works on
    uint8_t coreId_ = 0;           // which core this VM is; 0 for a VM that is not a core's
    std::shared_ptr<ParallelPool> parallelPool_;
    std::mutex internMutex_;       // cores intern strings in the root's table
    uint64_t extensionEpoch_ = 0;  // goes up when an extension function is added
    // In a section over the parts of a buffer, or over a list of values (M6.6 part 4): the bytes
    // or the list this core may write the part of, and that part. Of such a list it reads only
    // its own part too, which no other core is writing.
    const Obj* bandObject_ = nullptr;
    size_t bandFrom_ = 0, bandTo_ = 0;
    AstraThrow outsideBand(size_t i) const;
    AstraThrow wholeBand() const;
    // A built-in function or method given the list of values a section is over, which would
    // read every core's items: refused, but for one that reads none of them.
    void guardBand(Value v, const std::string& method = "") const {
        if (v.isObj() && v.as.obj == bandObject_ && bandObject_->type == ObjType::List && method != "isEmpty" && method != "isNotEmpty")
            [[unlikely]] throw wholeBand();
    }
    void refreshFromRoot();
    void adoptObjects(VM& core);
    AstraThrow otherCores(const Obj* obj) const;
    AstraThrow notOwned(const Obj* obj) const;
    AstraThrow notOwned(const std::string& what) const;
    void guardNative(const Value* args, int argc) const {
        for (int i = 0; i < argc; i++) {
            guardRead(args[i]);
            guardBand(args[i]);
        }
    }
    friend struct ParallelPool;
    std::atomic<bool> stopRequested_{false};

    // Worker threads for work that blocks (see blocking()).
    struct Workers;
    std::shared_ptr<Workers> workers_;

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
    ObjClass* mapEntryClass_ = nullptr;  // MapEntry, from the standard library: what a map's entries are
    ObjString* opNames_[static_cast<int>(Op::Count_)] = {};

    // Heap state
    Obj* objects_ = nullptr;
    size_t bytesAllocated_ = 0;
    size_t nextGC_ = kMinHeap;
    bool gcStress_ = false;  // ASTRA_GC_STRESS: collect at every safe point, to expose rooting bugs
    size_t liveBytes_ = 0;
    std::vector<Obj*> gray_;
    std::unordered_map<std::string_view, ObjString*> interned_;
    std::unordered_map<std::string, ObjTypeInfo*> types_;  // by what they are made of (typeOf)
    ObjTypeInfo* anyType_ = nullptr;
    // The type arguments WithTypes gave for the call that follows it, which takes them: the
    // frame it pushes, or the object it makes. Cleared by a call of C++, which has none.
    const ObjTypeInfo* pendingTypeArgs_ = nullptr;

    Value execute(const FunctionProto* proto);
    void run(int baseFrame);
    void resetState();

    // `packed`: the arguments are already arranged one per parameter (see callNamed).
    bool callValue(Value callee, int argc, bool packed = false);
    void callClosure(ObjClosure* closure, int argc, bool packed = false);
    ObjClosure* chooseForm(const ObjForms* set, const Value* args, int positional, const Value* pairs, int named);
    bool formTakes(const FunctionProto* proto, const Value* args, int positional, const Value* pairs, int named,
                   int& score, int& defaultsLeft) const;
    int paramFit(Value v, uint8_t type, const ObjString* className) const;
    ObjString* emptyName_ = nullptr;  // the name a lambda written after a call travels under
    void callTrailing(ObjClosure* closure, int argc);
    bool callValueTrailing(Value callee, int argc);
    int packVarargs(const FunctionProto* proto, int argc);

    // Bare names passed as arguments that may be enum entries (ValueKind::EnumName), not yet
    // settled. While there are none, calls need not look for them.
    int pendingEnums_ = 0;
    AstraThrow unsettledName(Value pending);
    void rejectPendingNames(const Value* args, int argc);
    Value settleEnumName(Value pending, const ObjClass* type);
    bool callSpread(int named);
    bool invoke(ObjString* name, int argc, bool trailing = false);
    bool callNativeNamed(int positional, int named);
    int scopeFunction(ObjString* name, int argc);
    bool callNamed(int positional, int named);
    void invokeOperator(Value receiver, ObjString* name, int argc);
    Value callNative(const std::string& name, int minArgs, int maxArgs, int argc);
    void checkArity(const std::string& name, int minArgs, int maxArgs, int argc);

    Value callWithReceiver(Value receiver, ObjClosure* method);
    Value implicitReceiver(const CallFrame* frame, const Value* base, const ObjString* name) const;
    void setAccess(ObjClass* cls, Table& marks, ObjString* name, bool write, const std::vector<uint16_t>& run, int kind);
    void declareAccess(ObjClass* cls, Table& marks, ObjString* name, int id, int kind, bool write);
    void joinProtected(ObjClass* subclass);
    bool accessDenied(Value target, const ObjString* name, const FunctionProto* proto, bool write) const;
    AstraThrow accessError(Value target, const ObjString* name, const FunctionProto* proto);
    AstraThrow undefinedName(const CallFrame* frame, const Value* base, const ObjString* name, size_t global);
    void ensureClassInit(ObjClass* cls);

public:
    // One entry of a map, as `map.entries` gives it: an object with a key and a value.
    Value makeEntry(Value key, Value value);
    Value mapEntries(ObjMap* map);
    // The type of a list of a map's keys (0), values (1) or entries (2), from the map's own; null where it has none.
    ObjTypeInfo* mapPartType(const ObjMap* map, int part);
    Value mapKeys(ObjMap* map);
    Value mapValues(ObjMap* map);

private:
    const Value* superMethod(Value super, Value self, const ObjString* name) const;

    // Extension functions on the built-in types, by TypeId; on null; and on every value.
    Table typeExtensions_[16];
    Table nullExtensions_;
    Table nullExtensionOwners_;  // for each of those, the type it was declared on
    Table anyExtensions_;
    bool hasExtensions_ = false;
    void addExtensionForm(Table& table, ObjString* name, Value function);
    std::unordered_map<std::string, std::unique_ptr<FunctionProto>> extensionSets_;  // the protos of their sets, by name
    const Value* findExtension(Value receiver, const ObjString* name) const;
    bool hasMember(Value candidate, const ObjString* name) const;
    void declareEntryFields(ObjClass* cls);
    void nameEntry(ObjInstance* entry, ObjString* name, int ordinal);
    void checkFieldAssignment(ObjInstance* instance, ObjString* name, Value value);
    void checkStaticType(ObjClass* owner, FieldDecl& field, Value value);
    void assignStatic(ObjClass* cls, ObjString* name, Value value);
    void checkRequiredFields(ObjInstance* instance, const ObjClosure* init);
    Value getProperty(Value obj, ObjString* name);
    void setProperty(Value obj, ObjString* name, Value value);
    Value getIndex(Value obj, Value index);
    void setIndex(Value obj, Value index, Value value);
    bool binarySlow(Op op);
    bool compareSlow(Op op);
    bool shift(Op op);
    bool bitwise(Op op);

    bool matchesType(const Value& v, uint8_t type, const ObjClass* cls) const;
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
        if (v.kind == ValueKind::Obj || v.kind == ValueKind::Ref) markObject(v.as.obj);
    }
    void markObject(Obj* obj);
    void markTable(const Table& table);
    void blacken(Obj* obj);
    void sweep();
};

// For a built-in function that is about to block: gives up the lock for as long as this
// object lives. Nothing of the VM, and no object, may be touched meanwhile.
class LockReleased {
public:
    explicit LockReleased(VM& vm) : vm_(vm), thread_(vm.currentThread()) { vm_.releaseLock(); }
    ~LockReleased() { vm_.acquireLock(thread_); }
    LockReleased(const LockReleased&) = delete;
    LockReleased& operator=(const LockReleased&) = delete;

private:
    VM& vm_;
    ThreadState* thread_;
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
void registerThreads(VM& vm);  // threads.cpp: threads, locks, the event loop and flows
void registerIo(VM& vm);       // io.cpp: streams, processes and sockets
void registerWorkers(VM& vm);  // worker.cpp: isolated workers
void registerParallel(VM& vm); // parallel.cpp: parallel sections
void registerText(VM& vm);     // text/api.cpp: fonts, text and pictures
// What started this program (see __io.selfCommand): set by main before the program runs.
void setSelfCommand(std::vector<std::string> command);

// The native layer of the Jade library (lib/Jade): windows, events and drawing. jade/api.cpp.
void registerJade(VM& vm);

}  // namespace astra
