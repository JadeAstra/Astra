// Streams, child processes and sockets: the built-in __io, which src/std/io.astra is
// written on.
//
// Everything here that can wait (a read, a write, an accept, waiting for a process to end)
// goes through VM::blocking. Called on a thread, it blocks that thread with the VM's lock
// released, so the other threads go on. Called in a task on the main thread, the task is
// set aside while a worker thread waits, and the event loop goes on. There is one set of
// functions for both.
//
// Windows only for now. On other systems the functions report that.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "builtins.h"
#include "charset.h"
#include "vm.h"

namespace astra {

namespace {

// A read that ran past a socket's readTimeout is a SocketTimeoutException, an IOException as in Java (D120).
AstraThrow ioError(VM& vm, const std::string& message) {
    return vm.errorOf(message == "the read timed out" ? "SocketTimeoutException" : "IOException", message);
}

#ifdef _WIN32

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

std::string narrow(const wchar_t* wide, int length = -1) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, length, utf8.data(), size, nullptr, nullptr);
    if (length < 0 && !utf8.empty() && utf8.back() == '\0') utf8.pop_back();
    return utf8;
}

// What Windows has to say about an error code.
std::string systemMessage(DWORD code) {
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                                        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string message = length > 0 ? narrow(text, static_cast<int>(length)) : "error " + std::to_string(code);
    if (text != nullptr) LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.' || message.back() == ' ')) message.pop_back();
    return message;
}

// Something that can be read from, written to, waited for or closed: a file, one end of a
// pipe, a socket, a listening socket, a process. Programs know each by a number.
struct Stream {
    enum class Kind { File, Pipe, Socket, Listener, Process };
    Kind kind;
    HANDLE handle = INVALID_HANDLE_VALUE;
    SOCKET socket = INVALID_SOCKET;
    DWORD pid = 0;

    std::atomic<bool> closed{false};
    std::atomic<int> users{0};  // threads inside a system call on it

    // Read side: what has been read from the system and not yet handed out.
    std::mutex readMutex;
    std::string buffer;
    size_t position = 0;
    bool ended = false;
    std::mutex writeMutex;

    // A process that has ended, and what it ended with.
    std::atomic<bool> exited{false};
    std::atomic<int> exitCode{0};

    explicit Stream(Kind k) : kind(k) {}

    ~Stream() { close(); }

    bool isSocket() const { return kind == Kind::Socket || kind == Kind::Listener; }

    // Closes it, first getting any thread that is waiting in it out.
    void close() {
        if (closed.exchange(true)) return;
        if (isSocket()) {
            if (socket != INVALID_SOCKET) {
                shutdown(socket, SD_BOTH);
                closesocket(socket);  // a thread in accept or recv returns with an error
                socket = INVALID_SOCKET;
            }
            return;
        }
        if (handle == INVALID_HANDLE_VALUE) return;
        while (users.load() > 0) {
            CancelIoEx(handle, nullptr);
            Sleep(1);
        }
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
};

struct Use {
    Stream& stream;
    explicit Use(Stream& s) : stream(s) { stream.users++; }
    ~Use() { stream.users--; }
};

// Reads up to `max` bytes. The count read; 0 at the end; -1 with `error` filled in.
int rawRead(Stream& s, char* out, size_t max, std::string& error) {
    Use use(s);
    if (s.closed.load()) {
        error = "the stream is closed";
        return -1;
    }
    if (s.isSocket()) {
        const int got = recv(s.socket, out, static_cast<int>(std::min<size_t>(max, 1 << 30)), 0);
        if (got >= 0) return got;
        const int code = WSAGetLastError();
        if (s.closed.load()) error = "the socket is closed";
        else if (code == WSAETIMEDOUT) error = "the read timed out";
        else if (code == WSAECONNRESET || code == WSAECONNABORTED) return 0;  // the other side went away
        else error = "cannot read from the socket: " + systemMessage(static_cast<DWORD>(code));
        return -1;
    }
    DWORD got = 0;
    if (ReadFile(s.handle, out, static_cast<DWORD>(std::min<size_t>(max, 1 << 30)), &got, nullptr)) return static_cast<int>(got);
    const DWORD code = GetLastError();
    if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) return 0;  // the writer has closed its end
    error = code == ERROR_OPERATION_ABORTED || s.closed.load() ? "the stream is closed" : "cannot read: " + systemMessage(code);
    return -1;
}

