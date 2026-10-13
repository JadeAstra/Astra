// Parallel sections: several cores running Astra code on the same objects at once. The
// built-in __parallel, which src/std/parallel.astra is written on.
//
// Threads take turns and workers copy; this does neither. A section is one call: the items
// of a list are dealt out to the cores, a function is run on each, and the call returns
// when every core is done. Meanwhile the VM that started it waits with its lock held, so
// nothing else of the program runs.
//
// Each core has a VM of its own for the interpreter's working state (stack, calls in
// progress), made once and kept. These VMs share the heap of the one that started the
// section, which is safe because of one rule, checked wherever an object is looked into
// (VM::guardRead, VM::guardWrite):
//
//   - an object is either shared (Obj::core is 0) or owned by one core;
//   - a core reads what is shared or its own, and changes only what is its own;
//   - a core owns the items it was dealt, and whatever it creates.
//
// So what every core can reach is never changed while the section runs, and what is changed
// is reached by one core only. A function that breaks the rule gets an error at the first
// attempt, not a damaged object. What the cores created joins the rest of the heap when the
// section ends; until then nothing is collected.

#include <algorithm>
#include <cstdlib>
#include <thread>

#include "builtins.h"
#include "vm.h"

namespace astra {

// One section: what to run, and each core's share of it.
struct ParallelJob {
    Value function;
    const std::vector<Value>* items = nullptr;  // the function is called on each of these
    bool keep = false;                          // what it returns is wanted
    const Obj* buffer = nullptr;                // or: called once per share with the share's range of this buffer

    struct Share {
        size_t from = 0, to = 0;
        std::vector<Value> results;
        bool failed = false;
        AstraThrow error;
    };
    std::vector<Share> shares;
    std::atomic<bool> cancelled{false};  // one core has failed: the others stop at their next item
};

// The cores: a thread and a VM each, waiting for a section to run.
struct ParallelPool {
    VM& root;
    const int count;
    std::vector<std::thread> threads;
    std::vector<VM*> vms;

    std::mutex mutex;
    std::condition_variable wake, settled;
    uint64_t generation = 0;
    ParallelJob* job = nullptr;
    int running = 0;
    int ready = 0;
    bool quitting = false;

    ParallelPool(VM& owner, int cores) : root(owner), count(cores), vms(static_cast<size_t>(cores), nullptr) {
        for (int i = 0; i < count; i++) threads.emplace_back([this, i] { coreMain(i); });
        // Their VMs are made from the root's tables, which is safe now: the root is here, waiting.
        std::unique_lock<std::mutex> hold(mutex);
        settled.wait(hold, [&] { return ready == count; });
    }

    ~ParallelPool() {
        {
            std::lock_guard<std::mutex> hold(mutex);
            quitting = true;
        }
        wake.notify_all();
        for (std::thread& thread : threads) thread.join();
    }

    // Runs a section: returns when every core has done its share.
    void run(ParallelJob& section) {
        {
            std::lock_guard<std::mutex> hold(mutex);
            job = &section;
            running = count;
            generation++;
        }
        wake.notify_all();
        std::unique_lock<std::mutex> hold(mutex);
        settled.wait(hold, [&] { return running == 0; });
        job = nullptr;
    }

