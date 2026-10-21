// Threads, locks, the event loop and flows: everything that has more than one line of
// execution in it.
//
// Threads are real operating-system threads that share one heap. Only one of them runs Astra
// code at a time, the one holding the VM's lock, so a program can get a wrong answer from a
// race but cannot damage the VM. A thread gives the lock up at a safe point (a call or the
// end of a loop body) once another has waited for it, and around anything that blocks.
//
// A flow is a function with a stack of its own (ObjFiber), which lets it stop in the middle
// of a call and go on later. It needs no thread: it runs on whichever thread takes it up.
//
// The Astra side of all this is in src/std/thread.astra and src/std/flow.astra.

#include <algorithm>
#include <cstdlib>
#include <new>
#include <thread>

#include "builtins.h"
#include "vm.h"

namespace astra {

using Clock = std::chrono::steady_clock;

// ---- Contexts ----

Context::Context(size_t stackSize, int frameLimit) : maxFrames(frameLimit) {
    stack = static_cast<Value*>(std::malloc(stackSize * sizeof(Value)));
    // Each frame's type arguments come after the frames (VM::frameTypeArgs).
    static_assert(sizeof(CallFrame) % alignof(const ObjTypeInfo*) == 0);
    // and their destinations (VM::frameDestination) after those.
    frames = static_cast<CallFrame*>(
        std::malloc(static_cast<size_t>(frameLimit) * (sizeof(CallFrame) + sizeof(const ObjTypeInfo*) + sizeof(Value))));
    if (stack == nullptr || frames == nullptr) {
        std::free(stack);
        std::free(frames);
        throw std::bad_alloc();
    }
    stackEnd = stack + stackSize;
    sp = stack;
}

Context::~Context() { release(); }

void Context::release() {
    std::free(stack);
    std::free(frames);
    stack = stackEnd = sp = nullptr;
    frames = nullptr;
    frameCount = 0;
    handlers.clear();
    openUpvalues = nullptr;
}

// Puts the running context's state, which the VM keeps in members of its own, back into it.
void VM::saveContext() {
    Context& c = *context_;
    c.sp = sp_;
    c.frameCount = frameCount_;
    c.handlers = std::move(handlers_);
    handlers_.clear();
    c.openUpvalues = openUpvalues_;
    c.nativeDepth = nativeDepth_;
    c.nativeBase = nativeBase_;
}

void VM::loadContext(Context* c) {
    context_ = c;
    stack_ = c->stack;
    stackEnd_ = c->stackEnd;
    sp_ = c->sp;
    frames_ = c->frames;
    frameCount_ = c->frameCount;
    maxFrames_ = c->maxFrames;
    handlers_ = std::move(c->handlers);
    c->handlers.clear();
    openUpvalues_ = c->openUpvalues;
    nativeDepth_ = c->nativeDepth;
    nativeBase_ = c->nativeBase;
}

// ---- The lock ----

void VM::releaseLock() {
    ThreadState* me = thread_;
    saveContext();
    me->current = context_;
    me->tempRoots.swap(tempRoots);
    me->tempVectors.swap(tempVectors);
    tempRoots.clear();
    tempVectors.clear();
    {
        std::lock_guard<std::mutex> hold(lockMutex_);
        lockHeld_ = false;
    }
    lockFree_.notify_one();
}

void VM::acquireLock(ThreadState* me) {
    {
        std::unique_lock<std::mutex> hold(lockMutex_);
        lockWaiters_++;
        while (lockHeld_) {
            // Having waited this long, ask the thread that has it to give way.
            if (lockFree_.wait_for(hold, std::chrono::milliseconds(5)) == std::cv_status::timeout && lockHeld_) attention_ = true;
        }
        lockWaiters_--;
        lockHeld_ = true;
        lockSwitches_++;
    }
    lockTaken_.notify_all();

    thread_ = me;
    tempRoots.swap(me->tempRoots);
    tempVectors.swap(me->tempVectors);
    loadContext(me->current);
    // Under the stress setting every safe point is a place to change threads.
    attention_ = stopRequested_.load() || (threadStress_ && threads_.size() > 1);
}

// Lets another thread have the lock, and takes it back afterwards.
void VM::yieldLock() {
    ThreadState* me = thread_;
    saveContext();
    me->current = context_;
    me->tempRoots.swap(tempRoots);
    me->tempVectors.swap(tempVectors);
    tempRoots.clear();
    tempVectors.clear();
    {
        std::unique_lock<std::mutex> hold(lockMutex_);
        const uint64_t before = lockSwitches_;
        lockHeld_ = false;
        lockFree_.notify_one();
        // Wait until another thread has had it, so that this one does not take it straight back.
        if (lockWaiters_ > 0) lockTaken_.wait_for(hold, std::chrono::milliseconds(50), [&] { return lockSwitches_ != before; });
    }
    acquireLock(me);
}

void VM::safePoint() {
    if (stopRequested_.load(std::memory_order_relaxed)) throw WorkerStop{};
    if (timeLimitPassed_.load(std::memory_order_relaxed) && keyCalls_ == 0) {
        timeLimitPassed_ = false;
        throw runtimeError(Thrown::AssertionError, "the test ran longer than its time limit, " + std::to_string(timeLimitMillis_.load()) + " ms");
    }
    if (keyCalls_ > 0) return;  // inside a key's == or hashCode (D26)
    if (bytesAllocated_ > nextGC_) collectGarbage();
    if (attention_.load(std::memory_order_relaxed)) {
        if (lockWaiters_.load() > 0) yieldLock();
        else if (!threadStress_) attention_ = false;
    }
}

void VM::requestStop() {
    stopRequested_ = true;
    attention_ = true;
    wakeLoop();
}

int64_t VM::millisUntilLoopWork() {
    {
        std::lock_guard<std::mutex> hold(loopMutex_);
        if (!nativeTasks_.empty()) return 0;
    }
    if (!loopTasks_.empty()) return 0;
    if (timers_.empty()) return -1;
    Clock::time_point due = timers_[0].due;
    for (const Timer& timer : timers_) due = std::min(due, timer.due);
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(due - Clock::now()).count();
    return left < 0 ? 0 : left + 1;
}

void VM::park(int64_t timeoutMs) {
    ThreadState* me = thread_;
    releaseLock();
    {
        std::unique_lock<std::mutex> hold(me->parkMutex);
        auto ready = [&] { return me->parkToken; };
        if (timeoutMs < 0) me->parkCv.wait(hold, ready);
        else me->parkCv.wait_for(hold, std::chrono::milliseconds(timeoutMs), ready);
        me->parkToken = false;
    }
    acquireLock(me);
}

void VM::unpark(ThreadState* thread) {
    {
        std::lock_guard<std::mutex> hold(thread->parkMutex);
        thread->parkToken = true;
    }
    thread->parkCv.notify_one();
}

// ---- Threads ----

int64_t VM::startThread(Value function, Value self, const std::string& name, bool daemon) {
    ThreadState* thread = new ThreadState(kThreadStackSize, kThreadMaxFrames);
    thread->id = nextThreadId_++;
    thread->name = name.empty() ? "Thread-" + std::to_string(thread->id) : name;
    thread->daemon = daemon;
    thread->function = function;
    thread->self = self;
    threads_.push_back(thread);
    if (threadStress_) attention_ = true;
    std::thread([this, thread] { threadMain(thread); }).detach();
    return thread->id;
}

void VM::threadMain(ThreadState* me) {
    acquireLock(me);
    try {
        call(me->function, 0, nullptr);
    } catch (AstraThrow& thrown) {
        if (onThreadError) onThreadError(*this, thrown, me->name);
    } catch (const AstraExit& exit) {
        if (onThreadExit) onThreadExit(exit.code);
    } catch (const FiberSuspend&) {
        // Cannot happen: a thread's own context is not a flow.
    } catch (const WorkerStop&) {
        // The worker this thread belongs to has been told to stop.
    }

    // The thread is over. Those waiting for it are woken, and the lock is given up for good.
    me->finished = true;
    threads_.erase(std::find(threads_.begin(), threads_.end(), me));
    for (ThreadState* joiner : me->joiners) unpark(joiner);
    for (auto it = monitors_.begin(); it != monitors_.end();) {
        // A lock it still held is let go, so that the others are not left waiting for ever.
        Monitor& monitor = it->second;
        if (monitor.owner == me) {
            monitor.owner = nullptr;
            monitor.count = 0;
            if (!monitor.entering.empty()) unpark(monitor.entering.front());
            else if (monitor.waiting.empty()) {
                it = monitors_.erase(it);
                continue;
            }
        }
        ++it;
    }
    closeUpvalues(stack_);
    handlers_.clear();
    tempRoots.clear();
    tempVectors.clear();
    {
        std::lock_guard<std::mutex> hold(lockMutex_);
        lockHeld_ = false;
    }
    lockFree_.notify_one();
    wakeLoop();
    delete me;
}

ThreadState* VM::findThread(int64_t id) const {
    for (ThreadState* thread : threads_)
        if (thread->id == id) return thread;
    return nullptr;
}

namespace {

// How long is left of a time limit; negative for no limit.
struct Deadline {
    bool limited;
    Clock::time_point at;
    explicit Deadline(int64_t timeoutMs)
        : limited(timeoutMs >= 0), at(Clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs)) {}
    bool passed() const { return limited && Clock::now() >= at; }
    int64_t left() const {
        if (!limited) return -1;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(at - Clock::now()).count();
        return ms < 1 ? 1 : ms;
    }
};

void removeFrom(std::vector<ThreadState*>& list, ThreadState* thread) {
    list.erase(std::remove(list.begin(), list.end(), thread), list.end());
}

}  // namespace