bool rawWrite(Stream& s, const char* data, size_t size, std::string& error) {
    Use use(s);
    while (size > 0) {
        if (s.closed.load()) {
            error = "the stream is closed";
            return false;
        }
        if (s.isSocket()) {
            const int sent = send(s.socket, data, static_cast<int>(std::min<size_t>(size, 1 << 30)), 0);
            if (sent == SOCKET_ERROR) {
                error = s.closed.load() ? "the socket is closed" : "cannot write to the socket: " + systemMessage(static_cast<DWORD>(WSAGetLastError()));
                return false;
            }
            data += sent;
            size -= static_cast<size_t>(sent);
        } else {
            DWORD written = 0;
            if (!WriteFile(s.handle, data, static_cast<DWORD>(std::min<size_t>(size, 1 << 30)), &written, nullptr)) {
                const DWORD code = GetLastError();
                error = code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA ? "the other end of the pipe is closed"
                        : code == ERROR_OPERATION_ABORTED || s.closed.load() ? "the stream is closed"
                        : "cannot write: " + systemMessage(code);
                return false;
            }
            data += written;
            size -= written;
        }
    }
    return true;
}

// The streams that are open, by number. One table for the whole process: a worker has a VM
// and a lock of its own, so the table has its own lock too.
std::mutex streamsMutex;
std::unordered_map<int64_t, std::shared_ptr<Stream>> streams;
int64_t nextStream = 1;

int64_t keep(std::shared_ptr<Stream> stream) {
    std::lock_guard<std::mutex> hold(streamsMutex);
    const int64_t id = nextStream++;
    streams.emplace(id, std::move(stream));
    return id;
}

// The stream of that number, or null; with `take`, it is taken out of the table as well.
std::shared_ptr<Stream> findStream(Value v, bool take = false) {
    std::lock_guard<std::mutex> hold(streamsMutex);
    const auto found = v.isInt() ? streams.find(v.as.i) : streams.end();
    if (found == streams.end()) return nullptr;
    std::shared_ptr<Stream> stream = found->second;
    if (take) streams.erase(found);
    return stream;
}

std::shared_ptr<Stream> streamOf(VM& vm, Value v) {
    std::shared_ptr<Stream> stream = findStream(v);
    if (stream == nullptr) throw ioError(vm, "the stream is closed");
    return stream;
}

Value bytesValue(VM& vm, const std::string& data) {
    ObjBytes* made = vm.alloc<ObjBytes>();
    made->data.assign(data.begin(), data.end());
    vm.account(made->data.size());
    return Value::object(made);
}

// What a blocking piece of work leaves for the function that turns it into a value.
struct Outcome {
    std::string data;
    std::string error;
    bool end = false;
    int64_t number = 0;
    std::shared_ptr<Stream> made;
};

// More from the system into the stream's buffer. False at the end or on an error.
bool fill(Stream& s, Outcome& out) {
    if (s.ended) return false;
    if (s.position > 0) {
        // What has been handed out goes, so that the buffer holds only what is still to come.
        s.buffer.erase(0, s.position);
        s.position = 0;
    }
    const size_t before = s.buffer.size();
    s.buffer.resize(before + 64 * 1024);
    const int got = rawRead(s, s.buffer.data() + before, 64 * 1024, out.error);
    s.buffer.resize(before + static_cast<size_t>(got > 0 ? got : 0));
    if (got <= 0) s.ended = got == 0;
    return got > 0;
}

// ---- Processes ----

