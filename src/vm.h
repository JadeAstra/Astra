#pragma once

#include <algorithm>
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
#include <unordered_set>
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
    // How many times the function has started again, calling itself as its last step (D29).
    uint32_t repeats;
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
// catch blocks. Every thread has one, and so has every task. The VM works on the one that is
// running through members of its own (sp_, frames_ and so on); the others keep their state
// here until it is their turn.
struct Context {
    Value* stack = nullptr;
    Value* stackEnd = nullptr;
    Value* sp = nullptr;
    CallFrame* frames = nullptr;
    int frameCount = 0;
    int maxFrames = 0;   // the frames laid out, with their type arguments and destinations after them
    int frameLimit = 0;  // how many may be used now: all of them, or of a stack that grows, those committed
    // A stack that grows (a generator's, D98): its memory reserved whole and committed as it is
    // used, so that it starts small and moves nowhere; how far it may go.
    bool growable = false;
    Value* stackReserveEnd = nullptr;
    std::vector<Handler> handlers;
    ObjUpvalue* openUpvalues = nullptr;
    int nativeDepth = 0;
    Value* nativeBase = nullptr;
    // The type arguments a call about to be made was given (Op::WithTypes): a thread may give
    // way between them and the call, so they are its own.
    const ObjTypeInfo* pendingTypeArgs = nullptr;
    Context* parent = nullptr;  // the one waiting for this one to finish or be set aside
    ObjFiber* fiber = nullptr;  // the task this is the stack of; null for a thread's own

    Context(size_t stackSize, int frameLimit, bool grows = false);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    void release();  // gives the memory back; the context is finished with
    // Of a stack that grows: commits enough for `top` and `frames`; false where it has no more.
    bool grow(const Value* top, int frames);
};

// A task: a function running on a stack of its own, so that it can be set aside in the
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
    bool generator = false;       // a sequence's generator (D98): set aside only where it yields
    bool hasEarly = false;        // an answer came before the task got as far as waiting
    bool earlyOk = false;
    Value early;

    // A generator's (D98) has a stack that starts small and grows as needed, so that many can
    // be alive at once.
    explicit ObjFiber(Value function, bool small = false) : Obj(ObjType::Fiber), context(kStackSize, kMaxFrames, small), entry(function) {
        context.fiber = this;
    }
};

inline ObjFiber* asFiber(const Value& v) { return static_cast<ObjFiber*>(v.as.obj); }

// Thrown by a built-in function to set the running task aside. It passes through the
// interpreter loop untouched, leaving the task's calls as they are, to where the task was
// started or last taken up.
struct FiberSuspend {};

// Thrown at a safe point in a worker that has been told to stop. Like FiberSuspend it is not
// an Astra error: no catch block sees it, and it ends whatever the worker was doing.
struct WorkerStop {};

struct WorkerLink;    // worker.cpp: what a worker and the program that started it share
struct ParallelPool;  // parallel.cpp: the cores of a VM's parallel sections
struct ParallelSection;
struct ParallelPiece;
struct SharedBlock;   // worker.cpp: frozen data shared with workers, outside every heap (D97)