bool VM::joinThread(int64_t id, int64_t timeoutMs) {
    ThreadState* me = thread_;
    const Deadline deadline(timeoutMs);
    for (;;) {
        ThreadState* target = findThread(id);
        if (target == nullptr) return true;
        if (target == me) throw runtimeError(Thrown::IllegalStateException, "a thread cannot wait for itself to finish");
        if (deadline.passed()) return false;
        target->joiners.push_back(me);
        park(deadline.left());
        target = findThread(id);  // it may be gone, and with it the list
        if (target != nullptr) removeFrom(target->joiners, me);
        if (me->interrupted.load()) return target == nullptr;
    }
}

void VM::interruptThread(int64_t id) {
    ThreadState* thread = findThread(id);
    if (thread == nullptr) return;
    thread->interrupted = true;
    unpark(thread);
}

// ---- Monitors: any object as a lock ----

VM::Monitor& VM::heldMonitor(Value lock, const char* what) {
    auto found = lock.isObj() ? monitors_.find(lock.as.obj) : monitors_.end();
    if (found == monitors_.end() || found->second.owner != thread_)
        throw runtimeError(Thrown::IllegalStateException, std::string(what) + ": this thread does not hold the lock; it is held inside synchronized(lock) { ... }");
    return found->second;
}

bool VM::monitorHeld(Value lock) const {
    auto found = lock.isObj() ? monitors_.find(lock.as.obj) : monitors_.end();
    return found != monitors_.end() && found->second.owner == thread_;
}

