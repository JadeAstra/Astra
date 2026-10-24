// cpudetect: a processor check in C++ alone.
//
// One thread runs a small bytecode interpreter with a garbage-collected heap, written the way
// scripting languages are, over a fixed program:
//
//   s = "string"
//   for (frame in 0 ..< 20) for (k in 0 ..< 3000000) {
//     t = s[0] + "tri" + "n" + "g"          // a new string each time, always "string"
//     if (s != t) count a WRONG result
//   }
//
// The comparison is always equal, and every operation checks the types of its operands, so a
// working computer reports no wrong results and no errors. A second process, started by this
// one, loads the same cores in bursts: 32 threads wake every 50 ms, compute for 20 ms, and sleep.
// The two share nothing.
//
//   cpudetect [--seconds N] [--cores all|p|e] [--no-bursts]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---- Cores ----

static uint64_t coreMask(const std::string& which) {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    std::vector<uint8_t> buffer(length);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &length)) return 0;
    int best = -1, worst = 256;
    for (DWORD at = 0; at < length;) {
        auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
        best = std::max(best, static_cast<int>(e->Processor.EfficiencyClass));
        worst = std::min(worst, static_cast<int>(e->Processor.EfficiencyClass));
        at += e->Size;
    }
    uint64_t mask = 0;
    for (DWORD at = 0; at < length;) {
        auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
        const int cls = e->Processor.EfficiencyClass;
        if (which == "all" || (which == "p" && cls == best) || (which == "e" && cls == worst)) mask |= e->Processor.GroupMask[0].Mask;
        at += e->Size;
    }
    return mask;
}

// ---- A crash is reported, not silent ----