// One operating-system thread that runs Astra code.
struct ThreadState {
    Context base;
    Context* current;  // the innermost context running on this thread
    int nesting = 0;   // how many tasks are running inside one another on it

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
    // The locks it holds, the one taken last at the back: what waitUntil gives back (D86).
    std::vector<const Obj*> held;

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
    ObjString* cause;
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
    ObjString* character;  // `n.char`
    ObjString* fromEnd;    // an Index's
    ObjString* iterator;
    ObjString* first;
    ObjString* last;
    ObjString* absoluteValue;
    ObjString* sign;
    ObjString* start;
    ObjString* end;
    ObjString* step;
    ObjString* name;
    ObjString* ordinal;
    ObjString* compareTo;
    ObjString* hasNext;
    ObjString* next;
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
    ObjString* hashCode;
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

enum class BuiltinType { String, List, Map, Range, Bytes, Number, Char, Byte, Index, Set, Count_ };

// The classes of what the VM throws (D35), Java's, declared in src/std/errors.astra; and
// Throwable, the root, which is all that `throw` takes.
enum class Thrown : uint8_t {
    RuntimeException, ArithmeticException, IndexOutOfBoundsException, NullPointerException, ClassCastException,
    IllegalArgumentException, NumberFormatException, IllegalStateException, UnsupportedOperationException,
    NoSuchElementException, ConcurrentModificationException, AssertionError, StackOverflowError, OutOfMemoryError,
    InternalError, IOException, Throwable, SocketTimeoutException, Count_
};

class VM : public KeyMethods {
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
#ifdef ASTRA_OP_COUNTS
    // In the build that counts instructions: every object made, shown when the program ends.
    static std::atomic<uint64_t> objectsMade;
#define ASTRA_COUNT_OBJECT() objectsMade.fetch_add(1, std::memory_order_relaxed)
#else
#define ASTRA_COUNT_OBJECT() ((void)0)
#endif
    template <class T, class... A>
    T* alloc(A&&... args) {
        ASTRA_COUNT_OBJECT();
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
        if (!cls->fieldsLaid.load(std::memory_order_acquire)) layFields(cls);
        ASTRA_COUNT_OBJECT();
        const uint32_t count = cls->slotCount;
        // Its payloads before its slots (M6.6 part 6), the values kept inside it after (part 5).
        const bool withValues = cls->inlineLaid.load(std::memory_order_acquire) && !cls->inlineFields.empty();
        const uint32_t inlineSlots = withValues ? cls->inlineSlots : 0;
        const uint32_t slots = count + captures + (typed ? 1u : 0u) + inlineSlots;
        ObjInstance* obj = new (slots, cls->payloadBytes) ObjInstance(cls, count, captures, typed, inlineSlots, withValues, cls->payloadBytes);
        obj->core = coreId_;
        obj->next = objects_;
        objects_ = obj;
        bytesAllocated_ += sizeof(ObjInstance) + slots * sizeof(Value) + cls->payloadBytes;
        return obj;
    }
    // Makes an object of `cls` in the slot under its `argc` arguments and starts its
    // initializer; with `captures`, an anonymous object that keeps them (Op::NewObject).
    // Returns whether a frame was pushed.
    bool construct(ObjClass* cls, int argc, bool packed, const Value* captures = nullptr, uint16_t captureCount = 0);

    // ---- Lists (M6.6 part 3): see "Lists" in vm.cpp ----
    ListStore storeFor(const ObjTypeInfo* item, uint32_t& width, ObjClass*& itemClass);
    void typeList(ObjList* list, ObjTypeInfo* type);

    // ---- Sets (M6.6 part 8), and views of a map's keys and entries (D77) ----
    // Part `index` of a value taken apart (D64).
    Value componentOf(Value whole, size_t index);
    ObjSet* newSet(ObjTypeInfo* type);
    Value mapView(ObjMap* map, bool entries);
    size_t setSize(const ObjSet* set) const { return set->view() ? set->viewOf->map.size() : set->items.size(); }
    bool setContains(const ObjSet* set, Value item);
    bool setAdd(ObjSet* set, Value item);
    bool setRemove(ObjSet* set, Value item);
    std::vector<Value> setItems(const ObjSet* set);
    // Every item of a list, a range, a set, a string, bytes or a map (its entries).
    std::vector<Value> itemsOf(Value from, const char* what);
    AstraThrow viewChanged(const ObjSet* view);
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
    void layInline(ObjClass* cls);
    void layFields(ObjClass* cls);
    void fieldAdded(ObjClass* cls, size_t index);
    // What a field read gives where it is not shown to be safe to read in place: a copy of a
    // value kept inside its object (M6.6 part 5), anything else as it is.
    Value fieldRead(const Value& slot) { return slot.kind == ValueKind::Ref ? ownRef(slot) : slot; }
    void listArrange(ObjList* list, const std::vector<size_t>& order);