bool VM::monitorTryEnter(Value lock) {
    if (!lock.isObj()) throw runtimeError(Thrown::ClassCastException, "a lock is an object, not a value of type " + typeName(lock));
    Monitor& monitor = monitors_[lock.as.obj];
    if (monitor.owner == thread_) {
        monitor.count++;
        return true;
    }
    if (monitor.owner != nullptr) return false;
    monitor.owner = thread_;
    monitor.count = 1;
    return true;
}

void VM::monitorEnter(Value lock) {
    if (monitorTryEnter(lock)) return;
    ThreadState* me = thread_;
    const Obj* key = lock.as.obj;
    // An entry is never removed while a thread is listed in it, and the map does not move its
    // entries, so the reference stays good across the waits.
    Monitor& monitor = monitors_[key];
    monitor.entering.push_back(me);
    while (monitor.owner != nullptr) park(-1);
    removeFrom(monitor.entering, me);
    monitor.owner = me;
    monitor.count = 1;
}

void VM::monitorExit(Value lock) {
    Monitor& monitor = heldMonitor(lock, "unlock");
    if (--monitor.count > 0) return;
    monitor.owner = nullptr;
    if (!monitor.entering.empty()) unpark(monitor.entering.front());
    else if (monitor.waiting.empty()) monitors_.erase(lock.as.obj);
}