// One argument as it has to be written on a Windows command line for the program started to
// read it back unchanged.
void appendArgument(std::wstring& line, const std::wstring& argument) {
    if (!line.empty()) line += L' ';
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += argument;
        return;
    }
    line += L'"';
    for (size_t i = 0;; i++) {
        size_t backslashes = 0;
        while (i < argument.size() && argument[i] == L'\\') {
            i++;
            backslashes++;
        }
        if (i == argument.size()) {
            line.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[i] == L'"') line.append(backslashes * 2 + 1, L'\\');
        else line.append(backslashes, L'\\');
        line += argument[i];
    }
    line += L'"';
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset() {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    HANDLE release() {
        HANDLE out = h;
        h = INVALID_HANDLE_VALUE;
        return out;
    }
};

// Where one of a new process's three standard streams goes: "pipe" (the program reads or
// writes it), "inherit" (the same as this program's), "discard", "merge" (for the error
// stream: into the output stream), or ">path" / ">>path" / "<path" for a file.
struct Redirect {
    Handle child;   // the end the new process gets
    Handle parent;  // the end this program keeps, for "pipe"
};

void openRedirect(VM& vm, const std::string& mode, bool childReads, DWORD standard, Redirect& out) {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (mode == "pipe") {
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &inheritable, 0)) throw ioError(vm, "cannot make a pipe: " + systemMessage(GetLastError()));
        out.child.h = childReads ? read : write;
        out.parent.h = childReads ? write : read;
        SetHandleInformation(out.parent.h, HANDLE_FLAG_INHERIT, 0);  // only the child's end goes to the child
    } else if (mode == "inherit") {
        HANDLE own = GetStdHandle(standard);
        if (own == nullptr || own == INVALID_HANDLE_VALUE) own = INVALID_HANDLE_VALUE;
        else if (!DuplicateHandle(GetCurrentProcess(), own, GetCurrentProcess(), &out.child.h, 0, TRUE, DUPLICATE_SAME_ACCESS)) out.child.h = INVALID_HANDLE_VALUE;
        if (out.child.h == INVALID_HANDLE_VALUE) openRedirect(vm, "discard", childReads, standard, out);  // this program has none itself
    } else if (mode == "discard") {
        out.child.h = CreateFileW(L"NUL", childReads ? GENERIC_READ : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
    } else if (!mode.empty() && (mode[0] == '>' || mode[0] == '<')) {
        const bool append = mode.compare(0, 2, ">>") == 0;
        const std::wstring path = widen(mode.substr(append ? 2 : 1));
        out.child.h = mode[0] == '<' ? CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, &inheritable, OPEN_EXISTING, 0, nullptr)
                                     : CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                                                   append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out.child.h == INVALID_HANDLE_VALUE)
            throw ioError(vm, "cannot open '" + mode.substr(append ? 2 : 1) + "' for the process: " + systemMessage(GetLastError()));
    } else {
        throw vm.runtimeError(Thrown::IllegalArgumentException, "a process's stream goes to pipe, inherit, discard, merge or a file, not '" + mode + "'");
    }
}

// The environment of this program with `changes` made to it, as CreateProcess wants it.
std::wstring environmentBlock(VM& vm, Value changes) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    if (wchar_t* own = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = own; *entry != L'\0'; entry += wcslen(entry) + 1) {
            const std::wstring text(entry);
            const size_t equals = text.find(L'=', 1);  // names such as "=C:" start with one
            if (equals != std::wstring::npos) entries.emplace_back(text.substr(0, equals), text.substr(equals + 1));
        }
        FreeEnvironmentStringsW(own);
    }
    for (const ValueMap::Entry& change : asMap(changes)->map.entries()) {
        if (!change.live) continue;
        const std::wstring name = widen(vm.toStr(change.key, "environment name"));
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) { return _wcsicmp(entry.first.c_str(), name.c_str()) == 0; }),
                      entries.end());
        if (!change.value.isNull()) entries.emplace_back(name, widen(vm.stringify(change.value)));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    std::wstring block;
    for (const auto& entry : entries) {
        block += entry.first + L'=' + entry.second;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

#endif  // _WIN32

std::vector<std::string> selfCommand;

}  // namespace

void setSelfCommand(std::vector<std::string> command) { selfCommand = std::move(command); }

#ifdef _WIN32