    // ---- References (M6.6 part 4) ----
    // A value as it is kept: a Ref replaced by a box with a copy, anything else as it is. What
    // is stored anywhere but a thread's stack goes through this.
    Value own(Value v) { return v.kind == ValueKind::Ref ? ownRef(v) : v; }
    // A value of a value type in a box of its own, whatever form it is in (M6.6 part 5): for
    // the code that works on boxes, and C++ that is given values.
    Value boxed(Value v) { return v.kind == ValueKind::Ref ? ownRef(v) : v.kind == ValueKind::Small ? boxSmall(v) : v; }
    Value boxSmall(Value v);
    void boxAll(Value* from, Value* to) {
        for (Value* at = from; at < to; at++)
            if (at->kind == ValueKind::Ref || at->kind == ValueKind::Small) *at = boxed(*at);
    }
    // A value made of a small value type's bytes, kept in the value itself.
    static Value smallFrom(const ObjClass* cls, const uint8_t* bytes, size_t size) {
        uint64_t bits = 0;
        std::memcpy(&bits, bytes, size);
        return Value::inSlot(cls->smallId, bits);
    }
    std::vector<ObjClass*> smallKept_;  // the small value types this VM numbered, kept alive
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
        } else if (r.as.obj->type == ObjType::Instance) {
            if (write ? r.as.obj->core != coreId_
                      : (r.as.obj->core != 0 && r.as.obj->core != coreId_ && (readable_ == nullptr || !readable_->test(r.as.obj->core))))
                return nullptr;
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
    std::string storageMismatch(Narrow narrow, Value v);
    // Positions and slices (D146, D147).
    bool position(Value index, size_t size, size_t& out, const char* what);
    AstraThrow outOfRange(const char* what, Value index, size_t size);
    void sliceBounds(const ObjRange* range, size_t size, size_t& begin, size_t& end);
    AstraThrow rangeFromEnd(const ObjRange* range, const std::string& use);
    // An int given where a storage width it does not fit is declared: an error saying so, with
    // `where` after it. Returns normally for anything else.
    void checkRange(uint8_t type, Value v, const std::string& where);
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
    // The type of what a value is, as `x#` gives its class (D48): an object's class with the
    // type arguments it was made with, a list's type, `int` for an int. Null has none.
    ObjTypeInfo* typeOfValue(const Value& v);

    // ---- Reflection (D38): see reflect.cpp ----
    // What the description of `cls` gives (Op::ClassMeta), for its type `own` (the class with
    // its type arguments, or null for none): `which` 0 its fields, 1 its methods; null where it
    // has none. Made once for each type.
    Value classDescription(ObjClass* cls, const ObjTypeInfo* own, int which);
    // Whether the code of `proto` may not use the member `name` of an object of `cls`, or may
    // not assign to it; with no code, what only some code may use.
    bool memberDenied(const ObjClass* cls, const ObjString* name, const FunctionProto* proto, bool write) const;
    // A member read, set or found by its name, whoever may use it: as reflection does once it
    // has listed it to the code asking.
    Value memberGet(Value obj, ObjString* name);
    void bundleValues(int count);  // Op::BundleValues
    void mergeBundles();           // Op::MergeBundle
    // Whether a value is an Iterator, an object with hasNext() and next().
    bool isIteratorObject(Value v) const;
    void memberSet(Value obj, ObjString* name, Value value);
    // An atomic field changed in one step (Op::AtomicField, D92).
    Value atomicField(Value object, ObjString* name, int kind, const Value* operands);
    // Calls `callee` with type arguments, as a generic function or class is called.
    Value callWithTypes(Value callee, const std::vector<ObjTypeInfo*>& typeArgs, int argc, const Value* args);

    // ---- astra test (D153) ----
    Value globalAt(int slot) const {
        return slot >= 0 && static_cast<size_t>(slot) < globals_.size() ? globals_[static_cast<size_t>(slot)] : Value::null();
    }
    // A test's time limit, `millis` long, passed: the next safe point throws an AssertionError
    // saying so. Called from another thread; clearTimeLimit() forgets one not yet acted on.
    void timeLimitPassed(int64_t millis) {
        timeLimitMillis_ = millis;
        timeLimitPassed_ = true;
        attention_ = true;
    }
    void clearTimeLimit() { timeLimitPassed_ = false; }
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
    // Whether the caller of `frame` keeps what it returns where it is (Op::WithRefResult).
    bool keptAsItIs(const CallFrame* frame) {
        return (frame->closure->proto->typeArgs & kTakesDestination) != 0 && frameDestination(frame - frames_).kind == ValueKind::Undefined;
    }
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
    ObjString* newString(Characters text);
    ObjString* joinStrings(const ObjString* first, const ObjString* second);
    Value str(std::string s) { return Value::object(newString(std::move(s))); }
    Value str(Characters text) { return Value::object(newString(std::move(text))); }
    // The string of character `c` alone.
    Value characterString(uint32_t c) {
        Characters one;
        one.width = c < 0x100 ? 1 : c < 0x10000 ? 2 : 4;
        one.units.resize(one.width);
        if (one.width == 1) one.units[0] = static_cast<char>(c);
        else if (one.width == 2) { const uint16_t unit = static_cast<uint16_t>(c); std::memcpy(one.units.data(), &unit, 2); }
        else std::memcpy(one.units.data(), &c, 4);
        return str(std::move(one));
    }

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

    AstraThrow runtimeError(const std::string& message);  // a RuntimeException
    AstraThrow runtimeError(Thrown kind, const std::string& message);
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
    // An int handed to Windows, OpenGL or the text engine, which take 32 bits: one past their
    // range is taken as their nearest end (D76), as `int.maxValue` for "no limit" is.
    int toInt32(Value v, const char* what) {
        return static_cast<int>(std::clamp<int64_t>(toInt(v, what), INT32_MIN, INT32_MAX));
    }
    double toNumber(Value v, const char* what);
    const std::string& toStr(Value v, const char* what);
    // A string, as its characters (D20); toStr gives its UTF-8 form.
    const ObjString* toText(Value v, const char* what) {
        if (!v.isString()) toStr(v, what);  // says what it should have been
        return asString(v);
    }

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

    // ---- Threads, the event loop and tasks (threads.cpp) ----
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
    Value innermostMonitor() const;
    int monitorLetGo(Value lock);
    void monitorTakeBack(Value lock, int count);
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
        if (obj->core != coreId_ && obj->core != 0) [[unlikely]] {
            if (readable_ == nullptr || !readable_->test(obj->core)) throw otherCores(obj);
        }
    }
    void guardRead(Value v) const {
        if (v.kind == ValueKind::Obj) guardRead(v.as.obj);
    }
    void guardWrite(const Obj* obj) const {
        if (obj->core != coreId_ || obj->frozen != 0) [[unlikely]] throw notOwned(obj);
    }
    // A frozen copy of a list, a map or a set (D26), which nobody may change; the lists, maps
    // and sets inside it are frozen copies too. One already frozen is itself.
    Value frozenCopy(Value v, int depth = 0);
    // A lazy field's, static's or global's value (D37), worked out on this first read and given
    // to `store` for its place; `what` names it for the errors.
    Value forceLazy(Value marker, const std::function<void(Value)>& store, const std::string& what);
    void wakeLazyWaiters(ObjLazy* lazy);
    // A global's value where a read finds none: a lazy one's, worked out; an error otherwise.
    Value globalOrLazy(size_t index);
    // Keys (D26): a class's own == and hashCode, called for a map or a set, and the error
    // for a key that can change.
    bool hashOf(const ObjClass* cls, const Value& v, size_t& out) override;
    bool equalOf(const ObjClass* cls, const Value& a, const Value& b, bool& out) override;
    [[noreturn]] void refuseKey(const Value& key) override;
    // Says which of a type's built-in methods change the value they are called on, and which
    // built-in functions are not for a parallel section (by the object they belong to).
    void markMutating(BuiltinType type, std::initializer_list<const char*> methods);
    void markTakesPlaces(BuiltinType type, std::initializer_list<const char*> methods);
    // Op::Numeric's work (builtins.cpp, beside Math's functions): the result of `which` on
    // `args`, one value or two.
    Value numeric(uint8_t which, const Value* args);
    AstraThrow overflowError(Op op, int64_t a, int64_t b);
    void markNotParallel(const std::string& global, std::initializer_list<const char*> except = {});