int VM::monitorWait(Value lock, int64_t timeoutMs) {
    Monitor& monitor = heldMonitor(lock, "wait");
    ThreadState* me = thread_;
    if (me->interrupted.exchange(false)) return 2;

    // Let the lock go altogether, however many times over it is held, and wait to be told.
    const int held = monitor.count;
    monitor.owner = nullptr;
    monitor.count = 0;
    if (!monitor.entering.empty()) unpark(monitor.entering.front());
    me->notified = false;
    monitor.waiting.push_back(me);

    int status = 0;
    const Deadline deadline(timeoutMs);
    for (;;) {
        if (me->notified) break;
        if (me->interrupted.exchange(false)) {
            status = 2;
            break;
        }
        if (deadline.passed()) {
            status = 1;
            break;
        }
        park(deadline.left());
    }
    removeFrom(monitor.waiting, me);

    // Take the lock back before going on, as it was held. (A thread that was notified is
    // already listed as entering: see monitorNotify.)
    if (std::find(monitor.entering.begin(), monitor.entering.end(), me) == monitor.entering.end()) monitor.entering.push_back(me);
    while (monitor.owner != nullptr) park(-1);
    removeFrom(monitor.entering, me);
    monitor.owner = me;
    monitor.count = held;
    return status;
}

void VM::monitorNotify(Value lock, bool all) {
    Monitor& monitor = heldMonitor(lock, "notify");
    while (!monitor.waiting.empty()) {
        ThreadState* thread = monitor.waiting.front();
        monitor.waiting.erase(monitor.waiting.begin());
        // It has the lock to take back, so it counts as entering from here on. That also keeps
        // the monitor in being until it has: one that nobody is listed in is thrown away.
        monitor.entering.push_back(thread);
        thread->notified = true;
        unpark(thread);
        if (!all) break;
    }
}

// ---- The event loop ----

void VM::wakeLoop() {
    {
        std::lock_guard<std::mutex> hold(loopMutex_);
        loopWake_ = true;
    }
    loopCv_.notify_all();
    if (auto hook = onLoopWake.load()) hook();
}

void VM::post(Value task) {
    loopTasks_.push_back(task);
    wakeLoop();
}

void VM::postNative(std::function<void(VM&)> work) {
    {
        std::lock_guard<std::mutex> hold(loopMutex_);
        nativeTasks_.push_back(std::move(work));
        loopWake_ = true;
    }
    loopCv_.notify_all();
    if (auto hook = onLoopWake.load()) hook();
}

int64_t VM::addTimer(Value task, int64_t delayMs, int64_t periodMs) {
    Timer timer;
    timer.id = nextTimerId_++;
    timer.due = Clock::now() + std::chrono::milliseconds(delayMs < 0 ? 0 : delayMs);
    timer.periodMs = periodMs;
    timer.task = task;
    timers_.push_back(timer);
    wakeLoop();
    return timer.id;
}

void VM::cancelTimer(int64_t id) {
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(), [&](const Timer& timer) { return timer.id == id; }), timers_.end());
    wakeLoop();
}

