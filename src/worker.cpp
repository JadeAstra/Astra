// Isolated workers: the built-in __worker, which src/std/worker.astra is written on.
//
// A worker is a second VM, on a thread of its own, with a heap and a lock of its own. So
// unlike a thread it really runs at the same time as the program that started it, on another
// core. The price is that the two share nothing: a worker runs a script of its own (compiled
// into the program, see CompilerSession::loadWorker), and what passes between it and its
// parent are messages, which are copied from the one heap into the other.

#include <algorithm>
#include <cstring>
#include <deque>
#include <iostream>
#include <thread>

#include "builtins.h"
#include "vm.h"

namespace astra {

// What a worker and the program that started it share. Everything in it is guarded by the
// mutex, which is what lets either side go away while the other still holds the link.
struct WorkerLink {
    std::mutex mutex;
    VM* parent = nullptr;            // the VM that started the worker
    VM* worker = nullptr;            // the worker's own; null before it is up and after it has ended
    std::deque<std::string> early;   // messages sent before it was up
    int64_t id = 0;
    bool daemon = false;
    bool ended = false;
    std::string path;                // of its script, as the program wrote it
};

// ---- Frozen data shared between workers (D97) ----
//
// Frozen data sent to a worker (a frozenList, frozenMap or frozenSet, or frozen bytes, holding only
// numbers, text and other frozen data) is not copied into each worker's heap: it is copied once
// into a block outside every heap, which each VM that is sent it reads in place. A VM's
// collection notes the blocks it still reaches and lets go of the others, and the last to let
// go frees the block. What the sender shared stays shared as the same block for as long as the
// sender keeps the frozen data.

struct SharedBlock {
    std::vector<Obj*> objects;   // all of it, the first the whole
    int holds = 0;               // the VMs that hold it, and the messages carrying it
};

namespace {

std::mutex blocksMutex;
std::unordered_map<const Obj*, SharedBlock*> blockOf;  // every object outside the heaps, to its block

void letGo(SharedBlock* block) {
    std::lock_guard<std::mutex> hold(blocksMutex);
    if (--block->holds > 0) return;
    for (Obj* obj : block->objects) {
        blockOf.erase(obj);
        delete obj;
    }
    delete block;
}

void take(SharedBlock* block) {
    std::lock_guard<std::mutex> hold(blocksMutex);
    block->holds++;
}

// A copy of frozen data outside every heap, every object of it in `block`.
Value outsideCopy(VM& vm, Value v, SharedBlock& block, int depth) {
    if (depth > 64) throw vm.runtimeError(Thrown::IllegalArgumentException, "a message is nested too deeply (or holds itself)");
    if (!v.isObj()) return v;
    Obj* made = nullptr;
    switch (v.as.obj->type) {
        case ObjType::String: {
            ObjString* text = new ObjString(asString(v)->utf8());
            text->utf8();  // its UTF-8 made now, while one thread holds it
            made = text;
            break;
        }
        case ObjType::Bytes: {
            ObjBytes* bytes = new ObjBytes();
            bytes->data = asBytes(v)->data;
            made = bytes;
            break;
        }
        case ObjType::List: {
            ObjList* list = new ObjList();
            block.objects.push_back(list);
            for (const Value& item : vm.listCopy(asList(v))) list->values().push_back(outsideCopy(vm, item, block, depth + 1));
            list->frozen = v.as.obj->frozen;
            list->outside = true;
            list->immortal = true;
            return Value::object(list);
        }
        case ObjType::Map: {
            ObjMap* map = new ObjMap();
            block.objects.push_back(map);
            for (const ValueMap::Entry& entry : asMap(v)->map.entries())
                if (entry.live) map->map.set(outsideCopy(vm, entry.key, block, depth + 1), outsideCopy(vm, entry.value, block, depth + 1));
            map->frozen = v.as.obj->frozen;
            map->outside = true;
            map->immortal = true;
            return Value::object(map);
        }
        case ObjType::Set: {
            ObjSet* set = new ObjSet();
            block.objects.push_back(set);
            for (const Value& item : vm.setItems(asSet(v))) set->items.set(outsideCopy(vm, item, block, depth + 1), Value::null());
            set->frozen = v.as.obj->frozen;
            set->outside = true;
            set->immortal = true;
            return Value::object(set);
        }
        default:
            throw vm.runtimeError(Thrown::IllegalArgumentException, "frozen data sent to a worker holds numbers, text, bytes and frozen lists, maps and sets, not a value of type " +
                                                                        vm.typeName(v) + ": the two share no objects");
    }
    made->frozen = v.as.obj->frozen;
    made->outside = true;
    made->immortal = true;
    block.objects.push_back(made);
    return Value::object(made);
}

// The block a VM's frozen data is shared as, made the first time it is sent; one more hold on
// it, for the message carrying it.
SharedBlock* shareFrozen(VM& vm, Value v) {
    if (v.as.obj->outside) {
        // Data this VM was itself sent, sent on: the same block.
        std::lock_guard<std::mutex> hold(blocksMutex);
        SharedBlock* block = blockOf.at(v.as.obj);
        block->holds++;
        return block;
    }
    auto known = vm.sharedAs.find(v.as.obj);
    SharedBlock* block = known != vm.sharedAs.end() ? known->second : nullptr;
    if (block == nullptr) {
        block = new SharedBlock;
        outsideCopy(vm, v, *block, 0);  // the whole first in the block
        {
            std::lock_guard<std::mutex> hold(blocksMutex);
            for (Obj* obj : block->objects) blockOf[obj] = block;
            block->holds = 1;  // the sender's, for as long as it keeps the data
        }
        vm.sharedAs[v.as.obj] = block;
        vm.heldBlocks.insert(block);
    }
    take(block);
    return block;
}

// ---- Messages ----
// A value is written out as bytes on the one side and built again on the other. It can hold
// null, booleans, numbers, strings, bytes, and lists and maps of those. Frozen data travels as
// the block it is shared as; bytes handed over travel as their own buffer, moved.

enum : char { kNull, kFalse, kTrue, kInt, kFloat, kString, kBytes, kList, kMap, kChar, kByte, kSet, kShared, kHandedOver };

void putRaw(std::string& out, const void* data, size_t size) { out.append(static_cast<const char*>(data), size); }

void putSize(std::string& out, size_t size) {
    const uint32_t n = static_cast<uint32_t>(size);
    putRaw(out, &n, sizeof n);
}

void encode(VM& vm, Value v, std::string& out, int depth) {
    if (depth > 64) throw vm.runtimeError(Thrown::IllegalArgumentException, "a message is nested too deeply (or holds itself)");
    if (v.isSmall() || v.isRef()) v = vm.boxed(v);  // a value of a value type, as a box (M6.6 part 5)
    switch (v.kind) {
        case ValueKind::Null: out += kNull; return;
        case ValueKind::Bool: out += v.as.b ? kTrue : kFalse; return;
        case ValueKind::Int:
            out += kInt;
            putRaw(out, &v.as.i, sizeof v.as.i);
            return;
        case ValueKind::Float:
            out += kFloat;
            putRaw(out, &v.as.d, sizeof v.as.d);
            return;
        case ValueKind::Char:
        case ValueKind::Byte:
            out += v.isChar() ? kChar : kByte;
            putRaw(out, &v.as.i, sizeof v.as.i);
            return;
        default: break;
    }
    if (v.isObj() && v.as.obj->frozen != 0 && v.as.obj->type != ObjType::String) {
        // Frozen data: shared, not copied (D97).
        SharedBlock* block = shareFrozen(vm, v);
        out += kShared;
        putRaw(out, &block, sizeof block);
        return;
    }
    if (v.isString()) {
        out += kString;
        putSize(out, asString(v)->utf8().size());
        out += asString(v)->utf8();
    } else if (v.isObjType(ObjType::Bytes)) {
        out += kBytes;
        putSize(out, asBytes(v)->data.size());
        putRaw(out, asBytes(v)->data.data(), asBytes(v)->data.size());
    } else if (v.isObjType(ObjType::List)) {
        const std::vector<Value> items = vm.listCopy(asList(v));
        out += kList;
        putSize(out, items.size());
        for (const Value& item : items) encode(vm, item, out, depth + 1);
    } else if (v.isObjType(ObjType::Set)) {
        const std::vector<Value> items = vm.setItems(asSet(v));
        out += kSet;
        putSize(out, items.size());
        for (const Value& item : items) encode(vm, item, out, depth + 1);
    } else if (v.isObjType(ObjType::Map)) {
        const ObjMap* map = asMap(v);
        out += kMap;
        putSize(out, map->map.size());
        for (const ValueMap::Entry& entry : map->map.entries()) {
            if (!entry.live) continue;
            encode(vm, entry.key, out, depth + 1);
            encode(vm, entry.value, out, depth + 1);
        }
    } else {
        throw vm.runtimeError(Thrown::IllegalArgumentException, "a message to or from a worker can hold numbers, strings, booleans, null, bytes, and lists and maps of those, not a value of type " +
                              vm.typeName(v) + ": the two share no objects");
    }
}

struct Reader {
    const char* at;
    const char* end;
    template <class T>
    T raw() {
        T value{};
        if (static_cast<size_t>(end - at) >= sizeof(T)) {
            std::memcpy(&value, at, sizeof(T));
            at += sizeof(T);
        }
        return value;
    }
    std::string_view text() {
        const size_t size = std::min<size_t>(raw<uint32_t>(), static_cast<size_t>(end - at));
        std::string_view view(at, size);
        at += size;
        return view;
    }
};

// No collection can run while a built-in function is at work, so what is being built needs
// no protecting until it is handed to the program.
Value decode(VM& vm, Reader& in) {
    if (in.at >= in.end) return Value::null();
    switch (*in.at++) {
        case kFalse: return Value::boolean(false);
        case kTrue: return Value::boolean(true);
        case kInt: return Value::integer(in.raw<int64_t>());
        case kFloat: return Value::number(in.raw<double>());
        case kChar: {
            const int64_t c = in.raw<int64_t>();
            if (!isCharacter(c)) throw std::runtime_error("a char in a message that is not a character");
            return Value::character(static_cast<uint32_t>(c));
        }
        case kByte: return Value::byte(static_cast<uint8_t>(in.raw<int64_t>()));
        case kString: return vm.str(std::string(in.text()));
        case kShared: {
            // What frozen data was shared as: the message's hold on it becomes this VM's, if it
            // does not hold the block already.
            SharedBlock* block = in.raw<SharedBlock*>();
            if (block == nullptr) return Value::null();
            if (!vm.heldBlocks.insert(block).second) letGo(block);
            return Value::object(block->objects.front());
        }
        case kHandedOver: {
            // A buffer moved here, not copied.
            std::vector<uint8_t>* data = in.raw<std::vector<uint8_t>*>();
            ObjBytes* made = vm.alloc<ObjBytes>();
            if (data != nullptr) {
                made->data.swap(*data);
                delete data;
            }
            vm.account(made->data.size());
            return Value::object(made);
        }
        case kBytes: {
            const std::string_view data = in.text();
            ObjBytes* made = vm.alloc<ObjBytes>();
            made->data.assign(data.begin(), data.end());
            vm.account(made->data.size());
            return Value::object(made);
        }
        case kList: {
            const uint32_t count = in.raw<uint32_t>();
            ObjList* list = newList(vm);
            list->values().reserve(std::min<size_t>(count, static_cast<size_t>(in.end - in.at)));
            for (uint32_t i = 0; i < count && in.at < in.end; i++) list->values().push_back(decode(vm, in));
            vm.account(list->values().size() * sizeof(Value));
            return Value::object(list);
        }
        case kMap: {
            const uint32_t count = in.raw<uint32_t>();
            ObjMap* map = vm.alloc<ObjMap>();
            for (uint32_t i = 0; i < count && in.at < in.end; i++) {
                const Value key = decode(vm, in);
                map->map.set(key, decode(vm, in));
            }
            vm.account(map->map.size() * 2 * sizeof(Value));
            return Value::object(map);
        }
        case kSet: {
            const uint32_t count = in.raw<uint32_t>();
            ObjSet* set = vm.alloc<ObjSet>();
            for (uint32_t i = 0; i < count && in.at < in.end; i++) set->items.set(decode(vm, in), Value::null());
            vm.account(set->items.size() * 64);
            return Value::object(set);
        }
        default: return Value::null();
    }
}

// `post(bytes, handOver = true)` (D97): the buffer moved into the message, the sender's left
// empty, so that only one side has it.
void handOver(VM& vm, Value v, std::string& out) {
    if (!v.isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::IllegalArgumentException, "what is handed over is bytes, not a value of type " + vm.typeName(v));
    vm.guardWrite(v.as.obj);
    auto* data = new std::vector<uint8_t>();
    data->swap(asBytes(v)->data);
    out += kHandedOver;
    putRaw(out, &data, sizeof data);
}

// Calls the function in field `name` of `object`, if there is one, with one argument.
void callField(VM& vm, Value object, const char* name, Value argument) {
    if (!object.isObjType(ObjType::Instance)) return;
    const Value* handler = asInstance(object)->field(vm.intern(name));
    if (handler == nullptr || handler->isNull()) return;
    TempRoot keep(vm, argument);
    vm.call(*handler, 1, &argument);
}

// Run on the worker's loop: a message from its parent.
void deliverToWorker(VM& vm, const std::string& data) {
    Reader in{data.data(), data.data() + data.size()};
    callField(vm, vm.workerSelf, "onMessage", decode(vm, in));
}

// ---- The worker's thread ----

void workerMain(std::shared_ptr<WorkerLink> link, std::shared_ptr<const std::string> image, int entry, std::vector<std::string> arguments,
                std::function<void(VM&, AstraThrow&, const std::string&)> onThreadError) {
    // Kept on the heap: if threads of its own are still running when the worker ends, the VM
    // is left as it is rather than taken apart under them.
    VM* vm = new VM(true);
    Program* program = new Program;
    vm->scriptArgs = std::move(arguments);
    vm->workerLink = link;
    vm->onThreadError = onThreadError;
    vm->programImage = [image] { return *image; };

    std::string failure;
    try {
        readProgram(*program, *vm, *image);
        vm->load(*program);
        {
            std::lock_guard<std::mutex> hold(link->mutex);
            link->worker = vm;
            for (std::string& message : link->early)
                vm->postNative([data = std::move(message)](VM& vm) { deliverToWorker(vm, data); });
            link->early.clear();
        }
        vm->jobStarted();  // it waits for messages until it closes or is terminated
        vm->runModule(entry);
        vm->runEventLoop(true);
    } catch (AstraThrow& thrown) {
        try {
            failure = thrown.value.isObjType(ObjType::Instance) ? vm->stringify(thrown.value) : "exception: " + vm->stringify(thrown.value);
        } catch (const AstraThrow&) {
            failure = "exception (its toString() failed)";
        }
        for (const std::string& line : thrown.trace) failure += "\n  " + line;
    } catch (const AstraExit&) {
    } catch (const WorkerStop&) {
    } catch (const std::exception& problem) {
        failure = std::string("the worker could not start: ") + problem.what();
    }

    VM* parent = nullptr;
    {
        std::lock_guard<std::mutex> hold(link->mutex);
        link->worker = nullptr;
        link->ended = true;
        parent = link->parent;
    }
    // The parent hears of the end on its loop: the worker counts for nothing any more, and
    // an error that ended it is handed to onError, or reported.
    if (parent != nullptr) {
        parent->postNative([link, failure](VM& vm) {
            vm.workerEnded();
            if (!link->daemon) vm.jobFinished();
            const auto object = vm.workerObjects.find(link->id);
            const Value worker = object == vm.workerObjects.end() ? Value::null() : object->second;
            TempRoot keep(vm, worker);
            vm.workerObjects.erase(link->id);
            vm.workerLinks.erase(link->id);
            if (failure.empty()) return;
            const Value* handler = worker.isObjType(ObjType::Instance) ? asInstance(worker)->field(vm.intern("onError")) : nullptr;
            if (handler != nullptr && !handler->isNull()) {
                Value text = vm.str(failure);
                TempRoot keepText(vm, text);
                vm.call(*handler, 1, &text);
            } else {
                vm.output->flush();
                std::cerr << "Uncaught " << failure << "\n  in worker " << link->path << "\n";
            }
        });
    }
    if (!vm->threadsAlive()) {
        delete vm;
        delete program;
    }
}

std::shared_ptr<WorkerLink> linkOf(VM& vm, Value id) {
    const auto found = id.isInt() ? vm.workerLinks.find(id.as.i) : vm.workerLinks.end();
    return found == vm.workerLinks.end() ? nullptr : found->second;
}

}  // namespace

void registerWorkers(VM& vm) {
    ObjClass* worker = vm.alloc<ObjClass>(vm.intern("worker"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        worker->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("worker.") + name, minArgs, maxArgs, fn)));
    };

    // ---- For the program that starts a worker ----

    // start(script, object, daemon): `script` is what the compiler made of the path written in
    // Worker("..."): the number of the module the script became, and the path.
    def("start", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const std::string spec = args[0].isString() ? asString(args[0])->utf8() : std::string();
        const size_t second = spec.size() > 1 && spec[0] == '\x01' ? spec.find('\x01', 1) : std::string::npos;
        if (second == std::string::npos)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a worker's script is named by a string written in the call itself, as in Worker(\"physics.astra\"), "
                                  "so that the script is built into the program");
        if (!vm.programImage) throw vm.runtimeError(Thrown::IllegalStateException, "workers cannot be started here");

        auto link = std::make_shared<WorkerLink>();
        link->parent = &vm;
        link->id = vm.nextWorkerId++;
        link->daemon = args[2].truthy();
        link->path = spec.substr(second + 1);
        vm.workerLinks[link->id] = link;
        vm.workerObjects[link->id] = args[1];
        vm.workerStarted();
        if (!link->daemon) vm.jobStarted();  // the program waits for a worker that is not a daemon

        auto image = std::make_shared<const std::string>(vm.programImage());
        std::thread(workerMain, link, image, std::stoi(spec.substr(1, second - 1)), vm.scriptArgs, vm.onThreadError).detach();
        return Value::integer(link->id);
    });

    def("path", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto link = linkOf(vm, args[0]);
        return link == nullptr ? Value::null() : vm.str(link->path);
    });

    def("post", 2, 3, [](VM& vm, int argc, Value* args) -> Value {
        auto link = linkOf(vm, args[0]);
        std::string data;
        if (argc > 2 && args[2].truthy()) handOver(vm, args[1], data);
        else encode(vm, args[1], data, 0);
        if (link == nullptr) return Value::boolean(false);  // it has ended
        std::lock_guard<std::mutex> hold(link->mutex);
        if (link->ended) return Value::boolean(false);
        if (link->worker == nullptr) link->early.push_back(std::move(data));
        else link->worker->postNative([data = std::move(data)](VM& vm) { deliverToWorker(vm, data); });
        return Value::boolean(true);
    });

    // Stops the worker at its next call or loop round, whatever it is doing.
    def("terminate", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto link = linkOf(vm, args[0]);
        if (link == nullptr) return Value::null();
        std::lock_guard<std::mutex> hold(link->mutex);
        if (link->worker != nullptr) link->worker->requestStop();
        return Value::null();
    });

    def("isAlive", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::boolean(linkOf(vm, args[0]) != nullptr); });

    // ---- For the worker itself ----

    def("inWorker", 0, 0, [](VM& vm, int, Value*) -> Value { return Value::boolean(vm.isWorker()); });

    // The object whose onMessage gets what the parent sends.
    def("attach", 1, 1, [](VM& vm, int, Value* args) -> Value {
        vm.workerSelf = args[0];
        return Value::null();
    });

    def("send", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        std::shared_ptr<WorkerLink> link = vm.workerLink;
        if (link == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "this program is not a worker: it has no parent to send to");
        std::string data;
        if (argc > 1 && args[1].truthy()) handOver(vm, args[0], data);
        else encode(vm, args[0], data, 0);
        std::lock_guard<std::mutex> hold(link->mutex);
        if (link->parent == nullptr) return Value::null();
        link->parent->postNative([link, data = std::move(data)](VM& vm) {
            const auto object = vm.workerObjects.find(link->id);
            if (object == vm.workerObjects.end()) return;
            Reader in{data.data(), data.data() + data.size()};
            callField(vm, object->second, "onMessage", decode(vm, in));
        });
        return Value::null();
    });

    // The worker has nothing more to wait for: it ends when what it is doing is done.
    def("close", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (vm.isWorker() && !vm.workerSelf.isNull()) {
            vm.workerSelf = Value::null();
            vm.jobFinished();
        }
        return Value::null();
    });

    vm.defineGlobal("__worker", Value::object(worker));
}

// ---- A VM's holds on shared blocks ----

void VM::noteOutside(const Obj* obj) {
    std::lock_guard<std::mutex> hold(blocksMutex);
    auto found = blockOf.find(obj);
    if (found != blockOf.end()) reachedBlocks.insert(found->second);
}

void VM::releaseUnreachedBlocks() {
    if (heldBlocks.empty()) return;
    // What this VM's own frozen data was shared as stays held while the data is kept.
    for (const auto& [own, block] : sharedAs) reachedBlocks.insert(block);
    std::vector<SharedBlock*> gone;
    for (SharedBlock* block : heldBlocks)
        if (reachedBlocks.count(block) == 0) gone.push_back(block);
    for (SharedBlock* block : gone) {
        heldBlocks.erase(block);
        letGo(block);
    }
    reachedBlocks.clear();
}

void VM::releaseAllBlocks() {
    for (SharedBlock* block : heldBlocks) letGo(block);
    heldBlocks.clear();
    sharedAs.clear();
}

}  // namespace astra