    // Runs `function` on every item of `source` (a list, a range, anything a loop takes), the
    // items dealt out to the cores. With `keep`, gives the list of what it returned, in order.
    // With `replace`, each item of the list `source` is replaced by what the function gives for it.
    Value runParallel(Value source, Value function, bool keep, bool replace = false);
    // Runs `function` on each of `parts` ranges that together cover a `bytes`, one core to
    // a range, each allowed to write that part of it.
    void runParallelChunks(Value buffer, Value function, size_t align = 1);
    int parallelCores();
    // The core of a parallel section this VM is (1 and up), or 0 for a program's own.
    uint8_t coreId() const { return coreId_; }
    // parallel({ a }, { b }) (D96): each function at once, on whatever cores are free; what they
    // give, in order.
    Value runParallelFunctions(const std::vector<Value>& functions);
    // gather (D93): for each piece of the items, `into` makes a result and `each` adds an item
    // to it; the pieces' results, in order.
    Value runParallelGather(Value source, Value into, Value each);
    // Runs a section from this VM, the program's or a core's, and waits for its pieces.
    void runSection(ParallelSection& section);
    // One piece of a section, run on this core's VM.
    void runParallelPiece(ParallelPiece& piece);

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

    // ---- Frozen data shared with workers (D97, worker.cpp) ----
    // The blocks outside every heap this VM holds, and what of its own frozen data was shared as
    // which. A collection notes the blocks it reaches; those it does not are let go.
    std::unordered_set<SharedBlock*> heldBlocks;
    std::unordered_set<SharedBlock*> reachedBlocks;
    std::unordered_map<const Obj*, SharedBlock*> sharedAs;
    void noteOutside(const Obj* obj);
    void releaseUnreachedBlocks();
    void releaseAllBlocks();
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