void VM::runEventLoop(bool wait, int64_t budgetMs) {
    if (thread_ != &mainThread_) throw runtimeError(Thrown::IllegalStateException, "the event loop runs on the main thread only");
    // Without waiting, and with a budget: what is ready is run until the time is up, and one
    // thing at least. A timer whose task takes longer than its period is always due, so
    // "until nothing is due" alone would never come back.
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max<int64_t>(budgetMs, 0));
    bool ranOne = false;
    for (;;) {
        if (stopRequested_.load()) throw WorkerStop{};
        if (!wait && budgetMs >= 0 && ranOne && Clock::now() >= deadline) return;
        ranOne = true;
        // What a worker thread has handed over: the end of a read, say, with a flow to take up.
        std::function<void(VM&)> native;
        {
            std::lock_guard<std::mutex> hold(loopMutex_);
            if (!nativeTasks_.empty()) {
                native = std::move(nativeTasks_.front());
                nativeTasks_.pop_front();
            }
        }
        if (native) {
            native(*this);
            continue;
        }

        // Windows' events: all that are waiting, then one of the loop's own tasks, in turn.
        if (loopPump != nullptr) loopPump(*this);

        if (!loopTasks_.empty()) {
            TempRoot task(*this, loopTasks_.front());
            loopTasks_.pop_front();
            call(task.get(), 0, nullptr);
            continue;
        }

        size_t next = timers_.size();
        for (size_t i = 0; i < timers_.size(); i++)
            if (next == timers_.size() || timers_[i].due < timers_[next].due) next = i;
        const bool haveTimer = next < timers_.size();
        const Clock::time_point due = haveTimer ? timers_[next].due : Clock::time_point();
        if (haveTimer && due <= Clock::now()) {
            TempRoot task(*this, timers_[next].task);
            if (timers_[next].periodMs > 0) timers_[next].due = Clock::now() + std::chrono::milliseconds(timers_[next].periodMs);
            else timers_.erase(timers_.begin() + static_cast<std::ptrdiff_t>(next));
            call(task.get(), 0, nullptr);
            continue;
        }

        if (!wait) return;
        bool working = false;  // a thread that is not a daemon keeps the program going
        for (const ThreadState* thread : threads_) working = working || (thread != &mainThread_ && !thread->daemon);
        if (!haveTimer && pendingJobs_ == 0 && !working) return;

        ThreadState* me = thread_;
        if (loopWait != nullptr) {
            // With windows, the wait is for the window system: an event ends it, and so does
            // onLoopWake, which whoever gives the loop work calls.
            int64_t millis = -1;
            if (haveTimer) millis = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(due - Clock::now()).count() + 1);
            bool waiting = true;
            {
                std::lock_guard<std::mutex> hold(loopMutex_);
                if (loopWake_ || !nativeTasks_.empty()) waiting = false;
                loopWake_ = false;
            }
            if (waiting) {
                releaseLock();
                loopWait(millis);
                acquireLock(me);
            }
            continue;
        }
        releaseLock();
        {
            std::unique_lock<std::mutex> hold(loopMutex_);
            auto woken = [&] { return loopWake_; };
            if (haveTimer) loopCv_.wait_until(hold, due, woken);
            else loopCv_.wait(hold, woken);
            loopWake_ = false;
        }
        acquireLock(me);
    }
}

// ---- Worker threads, for work that blocks ----

// Shared with the worker threads themselves, so that one still busy when the program ends
// does not find this gone.
struct VM::Workers {
    struct Shared {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<std::function<void()>> queue;
        int idle = 0;
        int busy = 0;
        bool closing = false;
    };
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();

    ~Workers() {
        {
            std::lock_guard<std::mutex> hold(shared->mutex);
            shared->closing = true;
        }
        shared->ready.notify_all();
    }

    void submit(std::function<void()> job) {
        std::unique_lock<std::mutex> hold(shared->mutex);
        shared->queue.push_back(std::move(job));
        if (shared->idle > 0) {
            shared->ready.notify_one();
            return;
        }
        // Nobody is free: one more worker. Each gives up after a while with nothing to do.
        std::thread([state = shared] {
            std::unique_lock<std::mutex> lock(state->mutex);
            for (;;) {
                while (state->queue.empty()) {
                    if (state->closing) return;
                    state->idle++;
                    const bool timedOut = state->ready.wait_for(lock, std::chrono::seconds(10)) == std::cv_status::timeout;
                    state->idle--;
                    if (timedOut && state->queue.empty()) return;
                }
                std::function<void()> work = std::move(state->queue.front());
                state->queue.pop_front();
                state->busy++;
                lock.unlock();
                work();
                work = nullptr;
                lock.lock();
                state->busy--;
            }
        }).detach();
    }
};

Value VM::blocking(std::function<void()> work, std::function<Value(VM&)> finish) {
    if (inParallelSection()) throw runtimeError(Thrown::IllegalStateException, "a parallel section does not wait: reading, writing and other waiting is done outside it");
    if (canSuspend() && onLoopThread()) {
        // In a flow on the loop thread: a worker does the waiting, and the flow is set aside
        // until the loop hears that the work is done.
        ObjFiber* fiber = context_->fiber;
        if (workers_ == nullptr) workers_ = std::make_shared<Workers>();
        waitingFibers_.push_back(fiber);
        pendingJobs_++;
        workers_->submit([this, fiber, work = std::move(work), finish = std::move(finish)]() mutable {
            work();
            postNative([fiber, finish = std::move(finish)](VM& vm) {
                vm.pendingJobs_--;
                vm.waitingFibers_.erase(std::find(vm.waitingFibers_.begin(), vm.waitingFibers_.end(), fiber));
                bool ok = true;
                Value result;
                try {
                    result = finish(vm);
                } catch (AstraThrow& thrown) {
                    ok = false;
                    result = thrown.value;
                }
                vm.runFiber(fiber, ok, result);
            });
        });
        fiber->waitingAt = 0;
        suspendFiber();
    }
    {
        LockReleased released(*this);
        work();
    }
    return finish(*this);
}