void registerIo(VM& vm) {
    ObjClass* io = vm.alloc<ObjClass>(vm.intern("io"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        io->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("io.") + name, minArgs, maxArgs, fn)));
    };

    // ---- Streams ----

    // open(path, mode): "r" to read, "w" to write a new file, "a" to add to the end of one.
    def("open", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::string path = vm.toStr(args[0], "file path");
        const std::string mode = vm.toStr(args[1], "mode");
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                const bool reading = mode == "r";
                HANDLE handle = CreateFileW(widen(path).c_str(), reading ? GENERIC_READ : (mode == "a" ? FILE_APPEND_DATA : GENERIC_WRITE),
                                            FILE_SHARE_READ | (reading ? FILE_SHARE_WRITE : 0), nullptr,
                                            reading ? OPEN_EXISTING : (mode == "a" ? OPEN_ALWAYS : CREATE_ALWAYS), FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle == INVALID_HANDLE_VALUE) {
                    out->error = std::string(reading ? "cannot read file '" : "cannot write file '") + path + "': " + systemMessage(GetLastError());
                    return;
                }
                out->made = std::make_shared<Stream>(Stream::Kind::File);
                out->made->handle = handle;
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, out->error);
                return Value::integer(keep(out->made));
            });
    });

    // Up to `max` bytes, at least one; null at the end.
    def("read", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const int64_t wanted = vm.toInt(args[1], "number of bytes");
        if (wanted < 1) throw vm.runtimeError(Thrown::IllegalArgumentException, "read() takes how many bytes to read at most, which is at least 1");
        const size_t max = static_cast<size_t>(std::min<int64_t>(wanted, 1 << 30));
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                std::lock_guard<std::mutex> hold(s->readMutex);
                if (s->position == s->buffer.size() && !fill(*s, *out)) {
                    out->end = out->error.empty();
                    return;
                }
                const size_t count = std::min(max, s->buffer.size() - s->position);
                out->data.assign(s->buffer, s->position, count);
                s->position += count;
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, out->error);
                return out->end ? Value::null() : bytesValue(vm, out->data);
            });
    });

    // Everything up to the end, as bytes.
    def("readAll", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                std::lock_guard<std::mutex> hold(s->readMutex);
                while (fill(*s, *out)) {}
                if (!out->error.empty()) return;
                out->data.assign(s->buffer, s->position, std::string::npos);
                s->buffer.clear();
                s->position = 0;
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, out->error);
                return bytesValue(vm, out->data);
            });
    });

    // Everything up to the end, as text in the character set (D127).
    def("readText", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const Charset cs = charsetOf(vm, argc > 1 ? args[1] : Value::null());
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                std::lock_guard<std::mutex> hold(s->readMutex);
                while (fill(*s, *out)) {}
                if (!out->error.empty()) return;
                out->data = decodeText(std::string_view(s->buffer).substr(s->position), cs);
                s->buffer.clear();
                s->position = 0;
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, out->error);
                return vm.str(std::move(out->data));
            });
    });

    // The next line, without its line end, in the character set (D127); null at the end.
    def("readLine", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const Charset cs = charsetOf(vm, argc > 1 ? args[1] : Value::null());
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                std::lock_guard<std::mutex> hold(s->readMutex);
                size_t searched = 0;  // how much of what is waiting has been looked through
                const size_t unit = cs == Charset::Utf16Le || cs == Charset::Utf16Be ? 2 : 1;
                for (;;) {
                    size_t endSize = 1;
                    const size_t at = lineEnd(s->buffer, s->position + searched, cs, endSize);
                    if (at != std::string::npos) {
                        out->data.assign(s->buffer, s->position, at - s->position);
                        s->position = at + endSize;
                        break;
                    }
                    searched = (s->buffer.size() - s->position) / unit * unit;
                    if (!fill(*s, *out)) {
                        if (!out->error.empty()) return;
                        // The end: what is left is the last line, if anything is.
                        if (s->position == s->buffer.size()) {
                            out->end = true;
                            return;
                        }
                        out->data.assign(s->buffer, s->position, std::string::npos);
                        s->position = s->buffer.size();
                        break;
                    }
                }
                out->data = decodeText(withoutCarriageReturn(std::move(out->data), cs), cs);
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, out->error);
                return out->end ? Value::null() : vm.str(out->data);
            });
    });

    // Text (in the character set, D127) or bytes. Returns when all of it has been handed to
    // the system.
    def("write", 2, 3, [](VM& vm, int argc, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        std::string data;
        if (args[1].isObjType(ObjType::Bytes)) data.assign(asBytes(args[1])->data.begin(), asBytes(args[1])->data.end());
        else data = encodeText(vm.stringify(args[1]), charsetOf(vm, argc > 2 ? args[2] : Value::null()));
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [s, out, data = std::move(data)] {
                std::lock_guard<std::mutex> hold(s->writeMutex);
                rawWrite(*s, data.data(), data.size(), out->error);
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, out->error);
                return Value::null();
            });
    });

    def("close", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<Stream> s = findStream(args[0], true);
        if (s == nullptr) return Value::null();
        // Closing may have to wait for a thread that is reading it to notice.
        return vm.blocking([s] { s->close(); }, [](VM&) -> Value { return Value::null(); });
    });

    def("isOpen", 1, 1, [](VM&, int, Value* args) -> Value {
        const std::shared_ptr<Stream> s = findStream(args[0]);
        return Value::boolean(s != nullptr && !s->closed.load());
    });

    // ---- Processes ----

    // spawn(command, directory, input, output, error, environment): starts a program. Gives
    // [process, its input, its output, its error output], the last three null unless "pipe".
    def("spawn", 6, 6, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List) || asList(args[0])->size() == 0)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a process is started from a list: the program, then its arguments");
        std::wstring commandLine;
        const std::vector<Value> parts = vm.listCopy(asList(args[0]));
        for (Value part : parts) appendArgument(commandLine, widen(vm.stringify(part)));
        const std::string program = vm.stringify(parts[0]);
        const std::wstring directory = args[1].isNull() ? std::wstring() : widen(vm.toStr(args[1], "directory"));
        const std::string errorMode = vm.toStr(args[4], "error stream");

        Redirect input, output, error;
        openRedirect(vm, vm.toStr(args[2], "input stream"), true, STD_INPUT_HANDLE, input);
        openRedirect(vm, vm.toStr(args[3], "output stream"), false, STD_OUTPUT_HANDLE, output);
        if (errorMode != "merge") openRedirect(vm, errorMode, false, STD_ERROR_HANDLE, error);
        std::wstring environment;
        if (args[5].isObjType(ObjType::Map)) environment = environmentBlock(vm, args[5]);

        // The new process is given these three handles and no others, whatever else this
        // program has open at the moment.
        HANDLE inherited[3] = {input.child.h, output.child.h, errorMode == "merge" ? output.child.h : error.child.h};
        const DWORD handleCount = inherited[2] == inherited[1] ? 2 : 3;
        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<char> attributeMemory(attributeSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeMemory.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) ||
            !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, handleCount * sizeof(HANDLE), nullptr, nullptr))
            throw ioError(vm, "cannot start '" + program + "': " + systemMessage(GetLastError()));

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = inherited[0];
        startup.StartupInfo.hStdOutput = inherited[1];
        startup.StartupInfo.hStdError = inherited[2];
        startup.lpAttributeList = attributes;

        // A program without a console of its own does not make one appear for what it starts.
        const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | (GetConsoleWindow() == nullptr ? CREATE_NO_WINDOW : 0);
        PROCESS_INFORMATION info{};
        std::vector<wchar_t> line(commandLine.begin(), commandLine.end());
        line.push_back(L'\0');
        const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, flags, environment.empty() ? nullptr : environment.data(),
                                            directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &info);
        const DWORD failure = started ? 0 : GetLastError();
        DeleteProcThreadAttributeList(attributes);
        if (!started) throw ioError(vm, "cannot start '" + program + "': " + systemMessage(failure));
        CloseHandle(info.hThread);

        auto process = std::make_shared<Stream>(Stream::Kind::Process);
        process->handle = info.hProcess;
        process->pid = info.dwProcessId;

        ObjList* result = newList(vm);
        TempRoot keepResult(vm, Value::object(result));
        result->values().push_back(Value::integer(keep(process)));
        for (Redirect* end : {&input, &output, &error}) {
            if (end->parent.h == INVALID_HANDLE_VALUE) {
                result->values().push_back(Value::null());
                continue;
            }
            auto pipe = std::make_shared<Stream>(Stream::Kind::Pipe);
            pipe->handle = end->parent.release();
            result->values().push_back(Value::integer(keep(pipe)));
        }
        return Value::object(result);
    });

    // Waits for a process to end: its exit code, or null if `timeout` ms ran out first.
    def("processWait", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const DWORD limit = argc > 1 && !args[1].isNull() ? static_cast<DWORD>(std::clamp<double>(vm.toNumber(args[1], "time limit"), 0, 4e9)) : INFINITE;
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                if (!s->exited.load()) {
                    if (s->closed.load() || WaitForSingleObject(s->handle, limit) != WAIT_OBJECT_0) {
                        out->end = true;  // still running
                        return;
                    }
                    DWORD code = 0;
                    GetExitCodeProcess(s->handle, &code);
                    s->exitCode = static_cast<int>(code);
                    s->exited = true;
                }
                out->number = s->exitCode.load();
            },
            [out](VM&) -> Value { return out->end ? Value::null() : Value::integer(out->number); });
    });

    // collect(process, output, error, charset): reads what a process writes to its output and
    // its error output, each to its end, at the same time (so that neither fills up while the
    // other is read), and waits for it to end. Gives [exit code, output, error], the text in
    // the character set; either stream may be null, giving "".
    def("collect", 4, 4, [](VM& vm, int, Value* args) -> Value {
        auto process = streamOf(vm, args[0]);
        std::shared_ptr<Stream> pipes[2] = {args[1].isNull() ? nullptr : streamOf(vm, args[1]), args[2].isNull() ? nullptr : streamOf(vm, args[2])};
        const Charset cs = charsetOf(vm, args[3]);
        auto out = std::make_shared<Outcome>();
        auto texts = std::make_shared<std::array<std::string, 2>>();
        return vm.blocking(
            [=] {
                std::string errors[2];
                auto drain = [&](int k) {
                    if (pipes[k] == nullptr) return;
                    Stream& s = *pipes[k];
                    std::lock_guard<std::mutex> hold(s.readMutex);
                    Outcome mine;
                    while (fill(s, mine)) {}
                    errors[k] = mine.error;
                    (*texts)[k] = decodeText(std::string_view(s.buffer).substr(s.position), cs);
                    s.buffer.clear();
                    s.position = 0;
                };
                std::thread second(drain, 1);
                drain(0);
                second.join();
                out->error = !errors[0].empty() ? errors[0] : errors[1];
                if (!process->exited.load()) {
                    WaitForSingleObject(process->handle, INFINITE);
                    DWORD code = 0;
                    GetExitCodeProcess(process->handle, &code);
                    process->exitCode = static_cast<int>(code);
                    process->exited = true;
                }
                out->number = process->exitCode.load();
            },
            [out, texts](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, out->error);
                ObjList* result = newList(vm);
                TempRoot keepResult(vm, Value::object(result));
                result->values().push_back(Value::integer(out->number));
                for (std::string& text : *texts) result->values().push_back(vm.str(std::move(text)));
                return Value::object(result);
            });
    });

    def("processAlive", 1, 1, [](VM&, int, Value* args) -> Value {
        const std::shared_ptr<Stream> s = findStream(args[0]);
        if (s == nullptr || s->exited.load() || s->closed.load()) return Value::boolean(false);
        return Value::boolean(WaitForSingleObject(s->handle, 0) == WAIT_TIMEOUT);
    });

    def("processKill", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        if (!s->exited.load() && !s->closed.load()) TerminateProcess(s->handle, 1);
        return Value::null();
    });

    def("processId", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::integer(streamOf(vm, args[0])->pid); });

    def("currentPid", 0, 0, [](VM&, int, Value*) -> Value { return Value::integer(GetCurrentProcessId()); });

    // What started this program: the path of its executable and, when astra is running a
    // script, the script. Starting that list again starts this program again.
    def("selfCommand", 0, 0, [](VM& vm, int, Value*) -> Value {
        ObjList* list = newList(vm);
        TempRoot keepList(vm, Value::object(list));
        for (const std::string& part : selfCommand) list->values().push_back(vm.str(part));
        return Value::object(list);
    });

    // ---- Sockets ----

    static std::mutex winsockMutex;
    static bool winsockStarted = false;
    auto startWinsock = [](VM& vm) {
        std::lock_guard<std::mutex> hold(winsockMutex);
        if (winsockStarted) return;
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw ioError(vm, "networking is not available");
        winsockStarted = true;
    };
    static const auto ensureWinsock = startWinsock;

    // connect(host, port, timeout): a TCP connection, tried for at most `timeout` ms if given.
    def("connect", 2, 3, [](VM& vm, int argc, Value* args) -> Value {
        ensureWinsock(vm);
        const std::string host = vm.toStr(args[0], "host");
        const int64_t port = vm.toInt(args[1], "port");
        const int64_t timeout = argc > 2 && !args[2].isNull() ? std::max<int64_t>(vm.toInt(args[2], "time limit"), 0) : -1;
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                addrinfoW hints{};
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;
                addrinfoW* found = nullptr;
                if (GetAddrInfoW(widen(host).c_str(), std::to_wstring(port).c_str(), &hints, &found) != 0 || found == nullptr) {
                    out->error = "cannot find host '" + host + "'";
                    return;
                }
                int lastError = 0;
                for (const addrinfoW* address = found; address != nullptr && out->made == nullptr; address = address->ai_next) {
                    SOCKET s = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
                    if (s == INVALID_SOCKET) continue;
                    bool connected;
                    if (timeout < 0) {
                        connected = ::connect(s, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0;
                        if (!connected) lastError = WSAGetLastError();
                    } else {
                        // With a time limit: start connecting without waiting, then wait that long.
                        u_long nonBlocking = 1;
                        ioctlsocket(s, FIONBIO, &nonBlocking);
                        connected = ::connect(s, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0;
                        if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
                            fd_set writable, failed;
                            FD_ZERO(&writable);
                            FD_ZERO(&failed);
                            FD_SET(s, &writable);
                            FD_SET(s, &failed);
                            timeval limit{static_cast<long>(timeout / 1000), static_cast<long>((timeout % 1000) * 1000)};
                            const int ready = select(0, nullptr, &writable, &failed, &limit);
                            connected = ready > 0 && FD_ISSET(s, &writable);
                            lastError = ready == 0 ? WSAETIMEDOUT : WSAECONNREFUSED;
                        }
                        nonBlocking = 0;
                        ioctlsocket(s, FIONBIO, &nonBlocking);
                    }
                    if (!connected) {
                        closesocket(s);
                        continue;
                    }
                    out->made = std::make_shared<Stream>(Stream::Kind::Socket);
                    out->made->socket = s;
                }
                FreeAddrInfoW(found);
                if (out->made == nullptr)
                    out->error = "cannot connect to " + host + ":" + std::to_string(port) + ": " +
                                 (lastError == WSAETIMEDOUT ? "timed out" : systemMessage(static_cast<DWORD>(lastError)));
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, out->error);
                return Value::integer(keep(out->made));
            });
    });

    // listen(port, address, backlog): a socket that takes connections. Port 0 lets the system
    // choose one; with no address it listens on every one this machine has.
    def("listen", 1, 3, [](VM& vm, int argc, Value* args) -> Value {
        ensureWinsock(vm);
        const int64_t port = vm.toInt(args[0], "port");
        const std::string address = argc > 1 && !args[1].isNull() ? vm.toStr(args[1], "address") : std::string();
        const int backlog = argc > 2 && !args[2].isNull() ? static_cast<int>(vm.toInt(args[2], "backlog")) : 50;

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(static_cast<u_short>(port));
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (!address.empty() && InetPtonW(AF_INET, widen(address == "localhost" ? "127.0.0.1" : address).c_str(), &local.sin_addr) != 1)
            throw ioError(vm, "'" + address + "' is not an address to listen on");

        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 || ::listen(s, backlog) != 0) {
            const std::string why = systemMessage(static_cast<DWORD>(WSAGetLastError()));
            if (s != INVALID_SOCKET) closesocket(s);
            throw ioError(vm, "cannot listen on port " + std::to_string(port) + ": " + why);
        }
        auto listener = std::make_shared<Stream>(Stream::Kind::Listener);
        listener->socket = s;
        return Value::integer(keep(listener));
    });

    // Waits for a connection and gives it.
    def("accept", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                Use use(*s);
                const SOCKET connection = ::accept(s->socket, nullptr, nullptr);
                if (connection == INVALID_SOCKET) {
                    out->error = s->closed.load() ? "the socket is closed" : "cannot accept a connection: " + systemMessage(static_cast<DWORD>(WSAGetLastError()));
                    return;
                }
                out->made = std::make_shared<Stream>(Stream::Kind::Socket);
                out->made->socket = connection;
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, out->error);
                return Value::integer(keep(out->made));
            });
    });

    // [local port, remote address, remote port] of a socket; the last two null for one that listens.
    def("socketInfo", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        ObjList* list = newList(vm);
        TempRoot keepList(vm, Value::object(list));
        sockaddr_storage address{};
        int size = sizeof(address);
        auto portOf = [](const sockaddr_storage& a) {
            return ntohs(a.ss_family == AF_INET6 ? reinterpret_cast<const sockaddr_in6&>(a).sin6_port : reinterpret_cast<const sockaddr_in&>(a).sin_port);
        };
        list->values().push_back(getsockname(s->socket, reinterpret_cast<sockaddr*>(&address), &size) == 0 ? Value::integer(portOf(address)) : Value::null());
        size = sizeof(address);
        if (s->kind == Stream::Kind::Socket && getpeername(s->socket, reinterpret_cast<sockaddr*>(&address), &size) == 0) {
            wchar_t text[64] = {};
            const void* raw = address.ss_family == AF_INET6 ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6&>(address).sin6_addr)
                                                            : static_cast<const void*>(&reinterpret_cast<const sockaddr_in&>(address).sin_addr);
            InetNtopW(address.ss_family, raw, text, 64);
            list->values().push_back(vm.str(narrow(text)));
            list->values().push_back(Value::integer(portOf(address)));
        } else {
            list->values().push_back(Value::null());
            list->values().push_back(Value::null());
        }
        return Value::object(list);
    });

    // How long a read on a socket waits before giving up, in ms; 0 for as long as it takes.
    def("socketTimeout", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const DWORD limit = static_cast<DWORD>(std::max<int64_t>(vm.toInt(args[1], "time limit"), 0));
        setsockopt(s->socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&limit), sizeof(limit));
        return Value::null();
    });

    // No more will be written on this socket: the other side reads the end.
    def("shutdownOutput", 1, 1, [](VM& vm, int, Value* args) -> Value {
        shutdown(streamOf(vm, args[0])->socket, SD_SEND);
        return Value::null();
    });

    vm.defineGlobal("__io", Value::object(io));
}

#else  // not Windows

void registerIo(VM& vm) {
    ObjClass* io = vm.alloc<ObjClass>(vm.intern("io"));
    for (const char* name : {"open", "read", "readAll", "readLine", "write", "close", "isOpen", "spawn", "processWait", "processAlive", "processKill",
                             "processId", "currentPid", "selfCommand", "connect", "listen", "accept", "socketInfo", "socketTimeout", "shutdownOutput"})
        io->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("io.") + name, 0, -1, [](VM& vm, int, Value*) -> Value {
                            throw ioError(vm, "streams, processes and sockets are only there on Windows so far");
                        })));
    vm.defineGlobal("__io", Value::object(io));
}

#endif

}  // namespace astra