    // Tasks.
    ObjFiber* currentFiber() const { return context_->fiber; }
    // Starts or takes up a task, giving it `value` as the result of what it was waiting for
    // (or, with `ok` false, raising it there as an error). Returns when the task finishes or
    // is set aside again; an error the task does not catch comes out of this call.
    void runFiber(ObjFiber* fiber, bool ok, Value value);
    // Whether a built-in function called now could set the running task aside.
    bool canSuspend() const { return context_->fiber != nullptr && !context_->fiber->generator && canSetAside(); }
    // Whether what runs now could be set aside: no built-in function is calling back into it.
    bool canSetAside() const { return nativeDepth_ == 0 && nativeBase_ != nullptr; }
    // Sets the running task aside: does not return. The task goes on when runFiber is called.
    [[noreturn]] void suspendFiber();

    // Runs `work`, which blocks and must not touch the VM, and gives what `finish` makes of
    // it. In a task on the loop thread the task is set aside while a worker thread does the
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
    static constexpr int kMaxTaskNesting = 60;
    static constexpr size_t kMinHeap = 4 * 1024 * 1024;

    // Execution state: that of the context that is running (see Context).
    Value* stack_;
    Value* stackEnd_;
    Value* sp_;
    CallFrame* frames_;
    int frameCount_ = 0;
    int maxFrames_ = 0;
    int frameLimit_ = 0;
    // Whether the stack has room up to `top`: it has, or it grew to have it (Context::grow).
    bool roomFor(const Value* top) { return top <= stackEnd_ || growContext(top, frameCount_); }
    bool growContext(const Value* top, int frames);
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
    std::vector<ObjFiber*> waitingFibers_;  // tasks set aside until work in progress is done
    bool isWorker_ = false;
    int liveWorkers_ = 0;