static double gStart = 0;
static LONG CALLBACK onCrash(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED &&
        code != 0xC0000374 /* heap corruption */ && code != 0xC0000409 /* fail fast */)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t rip = info->ContextRecord->Rip;
    char text[512];
    int n = std::snprintf(text, sizeof text, "\n  CRASH: exception %08lX at %p", code, reinterpret_cast<void*>(rip));
    if (rip >= base && rip < base + 0x1000000) n += std::snprintf(text + n, sizeof text - n, " (cpudetect.exe+0x%llx)", static_cast<unsigned long long>(rip - base));
    if (code == EXCEPTION_ACCESS_VIOLATION)
        n += std::snprintf(text + n, sizeof text - n, ", %s address %p", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                           reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
    n += std::snprintf(text + n, sizeof text - n, "\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, static_cast<DWORD>(n), &written, nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- The load, as a second process ----

static int burstMain(int seconds) {
    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::seconds(seconds);
    std::vector<std::thread> pool;
    std::vector<double> sinks(32);
    for (int t = 0; t < 32; t++) {
        pool.emplace_back([&, t] {
            auto tick = start;
            double x = t + 1.0;
            while (tick < end) {
                tick += std::chrono::milliseconds(50);
                std::this_thread::sleep_until(tick);
                const auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
                while (std::chrono::steady_clock::now() < stop)
                    for (int k = 0; k < 2000; k++) x = std::sqrt(x * 1.0000001 + k);
            }
            sinks[t] = x;
        });
    }
    for (auto& t : pool) t.join();
    return sinks[0] > 0 ? 0 : 1;
}

// ---- Values and a collected heap ----

enum class ObjType : uint8_t { String };
struct Obj {
    Obj* next = nullptr;
    const ObjType type;
    bool marked = false;
    explicit Obj(ObjType t) : type(t) {}
    virtual ~Obj() = default;
};
struct ObjString : Obj {
    std::string text;
    uint32_t hash = 0;
    explicit ObjString(std::string s) : Obj(ObjType::String), text(std::move(s)) {
        uint32_t h = 2166136261u;
        for (unsigned char c : text) h = (h ^ c) * 16777619u;
        hash = h;
    }
};

enum class Kind : uint8_t { Null, Bool, Int, Float, Obj };
struct Value {
    Kind kind;
    uint8_t refDepth;
    uint16_t refOffset;
    uint32_t refIndex;
    union { bool b; int64_t i; double d; Obj* obj; } as;
    static Value integer(int64_t v) { Value r{}; r.kind = Kind::Int; r.as.i = v; return r; }
    static Value boolean(bool v) { Value r{}; r.kind = Kind::Bool; r.as.b = v; return r; }
    static Value object(Obj* o) { Value r{}; r.kind = Kind::Obj; r.as.obj = o; return r; }
    bool isString() const { return kind == Kind::Obj && as.obj->type == ObjType::String; }
};
static ObjString* asString(const Value& v) { return static_cast<ObjString*>(v.as.obj); }

struct CallFrame {
    const uint8_t* ip;
    Value* base;
    bool dropResult;
    uint32_t repeats;
};

struct Thrown { std::string what; };

enum Op : uint8_t {
    OP_CONST, OP_GET_LOCAL, OP_GET_INDEX, OP_BUILD_STRING, OP_EQ_LOCAL, OP_NOT, OP_JUMP_IF_FALSE, OP_RANGE_LOOP,
    OP_RANGE_NEXT, OP_POP, OP_LOOP, OP_WRONG, OP_HALT, OP_FIRST_SPARE
};

struct VM {
    Value stack[256];
    Value* stack_ = stack;
    Value* sp_ = stack;
    CallFrame frames_[8];
    int frameCount_ = 0;
    std::vector<Value> consts;
    Obj* objects_ = nullptr;
    size_t bytesAllocated_ = 0;
    size_t nextGC_ = 1 << 20;
    volatile bool attention_ = false;
    int64_t wrong = 0;

    ~VM() {
        while (objects_ != nullptr) {
            Obj* next = objects_->next;
            delete objects_;
            objects_ = next;
        }
    }

    ObjString* newString(std::string text) {
        auto* s = new ObjString(std::move(text));
        s->next = objects_;
        objects_ = s;
        bytesAllocated_ += sizeof(ObjString) + s->text.capacity();
        return s;
    }

    // Mark and sweep: the stack and the constants are the roots.
    __declspec(noinline) void collectGarbage() {
        for (Value* v = stack_; v < sp_; v++)
            if (v->kind == Kind::Obj) v->as.obj->marked = true;
        for (Value& v : consts)
            if (v.kind == Kind::Obj) v.as.obj->marked = true;
        size_t live = 0;
        Obj** link = &objects_;
        while (*link != nullptr) {
            Obj* obj = *link;
            if (obj->marked) {
                obj->marked = false;
                live += sizeof(ObjString) + static_cast<ObjString*>(obj)->text.capacity();
                link = &obj->next;
            } else {
                *link = obj->next;
                delete obj;
            }
        }
        bytesAllocated_ = live;
        nextGC_ = std::max<size_t>(live * 2, 1 << 20);
    }
    __declspec(noinline) void safePoint() {
        if (bytesAllocated_ > nextGC_) collectGarbage();
    }

    // Kept out of the loop, as an interpreter keeps its slower operations.
    __declspec(noinline) Value getIndex(Value obj, Value index) {
        if (!obj.isString()) throw Thrown{"cannot index a value that is not a string"};
        if (index.kind != Kind::Int) throw Thrown{"string index is not an int"};
        const std::string& text = asString(obj)->text;
        if (index.as.i < 0 || static_cast<size_t>(index.as.i) >= text.size()) throw Thrown{"string index out of range"};
        return Value::object(newString(std::string(1, text[static_cast<size_t>(index.as.i)])));
    }
    __declspec(noinline) Value buildString(int count) {
        std::string out;
        for (Value* v = sp_ - count; v < sp_; v++) {
            if (!v->isString()) throw Thrown{"a piece of the string is not a string"};
            out += asString(*v)->text;
        }
        return Value::object(newString(std::move(out)));
    }
    static bool equal(const Value& a, const Value& b) {
        if (a.kind != b.kind) return false;
        if (a.kind != Kind::Obj) return a.as.i == b.as.i;
        if (a.as.obj == b.as.obj) return true;
        if (!a.isString() || !b.isString()) return false;
        const ObjString* x = asString(a);
        const ObjString* y = asString(b);
        return x->hash == y->hash && x->text == y->text;
    }

    void run(const uint8_t* code);
};

static int u16(const uint8_t* at) { return at[0] | (at[1] << 8); }
static int32_t i32(const uint8_t* at) { int32_t v; std::memcpy(&v, at, 4); return v; }

// Spare operations, so the dispatch table and the function are as large as a real interpreter's.
#define SPARE(n) case OP_FIRST_SPARE + n: sp[-1].as.i = sp[-1].as.i * (n + 3) + (n ^ 0x55); sp[-2].as.d = std::sqrt(sp[-2].as.d + n) * sp[-3].as.d; sp[-3].as.i ^= sp[-1].as.i >> (n & 7); continue;
#define SPARE10(n) SPARE(n) SPARE(n + 1) SPARE(n + 2) SPARE(n + 3) SPARE(n + 4) SPARE(n + 5) SPARE(n + 6) SPARE(n + 7) SPARE(n + 8) SPARE(n + 9)

void VM::run(const uint8_t* code) {
    frames_[0] = {code, stack_, false, 0};
    frameCount_ = 1;
    CallFrame* frame = nullptr;
    const uint8_t* ip = nullptr;
    Value* base = nullptr;
    const Value* k = nullptr;
#define LOAD_FRAME() do { frame = &frames_[frameCount_ - 1]; ip = frame->ip; base = frame->base; k = consts.data(); } while (0)
    for (;;) {
        try {
            LOAD_FRAME();
            Value* sp = sp_;
            for (;;) {
                sp_ = sp;
                frame->ip = ++ip;
                switch (static_cast<Op>(ip[-1])) {
                    case OP_CONST:
                        *sp++ = k[u16(ip)];
                        ip += 2;
                        continue;
                    case OP_GET_LOCAL:
                        *sp++ = base[u16(ip)];
                        ip += 2;
                        continue;
                    case OP_GET_INDEX: {
                        const Value r = getIndex(sp_[-2], sp_[-1]);
                        sp_[-2] = r;
                        sp_--;
                        break;
                    }
                    case OP_BUILD_STRING: {
                        const int count = *ip++;
                        const Value r = buildString(count);
                        sp_ -= count;
                        *sp_++ = r;
                        break;
                    }
                    case OP_EQ_LOCAL: {
                        const Value& other = base[u16(ip)];
                        ip += 2;
                        sp[-1] = Value::boolean(equal(sp[-1], other));
                        continue;
                    }
                    case OP_NOT:
                        if (sp[-1].kind != Kind::Bool) throw Thrown{"not of something that is not a bool"};
                        sp[-1].as.b = !sp[-1].as.b;
                        continue;
                    case OP_JUMP_IF_FALSE: {
                        const int32_t target = i32(ip);
                        ip += 4;
                        const Value c = *--sp;
                        if (c.kind != Kind::Bool) throw Thrown{"a condition that is not a bool"};
                        if (!c.as.b) ip = code + target;
                        continue;
                    }
                    case OP_RANGE_LOOP: {
                        ip++;
                        const Value end = sp_[-1], start = sp_[-2];
                        if (start.kind != Kind::Int || end.kind != Kind::Int) throw Thrown{"range bounds are not ints"};
                        sp_[-1] = Value::integer(end.as.i - start.as.i);
                        *sp_++ = Value::integer(1);
                        break;
                    }
                    case OP_RANGE_NEXT: {
                        Value* const counted = base + u16(ip);  // next value, how many are left, step
                        const int32_t target = i32(ip + 2);
                        ip += 6;
                        if (counted[0].kind != Kind::Int || counted[1].kind != Kind::Int) throw Thrown{"invalid loop state"};
                        if (counted[1].as.i > 0) {
                            counted[1].as.i--;
                            *sp_++ = Value::integer(counted[0].as.i);
                            counted[0].as.i += counted[2].as.i;
                        } else {
                            ip = code + target;
                        }
                        break;
                    }
                    case OP_POP:
                        sp--;
                        continue;
                    case OP_LOOP:
                        ip = code + i32(ip);
                        if (bytesAllocated_ > nextGC_ || attention_) safePoint();
                        break;
                    case OP_WRONG:
                        wrong++;
                        break;
                    case OP_HALT:
                        return;
                    SPARE10(0) SPARE10(10) SPARE10(20) SPARE10(30) SPARE10(40) SPARE10(50) SPARE10(60) SPARE10(70) SPARE10(80) SPARE10(90)
                    SPARE10(100) SPARE10(110) SPARE10(120) SPARE10(130) SPARE10(140) SPARE10(150) SPARE10(160) SPARE10(170) SPARE10(180) SPARE10(190)
                    SPARE10(200) SPARE10(210) SPARE10(220) SPARE10(230)
                    default:
                        throw Thrown{"unknown instruction " + std::to_string(ip[-1]) + " at " + std::to_string(ip - 1 - code)};
                }
                sp = sp_;
                if (sp < stack_ || sp > stack_ + 200) throw Thrown{"stack pointer out of range"};
            }
        } catch (...) {
            throw;  // no handlers in this program: the error ends the run
        }
    }
#undef LOAD_FRAME
}

// The program above, as bytecode. Locals: 0 s, 1..3 outer loop, 4 frame, 5..7 inner loop, 8 k, 9 t.
static std::vector<uint8_t> program() {
    std::vector<uint8_t> c;
    auto op = [&](Op o) { c.push_back(o); };
    auto w = [&](int v) { c.push_back(static_cast<uint8_t>(v)); c.push_back(static_cast<uint8_t>(v >> 8)); };
    auto d = [&](int32_t v) { for (int b = 0; b < 4; b++) c.push_back(static_cast<uint8_t>(v >> (8 * b))); };
    auto patch = [&](size_t at, int32_t v) { for (int b = 0; b < 4; b++) c[at + b] = static_cast<uint8_t>(v >> (8 * b)); };
    // consts: 0 "string", 1 0, 2 20, 3 3000000, 4 "tri", 5 "n", 6 "g"
    op(OP_CONST); w(0);                                            // local 0: s
    op(OP_CONST); w(1); op(OP_CONST); w(2); op(OP_RANGE_LOOP); c.push_back(0);   // locals 1, 2, 3
    const int outer = static_cast<int>(c.size());
    op(OP_RANGE_NEXT); w(1); const size_t outerExit = c.size(); d(0);           // local 4
    op(OP_CONST); w(1); op(OP_CONST); w(3); op(OP_RANGE_LOOP); c.push_back(0);   // locals 5, 6, 7
    const int inner = static_cast<int>(c.size());
    op(OP_RANGE_NEXT); w(5); const size_t innerExit = c.size(); d(0);           // local 8
    op(OP_CONST); w(0); op(OP_CONST); w(1); op(OP_GET_INDEX);                   // s[0]
    op(OP_CONST); w(4); op(OP_CONST); w(5); op(OP_CONST); w(6);
    op(OP_BUILD_STRING); c.push_back(4);                                       // local 9: t
    op(OP_CONST); w(0); op(OP_EQ_LOCAL); w(9); op(OP_NOT);                      // s != t
    op(OP_JUMP_IF_FALSE); const size_t skip = c.size(); d(0);
    op(OP_WRONG);
    patch(skip, static_cast<int32_t>(c.size()));
    op(OP_POP);                                                                // t
    op(OP_POP);                                                                // k
    op(OP_LOOP); d(inner);
    patch(innerExit, static_cast<int32_t>(c.size()));
    op(OP_POP); op(OP_POP); op(OP_POP);                                        // the inner loop's state
    op(OP_POP);                                                                // frame
    op(OP_LOOP); d(outer);
    patch(outerExit, static_cast<int32_t>(c.size()));
    op(OP_HALT);
    return c;
}

int main(int argc, char** argv) {
    int seconds = 120;
    std::string cores = "all";
    bool bursts = true;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--burst-process" && i + 1 < argc) return burstMain(std::atoi(argv[++i]));
        if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (a == "--cores" && i + 1 < argc) cores = argv[++i];
        else if (a == "--no-bursts") bursts = false;
        else { std::printf("usage: cpudetect [--seconds N] [--cores all|p|e] [--no-bursts]\n"); return 2; }
    }
    AddVectoredExceptionHandler(1, onCrash);
    const uint64_t mask = coreMask(cores);
    if (mask == 0 || !SetProcessAffinityMask(GetCurrentProcess(), static_cast<DWORD_PTR>(mask))) { std::printf("could not keep to those cores\n"); return 2; }

    char name[256] = "";
    DWORD size = sizeof name;
    RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size);
    std::printf("cpudetect: %s\ncores: %s (mask %llX); burst load in a second process: %s; %d seconds\n\n", name, cores.c_str(),
                static_cast<unsigned long long>(mask), bursts ? "yes" : "no", seconds);
    std::fflush(stdout);

    PROCESS_INFORMATION load{};
    if (bursts) {
        char self[MAX_PATH];
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        std::string command = "\"" + std::string(self) + "\" --burst-process " + std::to_string(seconds + 5);
        STARTUPINFOA si{};
        si.cb = sizeof si;
        if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &load)) {
            std::printf("could not start the load\n");
            return 2;
        }
        SetProcessAffinityMask(load.hProcess, static_cast<DWORD_PTR>(mask));
        // The load ends with this program, even if it crashes.
        static HANDLE job = CreateJobObjectA(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        AssignProcessToJobObject(job, load.hProcess);
    }

    const std::vector<uint8_t> code = program();
    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::seconds(seconds);
    int runs = 0, failed = 0;
    while (std::chrono::steady_clock::now() < end) {
        VM vm;
        vm.consts = {Value::object(vm.newString("string")), Value::integer(0), Value::integer(20), Value::integer(3000000),
                     Value::object(vm.newString("tri")), Value::object(vm.newString("n")), Value::object(vm.newString("g"))};
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        runs++;
        try {
            vm.run(code.data());
            if (vm.wrong != 0) {
                failed++;
                std::printf("  %5.1f s  run %d: %lld WRONG results ('string' != 'string')\n", t, runs, static_cast<long long>(vm.wrong));
            } else if (vm.sp_ != vm.stack_ + 4) {
                failed++;
                std::printf("  %5.1f s  run %d: the stack ended %lld values deep, not 4\n", t, runs, static_cast<long long>(vm.sp_ - vm.stack_));
            } else {
                std::printf("  %5.1f s  run %d: ok\n", t, runs);
            }
        } catch (const Thrown& e) {
            failed++;
            std::printf("  %5.1f s  run %d: ERROR: %s\n", t, runs, e.what.c_str());
        }
        std::fflush(stdout);
    }
    if (bursts) {
        TerminateProcess(load.hProcess, 0);
        CloseHandle(load.hProcess);
        CloseHandle(load.hThread);
    }
    std::printf("\n%d of %d runs failed.\n", failed, runs);
    if (failed == 0) std::printf("Every run gave the right result.\n");
    else std::printf("The program is the same every time and shares nothing with the load, so a working computer never fails it.\n");
    return failed == 0 ? 0 : 1;
}