    void coreMain(int index) {
        VM* vm = new VM(root, static_cast<uint8_t>(index + 1));
        currentCore = static_cast<uint8_t>(index + 1);
        {
            std::lock_guard<std::mutex> hold(mutex);
            vms[static_cast<size_t>(index)] = vm;
            ready++;
        }
        settled.notify_all();

        uint64_t seen = 0;
        for (;;) {
            ParallelJob* section = nullptr;
            {
                std::unique_lock<std::mutex> hold(mutex);
                wake.wait(hold, [&] { return quitting || generation != seen; });
                if (quitting) break;
                seen = generation;
                section = job;
            }
            if (static_cast<size_t>(index) < section->shares.size()) vm->runParallelShare(*section, static_cast<size_t>(index));
            {
                std::lock_guard<std::mutex> hold(mutex);
                running--;
            }
            settled.notify_all();
        }
        delete vm;
    }
};

// ---- A core's VM ----

VM::VM(VM& root, uint8_t coreId)
    : output(root.output), input(root.input), rng(std::random_device{}()), mainThread_(kThreadStackSize, kThreadMaxFrames),
      isWorker_(root.isWorker_), root_(&root), coreId_(coreId) {
    mainThread_.name = "core-" + std::to_string(coreId);
    mainThread_.id = nextThreadId_++;
    threads_.push_back(&mainThread_);
    loadContext(&mainThread_.base);

    // The same names and built-in methods as the root; no table of strings of its own (see
    // intern); and nothing is collected: what it makes is handed to the root when a section ends.
    names = root.names;
    for (size_t i = 0; i < static_cast<size_t>(Op::Count_); i++) opNames_[i] = root.opNames_[i];
    for (size_t i = 0; i < static_cast<size_t>(BuiltinType::Count_); i++) methods_[i] = root.methods_[i];
    scriptArgs = root.scriptArgs;
    nextGC_ = static_cast<size_t>(-1);
    extensionEpoch_ = static_cast<uint64_t>(-1);
}

// Before each section: the program's variables and functions as they are now. A core has a
// copy of the table of variables, which it reads and never writes.
void VM::refreshFromRoot() {
    program_ = root_->program_;
    globals_ = root_->globals_;
    globalTypes_ = root_->globalTypes_;
    loaded_ = root_->loaded_;
    errorClass_ = root_->errorClass_;
    mapEntryClass_ = root_->mapEntryClass_;
    hasExtensions_ = root_->hasExtensions_;
    if (extensionEpoch_ != root_->extensionEpoch_) {
        for (size_t i = 0; i < 16; i++) {
            typeExtensions_[i].clear();
            typeExtensions_[i].addAll(root_->typeExtensions_[i]);
        }
        nullExtensions_.clear();
        nullExtensions_.addAll(root_->nullExtensions_);
        anyExtensions_.clear();
        anyExtensions_.addAll(root_->anyExtensions_);
        extensionEpoch_ = root_->extensionEpoch_;
    }
    pendingEnums_ = 0;
    nextGC_ = static_cast<size_t>(-1);
}

// After a section: what a core created becomes the root's, and nobody's in particular.
void VM::adoptObjects(VM& core) {
    Obj* head = core.objects_;
    if (head == nullptr) return;
    Obj* tail = head;
    for (Obj* obj = head; obj != nullptr; obj = obj->next) {
        obj->core = 0;
        tail = obj;
    }
    tail->next = objects_;
    objects_ = head;
    bytesAllocated_ += core.bytesAllocated_;
    core.objects_ = nullptr;
    core.bytesAllocated_ = 0;
}

void VM::runParallelShare(ParallelJob& job, size_t index) {
    refreshFromRoot();
    ParallelJob::Share& share = job.shares[index];
    try {
        if (job.buffer != nullptr) {
            // One call, with the part of the buffer that is this core's to write.
            bandBuffer_ = job.buffer;
            bandFrom_ = share.from;
            bandTo_ = share.to;
            Value range = Value::object(alloc<ObjRange>(static_cast<int64_t>(share.from), static_cast<int64_t>(share.to), int64_t{1}, false));
            call(job.function, 1, &range);
        } else {
            if (job.keep) share.results.reserve(share.to - share.from);
            for (size_t i = share.from; i < share.to && !job.cancelled.load(std::memory_order_relaxed); i++) {
                Value item = (*job.items)[i];
                Value result = call(job.function, 1, &item);
                if (job.keep) share.results.push_back(result);
            }
        }
    } catch (AstraThrow& thrown) {
        share.error = thrown;
        share.failed = true;
        job.cancelled = true;
    } catch (const AstraExit&) {
        share.error = runtimeError("exit() was called in a parallel section");
        share.failed = true;
        job.cancelled = true;
    } catch (const FiberSuspend&) {
        share.error = runtimeError("a parallel section does not wait");
        share.failed = true;
        job.cancelled = true;
    }
    bandBuffer_ = nullptr;
}

// ---- Starting a section ----

namespace {

int coreCount() {
    int count = static_cast<int>(std::thread::hardware_concurrency());
    // ASTRA_CORES: how many cores a section uses, whatever the machine has. For tests.
    if (const char* wanted = std::getenv("ASTRA_CORES"); wanted != nullptr && *wanted != '\0') count = std::atoi(wanted);
    return std::clamp(count, 1, 64);
}

bool ownable(const Obj* obj) {
    return obj->type == ObjType::Instance || obj->type == ObjType::List || obj->type == ObjType::Map || obj->type == ObjType::Bytes;
}

// Gives back, when the section is over however it ends, the items that were dealt out.
struct Dealt {
    std::vector<Obj*> objects;
    ~Dealt() {
        for (Obj* obj : objects) obj->core = 0;
    }
};

std::string describe(const VM& vm, const Obj* obj) {
    switch (obj->type) {
        case ObjType::Instance: return "this " + static_cast<const ObjInstance*>(obj)->cls->name->chars;
        case ObjType::Class: return "class " + static_cast<const ObjClass*>(obj)->name->chars;
        case ObjType::Upvalue: return "this variable";
        default: return "this " + vm.typeName(Value::object(const_cast<Obj*>(obj)));
    }
}

}  // namespace

int VM::parallelCores() {
    if (root_ != nullptr) return root_->parallelCores();
    if (parallelPool_ == nullptr) parallelPool_ = std::make_shared<ParallelPool>(*this, coreCount());
    return parallelPool_->count;
}

Value VM::runParallel(Value source, Value function, bool keep) {
    std::vector<Value> items;
    TempVector holdItems(*this, items);
    collect(source, items);
    ObjList* out = keep ? newList(*this) : nullptr;
    TempRoot holdOut(*this, keep ? Value::object(out) : Value::null());
    if (items.empty()) return keep ? Value::object(out) : Value::null();

    const size_t shares = std::min(static_cast<size_t>(parallelCores()), items.size());
    ParallelJob job;
    job.function = function;
    job.items = &items;
    job.keep = keep;
    job.shares.resize(shares);
    for (size_t k = 0; k < shares; k++) {
        job.shares[k].from = items.size() * k / shares;
        job.shares[k].to = items.size() * (k + 1) / shares;
    }

    {
        // Each core owns its items for as long as the section lasts.
        Dealt dealt;
        for (size_t k = 0; k < shares; k++) {
            for (size_t i = job.shares[k].from; i < job.shares[k].to; i++) {
                if (!items[i].isObj() || !ownable(items[i].as.obj)) continue;
                Obj* obj = items[i].as.obj;
                if (obj->core != 0)
                    throw runtimeError("the same " + typeName(items[i]) + " is in the stream more than once (it is item " + std::to_string(i) +
                                       " as well): each core has to have its items to itself");
                obj->core = static_cast<uint8_t>(k + 1);
                dealt.objects.push_back(obj);
            }
        }
        parallelPool_->run(job);
        for (VM* core : parallelPool_->vms) adoptObjects(*core);
    }

    for (const ParallelJob::Share& share : job.shares)
        if (share.failed) throw share.error;
    if (!keep) return Value::null();
    out->items.reserve(items.size());
    for (const ParallelJob::Share& share : job.shares) out->items.insert(out->items.end(), share.results.begin(), share.results.end());
    account(out->items.size() * sizeof(Value));
    return Value::object(out);
}

void VM::runParallelChunks(Value buffer, Value function) {
    if (!buffer.isObjType(ObjType::Bytes)) throw runtimeError("parallelChunks is for bytes, not a value of type " + typeName(buffer));
    const size_t size = asBytes(buffer)->data.size();
    if (size == 0) return;

    const size_t shares = std::min(static_cast<size_t>(parallelCores()), size);
    ParallelJob job;
    job.function = function;
    job.buffer = buffer.as.obj;
    job.shares.resize(shares);
    for (size_t k = 0; k < shares; k++) {
        job.shares[k].from = size * k / shares;
        job.shares[k].to = size * (k + 1) / shares;
    }
    parallelPool_->run(job);
    for (VM* core : parallelPool_->vms) adoptObjects(*core);
    for (const ParallelJob::Share& share : job.shares)
        if (share.failed) throw share.error;
}

// ---- What a core is told when it oversteps ----

AstraThrow VM::notOwned(const std::string& what) const {
    return const_cast<VM*>(this)->runtimeError("a parallel section changes only the item it was given and what it makes itself: " + what);
}

AstraThrow VM::notOwned(const Obj* obj) const {
    if (obj->core != 0) return otherCores(obj);
    return notOwned(describe(*this, obj) + " is shared with the other cores, so here it can be read and not changed");
}

AstraThrow VM::otherCores(const Obj* obj) const {
    return const_cast<VM*>(this)->runtimeError(describe(*this, obj) + " is another core's item in this parallel section: a core looks only into its own items " +
                                               "and into what is shared");
}

void VM::markMutating(BuiltinType type, std::initializer_list<const char*> methods) {
    for (const char* name : methods) methods_[static_cast<int>(type)].at(intern(name)).mutates = true;
}

void VM::markNotParallel(const std::string& global, std::initializer_list<const char*> except) {
    auto found = builtins_.find(global);
    if (found == builtins_.end()) return;
    if (found->second.isObjType(ObjType::Native)) {
        asNative(found->second)->parallelSafe = false;
    } else if (found->second.isObjType(ObjType::Class)) {
        const Table& members = asClass(found->second)->statics;
        for (uint32_t i = 0; i < members.capacity(); i++) {
            const Table::Entry& entry = members.entries()[i];
            if (entry.key == nullptr || !entry.value.isObjType(ObjType::Native)) continue;
            bool excepted = false;
            for (const char* name : except) excepted = excepted || entry.key->chars == name;
            asNative(entry.value)->parallelSafe = excepted;
        }
    }
}

void registerParallel(VM& vm) {
    ObjClass* parallel = vm.alloc<ObjClass>(vm.intern("parallel"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        parallel->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("parallel.") + name, minArgs, maxArgs, fn)));
    };

    // run(source, function, keep): the function on every item, the items dealt out to the
    // cores. With `keep`, the list of what it gave, in the order of the items.
    def("run", 3, 3, [](VM& vm, int, Value* args) -> Value { return vm.runParallel(args[0], args[1], args[2].truthy()); });

    // chunks(buffer, function): the function once on each core, with the range of positions
    // of the buffer that the core may write.
    def("chunks", 2, 2, [](VM& vm, int, Value* args) -> Value {
        vm.runParallelChunks(args[0], args[1]);
        return Value::null();
    });

    def("cores", 0, 0, [](VM& vm, int, Value*) -> Value { return Value::integer(vm.parallelCores()); });

    vm.defineGlobal("__parallel", Value::object(parallel));
}

}  // namespace astra