    // Parallel sections
    VM* root_ = nullptr;           // for a core's VM: the VM whose objects it works on
    // Which piece of work this VM is running, by the number what the piece owns is marked with;
    // 0 for a VM that is not a core's. A core's runs one piece after another, and pieces inside
    // pieces (D96): each reads, besides its own and what is shared, what `readable_` holds,
    // the numbers of the pieces that started its section.
    uint8_t coreId_ = 0;
    const std::bitset<256>* readable_ = nullptr;
    ParallelSection* section_ = nullptr;   // the section of the piece running
    uint64_t seenGeneration_ = 0;          // the outermost section it last took the program's state for
    int coreIndex_ = -1;                   // which core's VM this is
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
    std::atomic<bool> timeLimitPassed_{false};
    std::atomic<int64_t> timeLimitMillis_{0};

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
    ObjClass* thrownClasses_[static_cast<size_t>(Thrown::Count_)] = {};
    ObjClass* mapEntryClass_ = nullptr;  // MapEntry, from the standard library: what a map's entries are
    ObjClass* viewClass_ = nullptr;      // ListView, from the standard library: what splitAt hands out (D96)
    ObjString* opNames_[static_cast<int>(Op::Count_)] = {};

    // Heap state
    Obj* objects_ = nullptr;
    size_t bytesAllocated_ = 0;
    size_t nextGC_ = kMinHeap;
    bool gcStress_ = false;  // ASTRA_GC_STRESS: collect at every safe point, to expose rooting bugs
    // A class's own == or hashCode running for a map or a set (D26): the C++ around it holds
    // objects nothing roots (a map being built, its entries), so meanwhile nothing is collected
    // and the lock is not handed to another thread.
    int keyCalls_ = 0;
    friend struct KeyCall;
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
    // What ImplicitFirst gave the next lookup of a bare name (D58, D59).
    mutable Value implicitFirst_[8];
    mutable uint8_t implicitFirstCount_ = 0;
    mutable bool implicitOnly_ = false;
    mutable uint8_t implicitTried_ = 0;  // how many of them the last lookup tried, for its error
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
    Table typeExtensions_[kTypeCount_];
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

    bool numberArithmetic(Op op);
    bool compareSlow(Op op);
    bool shift(Op op);
    bool bitwise(Op op);

    bool matchesType(const Value& v, uint8_t type, const ObjClass* cls) const;
    // A byte kept where an int or a Number is declared becomes that int (8.9c): asked only once
    // the value has failed its type's check, so that the checks that pass cost nothing more.
    // True where it was widened, and so now fits.
    static bool widenedByte(Value& v, uint8_t type);
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
        if (v.kind == ValueKind::Obj || v.kind == ValueKind::Ref) markObject(v.as.obj);  // a Small holds no object
        else if (v.kind == ValueKind::Undefined && v.as.obj != nullptr) markObject(v.as.obj);  // a lazy one's ObjLazy
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
void registerThreads(VM& vm);  // threads.cpp: threads, locks, the event loop and tasks
void registerIo(VM& vm);       // io.cpp: streams, processes and sockets
void registerGrid(VM& vm);     // grid.cpp: what Grid<T> is built on (D155)
void registerWorkers(VM& vm);  // worker.cpp: isolated workers
void registerParallel(VM& vm); // parallel.cpp: parallel sections
void registerText(VM& vm);     // text/api.cpp: fonts, text and pictures
// What started this program (see __io.selfCommand): set by main before the program runs.
void setSelfCommand(std::vector<std::string> command);

// The native layer of the Jade library (lib/Jade): windows, events and drawing. jade/api.cpp.
void registerJade(VM& vm);

}  // namespace astra