// ---- Flows ----

void VM::suspendFiber() {
    context_->fiber->pendingSlot = nativeBase_;
    throw FiberSuspend{};
}

void VM::runFiber(ObjFiber* fiber, bool ok, Value value) {
    if (fiber->state == FiberState::Running) throw runtimeError(Thrown::IllegalStateException, "the flow is already running");
    if (fiber->state == FiberState::Done) throw runtimeError(Thrown::IllegalStateException, "the flow has already finished");
    if (thread_->nesting >= kMaxFlowNesting) throw runtimeError(Thrown::IllegalStateException, "too many flows are running inside one another");
    TempRoot keepFiber(*this, Value::object(fiber));
    TempRoot keepValue(*this, value);

    Context* const outer = context_;
    const bool fresh = fiber->state == FiberState::Fresh;
    saveContext();
    fiber->context.parent = outer;
    fiber->state = FiberState::Running;
    loadContext(&fiber->context);
    thread_->current = context_;
    thread_->nesting++;

    // Back to the context that took the flow up, whichever way the flow stopped.
    auto leave = [&](FiberState state) {
        fiber->state = state;
        if (state == FiberState::Done) closeUpvalues(stack_);
        saveContext();
        fiber->context.parent = nullptr;
        if (state == FiberState::Done) fiber->context.release();
        loadContext(outer);
        thread_->current = outer;
        thread_->nesting--;
    };

    try {
        if (fresh) {
            *sp_++ = fiber->entry;
            if (callValue(fiber->entry, 0)) run(0);
        } else {
            // The call it was waiting in is over: its arguments go, and its result, or the
            // error it raises, takes their place.
            sp_ = fiber->pendingSlot;
            fiber->pendingSlot = nullptr;
            if (ok) {
                *sp_++ = value;
            } else {
                AstraThrow thrown;
                thrown.value = value;
                if (handlers_.empty()) {
                    thrown.trace = captureTrace();
                    thrown.hasTrace = true;
                }
                if (!tryHandle(value, 0)) {
                    unwindTo(0);
                    throw thrown;
                }
            }
            run(0);
        }
        leave(FiberState::Done);
    } catch (const FiberSuspend&) {
        leave(FiberState::Suspended);
    } catch (...) {
        leave(FiberState::Done);
        throw;
    }
}

// ---- What the standard library is written on ----

namespace {

int64_t timeLimit(VM& vm, int argc, const Value* args, int i) {
    if (i >= argc || args[i].isNull()) return -1;
    const double ms = vm.toNumber(args[i], "time in milliseconds");
    return ms < 0 ? 0 : static_cast<int64_t>(ms);
}

ObjFiber* fiberArgument(VM& vm, Value v) {
    if (!v.isObjType(ObjType::Fiber)) throw vm.runtimeError(Thrown::ClassCastException, "expected a flow, got a value of type " + vm.typeName(v));
    return asFiber(v);
}

ObjClass* holder(VM& vm, const char* name) {
    ObjClass* made = vm.alloc<ObjClass>(vm.intern(name));
    vm.defineGlobal(std::string("__") + name, Value::object(made));
    return made;
}

void define(VM& vm, ObjClass* on, const char* name, int minArgs, int maxArgs, NativeFn fn) {
    on->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(on->name->utf8() + "." + name, minArgs, maxArgs, fn)));
}

}  // namespace

void registerThreads(VM& vm) {
    // -- __thread: threads and locks --
    ObjClass* thread = holder(vm, "thread");

    define(vm, thread, "start", 4, 4, [](VM& vm, int, Value* args) -> Value {
        return Value::integer(vm.startThread(args[0], args[1], args[2].isNull() ? std::string() : vm.toStr(args[2], "thread name"), args[3].truthy()));
    });
    // 0: it has finished; 1: the time ran out; 2: this thread was interrupted while waiting.
    define(vm, thread, "join", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        ThreadState* me = vm.currentThread();
        if (me->interrupted.exchange(false)) return Value::integer(2);
        const bool done = vm.joinThread(vm.toInt(args[0], "thread"), timeLimit(vm, argc, args, 1));
        if (!done && me->interrupted.exchange(false)) return Value::integer(2);
        return Value::integer(done ? 0 : 1);
    });
    // False if the thread was interrupted before the time was up.
    define(vm, thread, "sleep", 1, 1, [](VM& vm, int argc, Value* args) -> Value {
        ThreadState* me = vm.currentThread();
        const Deadline deadline(timeLimit(vm, argc, args, 0));
        for (;;) {
            if (me->interrupted.exchange(false)) return Value::boolean(false);
            if (deadline.passed()) return Value::boolean(true);
            vm.park(deadline.left());
        }
    });
    define(vm, thread, "yield", 0, 0, [](VM& vm, int, Value*) -> Value {
        ThreadState* me = vm.currentThread();
        vm.releaseLock();
        std::this_thread::yield();
        vm.acquireLock(me);
        return Value::null();
    });
    define(vm, thread, "currentId", 0, 0, [](VM& vm, int, Value*) -> Value { return Value::integer(vm.currentThread()->id); });
    define(vm, thread, "currentName", 0, 0, [](VM& vm, int, Value*) -> Value { return vm.str(vm.currentThread()->name); });
    // The Thread object of the running thread, and a way to give the main thread one.
    define(vm, thread, "current", 0, 0, [](VM& vm, int, Value*) -> Value { return vm.currentThread()->self; });
    define(vm, thread, "setCurrent", 1, 1, [](VM& vm, int, Value* args) -> Value {
        vm.currentThread()->self = args[0];
        return Value::null();
    });
    define(vm, thread, "isAlive", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return Value::boolean(vm.findThread(vm.toInt(args[0], "thread")) != nullptr);
    });
    define(vm, thread, "interrupt", 1, 1, [](VM& vm, int, Value* args) -> Value {
        vm.interruptThread(vm.toInt(args[0], "thread"));
        return Value::null();
    });
    define(vm, thread, "isInterrupted", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const ThreadState* target = vm.findThread(vm.toInt(args[0], "thread"));
        return Value::boolean(target != nullptr && target->interrupted.load());
    });
    // Whether the running thread has been interrupted; asking clears the mark.
    define(vm, thread, "interrupted", 0, 0, [](VM& vm, int, Value*) -> Value {
        return Value::boolean(vm.currentThread()->interrupted.exchange(false));
    });

    define(vm, thread, "enter", 1, 1, [](VM& vm, int, Value* args) -> Value {
        vm.monitorEnter(args[0]);
        return Value::null();
    });
    define(vm, thread, "tryEnter", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::boolean(vm.monitorTryEnter(args[0])); });
    define(vm, thread, "exit", 1, 1, [](VM& vm, int, Value* args) -> Value {
        vm.monitorExit(args[0]);
        return Value::null();
    });
    define(vm, thread, "holds", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::boolean(vm.monitorHeld(args[0])); });
    // 0: notified; 1: the time ran out; 2: interrupted.
    define(vm, thread, "wait", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        return Value::integer(vm.monitorWait(args[0], timeLimit(vm, argc, args, 1)));
    });
    define(vm, thread, "notify", 1, 1, [](VM& vm, int, Value* args) -> Value {
        vm.monitorNotify(args[0], false);
        return Value::null();
    });
    define(vm, thread, "notifyAll", 1, 1, [](VM& vm, int, Value* args) -> Value {
        vm.monitorNotify(args[0], true);
        return Value::null();
    });

    // -- __loop: the event loop --
    ObjClass* loop = holder(vm, "loop");

    define(vm, loop, "post", 1, 1, [](VM& vm, int, Value* args) -> Value {
        vm.post(args[0]);
        return Value::null();
    });
    // A task to run once after `delay` milliseconds, or again every `period` after that.
    define(vm, loop, "timer", 2, 3, [](VM& vm, int argc, Value* args) -> Value {
        const int64_t period = argc > 2 && !args[2].isNull() ? std::max<int64_t>(timeLimit(vm, argc, args, 2), 1) : 0;
        return Value::integer(vm.addTimer(args[1], timeLimit(vm, argc, args, 0), period));
    });
    define(vm, loop, "cancel", 1, 1, [](VM& vm, int, Value* args) -> Value {
        vm.cancelTimer(vm.toInt(args[0], "timer"));
        return Value::null();
    });
    // Work is in progress that will end by posting to the loop: the loop waits for it. Each
    // hold() is matched by a release().
    define(vm, loop, "hold", 0, 0, [](VM& vm, int, Value*) -> Value {
        vm.jobStarted();
        return Value::null();
    });
    define(vm, loop, "release", 0, 0, [](VM& vm, int, Value*) -> Value {
        vm.jobFinished();
        return Value::null();
    });
    define(vm, loop, "isLoopThread", 0, 0, [](VM& vm, int, Value*) -> Value { return Value::boolean(vm.onLoopThread()); });
    define(vm, loop, "run", 0, 0, [](VM& vm, int, Value*) -> Value {
        vm.runEventLoop(true);
        return Value::null();
    });

    // -- __flow: flows --
    ObjClass* flow = holder(vm, "flow");

    // Starts a flow running `function`, and returns when it finishes or is first set aside.
    define(vm, flow, "start", 1, 1, [](VM& vm, int, Value* args) -> Value {
        ObjFiber* fiber = vm.alloc<ObjFiber>(args[0]);
        vm.account(64 * 1024);
        vm.runFiber(fiber, true, Value::null());
        return Value::null();
    });
    define(vm, flow, "current", 0, 0, [](VM& vm, int, Value*) -> Value {
        ObjFiber* fiber = vm.currentFiber();
        return fiber == nullptr ? Value::null() : Value::object(fiber);
    });
    // A new place for the running flow to wait at. The number tells its answer from one
    // meant for an earlier wait.
    define(vm, flow, "prepare", 0, 0, [](VM& vm, int, Value*) -> Value {
        ObjFiber* fiber = vm.currentFiber();
        if (fiber == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "not in a flow");
        fiber->hasEarly = false;
        fiber->early = Value::null();
        return Value::integer(++fiber->waitToken);
    });
    // Waits for the answer to that wait: the value given, or the error raised here.
    define(vm, flow, "wait", 1, 1, [](VM& vm, int, Value* args) -> Value {
        ObjFiber* fiber = vm.currentFiber();
        if (fiber == nullptr || args[0].as.i != fiber->waitToken) throw vm.runtimeError(Thrown::IllegalStateException, "not in the flow that is to wait");
        if (fiber->hasEarly) {
            // It was answered before it got here, so there is nothing to wait for.
            fiber->hasEarly = false;
            const Value answer = fiber->early;
            fiber->early = Value::null();
            if (fiber->earlyOk) return answer;
            AstraThrow thrown;
            thrown.value = answer;
            throw thrown;
        }
        if (!vm.canSuspend())
            throw vm.runtimeError(Thrown::IllegalStateException, "the flow cannot be set aside here: it is inside a built-in function that is calling back into the "
                                  "program (a sort, or a property being read); wait outside it");
        fiber->waitingAt = fiber->waitToken;
        vm.suspendFiber();
    });
    // Gives the answer to a wait: `ok` and a value, or not ok and what to throw there.
    define(vm, flow, "resume", 4, 4, [](VM& vm, int, Value* args) -> Value {
        ObjFiber* fiber = fiberArgument(vm, args[0]);
        const int64_t token = vm.toInt(args[1], "wait");
        // Waits are numbered as the flow comes to them, and answered in that order.
        if (token <= fiber->resumedToken) throw vm.runtimeError(Thrown::IllegalStateException, "this continuation has already been resumed");
        // An answer to a wait the flow gave up (an error took it elsewhere, or it has ended)
        // has nowhere to go.
        if (token != fiber->waitToken || fiber->state == FiberState::Done) return Value::null();
        fiber->resumedToken = token;
        if (fiber->state == FiberState::Suspended && fiber->waitingAt == token) {
            vm.runFiber(fiber, args[2].truthy(), args[3]);
        } else {
            // The flow has not got as far as waiting: it finds the answer there when it does.
            fiber->hasEarly = true;
            fiber->earlyOk = args[2].truthy();
            fiber->early = args[3];
        }
        return Value::null();
    });
    // How many parameters a function takes; 1 where that cannot be told.
    define(vm, flow, "arity", 1, 1, [](VM&, int, Value* args) -> Value {
        if (args[0].isObjType(ObjType::Closure)) return Value::integer(asClosure(args[0])->proto->arity);
        if (args[0].isObjType(ObjType::BoundMethod)) return Value::integer(asBoundMethod(args[0])->method->proto->arity);
        return Value::integer(1);
    });
}

}  // namespace astra
