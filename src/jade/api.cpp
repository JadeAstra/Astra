// Jade's native layer as the library sees it: the built-in __jade, which lib/Jade is
// written on. Windows, the raw events they receive, pictures on the graphics card
// ("surfaces"), the drawing operations of the toolkit's canvas, the clipboard and the clock.
//
// The library works in layout units; everything below this file works in device pixels.
// The crossing is here: a position is multiplied by the scale and rounded to a whole
// pixel, so that the edges of things are sharp at any scale and neighbours still meet.
//
// Windows and drawing belong to the main thread, the one the event loop runs on. The
// window system's events are handled from that loop (VM::loopPump), so a program with a
// window open goes on until its windows are closed, with no loop of its own to write.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>

#include "../builtins.h"
#include "../vm.h"
#include "platform.h"
#include "render.h"

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace astra {

std::shared_ptr<text::Font> textFontOf(VM& vm, Value v);  // text/api.cpp

namespace {

using jade::Platform;
using jade::PlatformWindow;
using jade::RawEvent;
using jade::Renderer;

struct Window {
    std::unique_ptr<PlatformWindow> native;
    int surface = 0;
    bool holdsLoop = false;  // shown: the program goes on while it is
};

struct State {
    VM* vm = nullptr;
    Renderer* renderer = nullptr;
    std::map<int, Window> windows;
    int nextWindow = 1;

    bool hasDispatcher = false;
    size_t dispatcherPin = 0;
    Value dispatcher;

    // An error raised by the program while it was being told of an event. It cannot leave
    // through the window system's own code, so it is kept and raised again once we are out.
    bool hasThrow = false, hasExit = false;
    AstraThrow pendingThrow;
    int exitCode = 0;
    bool pumping = false;
    bool idling = false;
    // Windows closed from inside one of their own events: the event is still on its way
    // through the window's code, so the window itself goes once that has returned.
    int delivering = 0;
    std::vector<std::unique_ptr<PlatformWindow>> closed;

    // Where the drawing calls draw: set by `target`.
    int originX = 0, originY = 0;
    double scale = 1;
    uint32_t color = 0x000000FF;
    std::shared_ptr<text::Font> font;

    // For tests: a clipboard of the program's own, so that the system's is left alone.
    bool ownClipboard = false;
    std::string clipboard;
};

// Made the first time it is needed, not as the program starts: this file's code is left out
// of a built program that does not use it, start-up code included (M6.5 part 5).
State& state() {
    static State made;
    return made;
}

Renderer& need(VM& vm) {
    if (!vm.onLoopThread()) throw vm.runtimeError("windows and drawing belong to the main thread; hand the work to it with EventLoop.post");
    if (state().renderer == nullptr) {
        std::string error;
        state().renderer = Renderer::get(error);
        if (state().renderer == nullptr) throw vm.runtimeError("no windows or drawing here: " + error);
    }
    return *state().renderer;
}

Window& windowOf(VM& vm, Value v) {
    auto found = state().windows.find(vm.toInt32(v, "window"));
    if (found == state().windows.end()) throw vm.runtimeError("the window has been closed");
    return found->second;
}

void release(VM& vm, Window& window) {
    if (!window.holdsLoop) return;
    window.holdsLoop = false;
    vm.jobFinished();
}

// A surface is given as the value surfaceCreate made, or as a window's own by its number.
int surfaceOf(VM& vm, Value v) {
    if (v.isObjType(ObjType::Resource)) return static_cast<int>(static_cast<const ObjResource*>(v.as.obj)->id);
    return vm.toInt32(v, "surface");
}

jade::Surface& surfaceNeeded(VM& vm, Renderer& r, Value v) {
    jade::Surface* s = r.surface(surfaceOf(vm, v));
    if (s == nullptr) throw vm.runtimeError("the picture has been disposed of");
    return *s;
}

// From 0xAARRGGBB, as programs write colours, to 0xRRGGBBAA.
uint32_t colorOf(VM& vm, Value v) {
    const uint64_t argb = static_cast<uint64_t>(vm.toInt(v, "colour"));
    return static_cast<uint32_t>(((argb & 0xFFFFFF) << 8) | ((argb >> 24) & 0xFF));
}

// From layout units to device pixels: a place, and a thickness.
int dx(double x) { return state().originX + static_cast<int>(std::lround(x * state().scale)); }
int dy(double y) { return state().originY + static_cast<int>(std::lround(y * state().scale)); }
int thickness() { return std::max(1, static_cast<int>(std::lround(state().scale))); }
double num(VM& vm, Value v) { return vm.toNumber(v, "number"); }

// ---- Events ----

Value done(Value result = Value::null()) {
    if (state().pumping) return result;  // raised once the window system's code has been left
    if (state().hasThrow) {
        state().hasThrow = false;
        AstraThrow thrown = state().pendingThrow;
        state().pendingThrow = AstraThrow();
        throw thrown;
    }
    if (state().hasExit) {
        state().hasExit = false;
        throw AstraExit{state().exitCode};
    }
    return result;
}

// Tells the program of an event: dispatcher(window, kind, x, y, n1, n2, f1, f2, extra).
void dispatch(int window, const char* kind, int64_t x = 0, int64_t y = 0, int64_t n1 = 0, int64_t n2 = 0, double f1 = 0, double f2 = 0,
              Value extra = Value::null()) {
    if (!state().hasDispatcher || state().hasThrow || state().hasExit) return;
    VM& vm = *state().vm;
    std::vector<Value> args = {Value::integer(window), Value::null(), Value::integer(x),  Value::integer(y), Value::integer(n1),
                               Value::integer(n2),     Value::number(f1), Value::number(f2), extra};
    TempVector keep(vm, args);
    args[1] = Value::object(vm.intern(kind));
    try {
        vm.call(state().dispatcher, static_cast<int>(args.size()), args.data());
    } catch (AstraThrow& thrown) {
        state().pendingThrow = thrown;
        state().hasThrow = true;
    } catch (AstraExit& exit) {
        state().exitCode = exit.code;
        state().hasExit = true;
    }
}

// One event of the window system, as it goes to the program: positions in layout units.
void deliver(int id, const RawEvent& e) {
    auto found = state().windows.find(id);
    if (found == state().windows.end()) return;
    Window& window = found->second;
    VM& vm = *state().vm;
    state().delivering++;
    struct Leave {
        ~Leave() { state().delivering--; }
    } leave;
    const double scale = window.native->scale();
    auto unit = [&](int device) { return static_cast<int64_t>(std::floor(device / scale)); };

    switch (e.kind) {
        case RawEvent::PointerMove:
            dispatch(id, "pointerMove", unit(e.x), unit(e.y), e.buttons, e.modifiers, e.dx / scale, e.dy / scale);
            break;
        case RawEvent::PointerDown:
            dispatch(id, "pointerDown", unit(e.x), unit(e.y), e.button, e.modifiers);
            break;
        case RawEvent::PointerUp:
            dispatch(id, "pointerUp", unit(e.x), unit(e.y), e.button, e.modifiers);
            break;
        case RawEvent::PointerLeave:
            dispatch(id, "pointerLeave");
            break;
        case RawEvent::Wheel:
            dispatch(id, "wheel", unit(e.x), unit(e.y), 0, e.modifiers, e.dx, e.dy);
            break;
        case RawEvent::KeyDown:
        case RawEvent::KeyUp:
            dispatch(id, e.kind == RawEvent::KeyDown ? "keyDown" : "keyUp", e.letter, 0, e.repeat ? 1 : 0, e.modifiers, 0, 0,
                     Value::object(vm.intern(jade::keyName(e.key))));
            break;
        case RawEvent::Text: {
            TempRoot text(vm, vm.str(e.text));
            dispatch(id, "text", 0, 0, 0, 0, 0, 0, text.get());
            break;
        }
        case RawEvent::Focus:
            dispatch(id, "focus", 0, 0, e.on ? 1 : 0);
            break;
        case RawEvent::Resize:
            // The window's picture follows its size, keeping what was on it: the program
            // draws it again when it hears of the new size, from inside this very call if
            // an edge is being dragged.
            if (state().renderer != nullptr && e.width > 0 && e.height > 0) state().renderer->resizeSurface(window.surface, e.width, e.height);
            dispatch(id, "resize", unit(e.width), unit(e.height), e.width, e.height, scale);
            break;
        case RawEvent::Scale:
            dispatch(id, "scale", 0, 0, 0, 0, e.scale);
            break;
        case RawEvent::Close:
            dispatch(id, "close");
            break;
        case RawEvent::FilesDropped: {
            ObjList* files = newList(vm);
            TempRoot keep(vm, Value::object(files));
            for (const std::string& path : e.files) files->values().push_back(vm.str(path));
            dispatch(id, "filesDropped", unit(e.x), unit(e.y), 0, 0, 0, 0, Value::object(files));
            break;
        }
    }
}

void raisePending() {
    if (state().hasThrow) {
        state().hasThrow = false;
        AstraThrow thrown = state().pendingThrow;
        state().pendingThrow = AstraThrow();
        throw thrown;
    }
    if (state().hasExit) {
        state().hasExit = false;
        throw AstraExit{state().exitCode};
    }
}

// The window system's events, handled from the runtime's event loop.
void pump(VM&) {
    if (state().renderer == nullptr || state().pumping) return;
    state().pumping = true;
    state().renderer->platform().pumpEvents();
    state().pumping = false;
    state().closed.clear();
    raisePending();
}

void waitForEvents(int64_t millis) {
    if (state().renderer != nullptr) state().renderer->platform().waitEvents(millis);
}

void wakeLoop() {
    std::string unused;
    if (Platform* platform = Platform::get(unused)) platform->wake();
}

// While the system runs a loop of its own (a window being dragged by its title bar or an
// edge), the program's tasks and timers are run from here, so an animation goes on
// meanwhile. Only for a moment at a time: the system's loop has to get its turn back, or
// the drag never moves and the window stops answering. An animation whose frame takes
// longer than its timer's period always has a frame due, so this cannot wait for "nothing
// left to do".
void idle() {
    VM& vm = *state().vm;
    if (state().hasThrow || state().hasExit || state().idling) return;
    state().idling = true;
    struct Reset {
        ~Reset() { state().idling = false; }
    } reset;
    try {
        vm.runEventLoop(false, 2);
    } catch (AstraThrow& thrown) {
        state().pendingThrow = thrown;
        state().hasThrow = true;
    } catch (AstraExit& exit) {
        state().exitCode = exit.code;
        state().hasExit = true;
    }
}

void hookLoop(VM& vm) {
    if (vm.loopPump != nullptr) return;
    vm.loopPump = pump;
    vm.loopWait = waitForEvents;
    vm.onLoopWake = wakeLoop;
    state().renderer->platform().onIdle = idle;
}

RawEvent syntheticEvent(VM& vm, Window& window, const std::string& kind, Value* args) {
    const double scale = window.native->scale();
    auto device = [&](Value v) { return static_cast<int>(std::lround(vm.toNumber(v, "position") * scale)); };
    RawEvent e;
    if (kind == "pointerMove" || kind == "pointerDown" || kind == "pointerUp" || kind == "wheel" || kind == "filesDropped") {
        e.x = device(args[0]);
        e.y = device(args[1]);
    }
    if (kind == "pointerMove") {
        e.kind = RawEvent::PointerMove;
        e.buttons = vm.toInt32(args[2], "buttons");
    } else if (kind == "pointerDown" || kind == "pointerUp") {
        e.kind = kind == "pointerDown" ? RawEvent::PointerDown : RawEvent::PointerUp;
        e.button = vm.toInt32(args[2], "button");
    } else if (kind == "pointerLeave") {
        e.kind = RawEvent::PointerLeave;
    } else if (kind == "wheel") {
        e.kind = RawEvent::Wheel;
        e.dx = vm.toNumber(args[4], "dx");
        e.dy = vm.toNumber(args[5], "dy");
    } else if (kind == "keyDown" || kind == "keyUp") {
        e.kind = kind == "keyDown" ? RawEvent::KeyDown : RawEvent::KeyUp;
        e.key = jade::keyNamed(vm.toStr(args[6], "key"));
        e.letter = static_cast<uint32_t>(vm.toInt(args[0], "letter"));
        e.repeat = args[2].truthy() && !(args[2].isInt() && args[2].as.i == 0);
    } else if (kind == "text") {
        e.kind = RawEvent::Text;
        e.text = vm.toStr(args[6], "text");
    } else if (kind == "focus") {
        e.kind = RawEvent::Focus;
        e.on = args[2].truthy() && !(args[2].isInt() && args[2].as.i == 0);
    } else if (kind == "close") {
        e.kind = RawEvent::Close;
    } else if (kind == "filesDropped") {
        e.kind = RawEvent::FilesDropped;
        if (args[6].isObjType(ObjType::List))
            for (const Value& path : vm.listCopy(asList(args[6]))) e.files.push_back(vm.toStr(path, "file path"));
    } else {
        throw vm.runtimeError("no such event: " + kind);
    }
    e.modifiers = vm.toInt32(args[3], "modifiers");
    return e;
}

Value pairOf(VM& vm, std::initializer_list<Value> values) {
    ObjList* list = newList(vm, std::vector<Value>(values));
    return Value::object(list);
}

}  // namespace

void registerJade(VM& vm) {
    state().vm = &vm;
    ObjClass* lib = vm.alloc<ObjClass>(vm.intern("jade"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        lib->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("jade.") + name, minArgs, maxArgs, fn)));
    };

    // ---- The system ----

    // Whether there is anything to draw with here; if not, why.
    def("problem", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (!vm.onLoopThread()) return vm.str("windows and drawing belong to the main thread");
        std::string error;
        if (state().renderer == nullptr) state().renderer = Renderer::get(error);
        return state().renderer != nullptr ? Value::null() : vm.str(error);
    });
    def("describe", 0, 0, [](VM& vm, int, Value*) -> Value { return vm.str(need(vm).description()); });
    // [shapes, batches] sent to the graphics card since the program began, and how many
    // surfaces there are now.
    def("counters", 0, 0, [](VM& vm, int, Value*) -> Value {
        Renderer& r = need(vm);
        r.flush();  // which also lets go of the surfaces the collector has found unused
        return pairOf(vm, {Value::integer(r.shapesDrawn()), Value::integer(r.batchesDrawn()), Value::integer(static_cast<int64_t>(r.surfaceCount()))});
    });
    def("setDispatcher", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (state().hasDispatcher) {
            vm.pinned[state().dispatcherPin] = args[0];
        } else {
            state().dispatcherPin = vm.pinned.size();
            vm.pinned.push_back(args[0]);
        }
        state().dispatcher = args[0];
        state().hasDispatcher = true;
        return Value::null();
    });
    // A clock that only goes forwards, in nanoseconds.
    def("nanos", 0, 0, [](VM&, int, Value*) -> Value {
        return Value::integer(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    });
    def("doubleClickTime", 0, 0, [](VM& vm, int, Value*) -> Value { return Value::integer(need(vm).platform().doubleClickTime()); });
    def("screenScale", 0, 0, [](VM& vm, int, Value*) -> Value { return Value::number(need(vm).platform().screenScale()); });

    def("clipboardOwn", 1, 1, [](VM&, int, Value* args) -> Value {
        state().ownClipboard = args[0].truthy();
        return Value::null();
    });
    def("clipboardGet", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (state().ownClipboard) return vm.str(state().clipboard);
        return vm.str(need(vm).platform().clipboardText());
    });
    def("clipboardSet", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const std::string& text = vm.toStr(args[0], "text");
        if (state().ownClipboard) state().clipboard = text;
        else need(vm).platform().setClipboardText(text);
        return Value::null();
    });

    // ---- Windows ----

    // windowCreate(title, width, height, resizable, dialog, owner or 0, scale or 0): a window,
    // not yet shown. The size is of what can be drawn on, in layout units.
    def("windowCreate", 7, 7, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        jade::WindowSettings settings;
        settings.title = vm.toStr(args[0], "title");
        settings.width = static_cast<int>(std::clamp<int64_t>(vm.toInt(args[1], "width"), 1, 16384));
        settings.height = static_cast<int>(std::clamp<int64_t>(vm.toInt(args[2], "height"), 1, 16384));
        settings.resizable = args[3].truthy();
        settings.dialog = args[4].truthy();
        settings.scale = vm.toNumber(args[6], "scale");
        PlatformWindow* owner = nullptr;
        const int64_t ownerId = vm.toInt(args[5], "owner");
        if (ownerId != 0) owner = windowOf(vm, args[5]).native.get();

        std::string error;
        const int id = state().nextWindow++;
        Window window;
        window.native = r.platform().createWindow(settings, owner, error);
        if (window.native == nullptr) throw vm.runtimeError("the window cannot be made: " + error);
        window.surface = r.createSurface(std::max(window.native->width(), 1), std::max(window.native->height(), 1));
        if (window.surface == 0) throw vm.runtimeError("the window is too large to draw");
        window.native->onEvent = [id](const RawEvent& event) { deliver(id, event); };
        window.native->onExpose = [id] {
            auto found = state().windows.find(id);
            if (found != state().windows.end() && state().renderer != nullptr && state().renderer->present(found->second.surface, *found->second.native))
                found->second.native->swapBuffers();
        };
        state().windows[id] = std::move(window);
        hookLoop(vm);
        return Value::integer(id);
    });

    def("windowShow", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        if (!window.holdsLoop) {
            window.holdsLoop = true;
            vm.jobStarted();
        }
        window.native->show();
        return done();
    });
    def("windowHide", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        window.native->hide();
        release(vm, window);
        return done();
    });
    // Closes a window for good. Closing one that is closed already does nothing.
    def("windowClose", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto found = state().windows.find(vm.toInt32(args[0], "window"));
        if (found == state().windows.end()) return Value::null();
        Renderer& r = need(vm);
        release(vm, found->second);
        r.destroySurface(found->second.surface);
        // Taken out of the table first: destroying it sends it messages still.
        Window gone = std::move(found->second);
        state().windows.erase(found);
        if (state().delivering > 0 || state().pumping) {
            // Closed from inside an event: out of sight now, and gone when the event has
            // finished its way through the window's own code.
            gone.native->hide();
            state().closed.push_back(std::move(gone.native));
        } else {
            gone.native.reset();
        }
        return done();
    });
    def("windowIsOpen", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return Value::boolean(state().windows.count(vm.toInt32(args[0], "window")) != 0);
    });
    def("windowTitle", 2, 2, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->setTitle(vm.toStr(args[1], "title"));
        return Value::null();
    });
    // [width, height] in layout units, [width, height] in device pixels, the scale, and the surface.
    def("windowInfo", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        const double scale = window.native->scale();
        const int w = window.native->width(), h = window.native->height();
        return pairOf(vm, {Value::integer(static_cast<int64_t>(std::floor(w / scale))), Value::integer(static_cast<int64_t>(std::floor(h / scale))),
                           Value::integer(w), Value::integer(h), Value::number(scale), Value::integer(window.surface)});
    });
    def("windowSetSize", 3, 3, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        const double scale = window.native->scale();
        window.native->setSize(static_cast<int>(std::lround(num(vm, args[1]) * scale)), static_cast<int>(std::lround(num(vm, args[2]) * scale)));
        return done();
    });
    // Where the window is on the screen, in the screen's own pixels.
    def("windowPosition", 1, 1, [](VM& vm, int, Value* args) -> Value {
        int x = 0, y = 0;
        windowOf(vm, args[0]).native->position(x, y);
        return pairOf(vm, {Value::integer(x), Value::integer(y)});
    });
    def("windowSetPosition", 3, 3, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->setPosition(vm.toInt32(args[1], "x"), vm.toInt32(args[2], "y"));
        return done();
    });
    // windowLimits(window, minWidth, minHeight, maxWidth, maxHeight) in layout units; 0 for none.
    def("windowLimits", 5, 5, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        const double scale = window.native->scale();
        auto device = [&](Value v) { return static_cast<int>(std::lround(num(vm, v) * scale)); };
        window.native->setLimits(device(args[1]), device(args[2]), device(args[3]), device(args[4]));
        return Value::null();
    });
    def("windowResizable", 2, 2, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->setResizable(args[1].truthy());
        return done();
    });
    def("windowToFront", 1, 1, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->toFront();
        return done();
    });
    def("windowAttention", 1, 1, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->requestAttention();
        return Value::null();
    });
    def("windowState", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        return pairOf(vm, {Value::boolean(window.native->visible()), Value::boolean(window.native->focused())});
    });
    // windowIcon(window, pixels, width, height): red, green, blue, alpha, four bytes a pixel.
    def("windowIcon", 4, 4, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        if (!args[1].isObjType(ObjType::Bytes)) throw vm.runtimeError("an icon's pixels are bytes");
        window.native->setIcon(asBytes(args[1])->data, vm.toInt32(args[2], "width"), vm.toInt32(args[3], "height"));
        return Value::null();
    });
    def("windowCursor", 2, 2, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->setCursor(static_cast<jade::Cursor>(std::clamp<int64_t>(vm.toInt(args[1], "cursor"), 0, 8)));
        return Value::null();
    });
    def("windowPointerMode", 2, 2, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->setPointerMode(static_cast<jade::PointerMode>(std::clamp<int64_t>(vm.toInt(args[1], "pointer mode"), 0, 2)));
        return Value::null();
    });
    // Shows on the screen what has been drawn on the window's surface.
    def("windowPresent", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        Renderer& r = need(vm);
        if (!window.native->visible()) {
            r.flush();
            return Value::null();
        }
        if (r.present(window.surface, *window.native)) {
            // Showing it waits for the screen's next refresh; the program's other threads
            // have that time.
            PlatformWindow* native = window.native.get();
            LockReleased released(vm);
            native->swapBuffers();
        }
        return Value::null();
    });
    // windowSend(window, kind, x, y, n1, n2, f1, f2, extra): an event made up by a test,
    // which then goes the way a real one does.
    def("windowSend", 9, 9, [](VM& vm, int, Value* args) -> Value {
        Window& window = windowOf(vm, args[0]);
        const int id = vm.toInt32(args[0], "window");
        const RawEvent event = syntheticEvent(vm, window, vm.toStr(args[1], "kind of event"), args + 2);
        deliver(id, event);
        if (state().delivering == 0 && !state().pumping) state().closed.clear();
        return done();
    });

    // windowPost(window, what): for tests, a message put in the window's queue as the
    // system would put it ("moveBegan", "moveEnded", "close"), handled when events are
    // next pumped and not from inside this call.
    def("windowPost", 2, 2, [](VM& vm, int, Value* args) -> Value {
        windowOf(vm, args[0]).native->postForTest(vm.toStr(args[1], "kind of message"));
        return Value::null();
    });

    // ---- Surfaces ----

    // A picture on the graphics card, every pixel clear. In device pixels.
    def("surfaceCreate", 2, 2, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        const int64_t w = vm.toInt(args[0], "width"), h = vm.toInt(args[1], "height");
        const int64_t most = r.maxSurfaceSize();
        if (w < 1 || h < 1 || w > most || h > most)
            throw vm.runtimeError("a picture on the graphics card is from 1 to " + std::to_string(most) + " pixels each way, not " + std::to_string(w) +
                                  " by " + std::to_string(h));
        const int id = r.createSurface(static_cast<int>(w), static_cast<int>(h));
        if (id == 0) throw vm.runtimeError("the graphics card cannot hold a picture of " + std::to_string(w) + " by " + std::to_string(h));
        // What it takes on the card counts towards the next collection, which is what lets
        // go of the surfaces a program has stopped using.
        vm.account(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        return Value::object(vm.alloc<ObjResource>("surface", id, &Renderer::releaseLater));
    });
    def("surfaceDispose", 1, 1, [](VM& vm, int, Value* args) -> Value {
        need(vm).destroySurface(surfaceOf(vm, args[0]));
        return Value::null();
    });
    def("surfaceSize", 1, 1, [](VM& vm, int, Value* args) -> Value {
        jade::Surface& s = surfaceNeeded(vm, need(vm), args[0]);
        return pairOf(vm, {Value::integer(s.width), Value::integer(s.height)});
    });
    // The pixels: red, green, blue, alpha, four bytes each, top row first.
    def("surfaceRead", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        surfaceNeeded(vm, r, args[0]);
        ObjBytes* made = vm.alloc<ObjBytes>();
        r.read(surfaceOf(vm, args[0]), made->data);
        vm.account(made->data.size());
        return Value::object(made);
    });
    def("surfaceUpload", 2, 2, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        jade::Surface& s = surfaceNeeded(vm, r, args[0]);
        if (!args[1].isObjType(ObjType::Bytes) || asBytes(args[1])->data.size() != static_cast<size_t>(s.width) * static_cast<size_t>(s.height) * 4)
            throw vm.runtimeError("the pixels do not fit the picture");
        r.upload(surfaceOf(vm, args[0]), asBytes(args[1])->data.data());
        return Value::null();
    });

    // ---- Drawing ----

    // target(surface, originX, originY, scale, clipLeft, clipTop, clipRight, clipBottom):
    // where the calls that follow draw. The origin and the clip are in device pixels; what
    // the drawing calls are given is in layout units from that origin.
    def("target", 8, 8, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        surfaceNeeded(vm, r, args[0]);
        state().originX = vm.toInt32(args[1], "x");
        state().originY = vm.toInt32(args[2], "y");
        state().scale = vm.toNumber(args[3], "scale");
        auto edge = [&](Value v) { return static_cast<int>(std::clamp<int64_t>(vm.toInt(v, "clip"), -(int64_t{1} << 24), int64_t{1} << 24)); };
        r.setTarget(surfaceOf(vm, args[0]), edge(args[4]), edge(args[5]), edge(args[6]), edge(args[7]));
        return Value::null();
    });
    def("color", 1, 1, [](VM& vm, int, Value* args) -> Value {
        state().color = colorOf(vm, args[0]);
        return Value::null();
    });
    def("font", 1, 1, [](VM& vm, int, Value* args) -> Value {
        state().font = textFontOf(vm, args[0]);
        return Value::null();
    });
    def("flush", 0, 0, [](VM& vm, int, Value*) -> Value {
        need(vm).flush();
        return Value::null();
    });

    // Makes a rectangle clear again.
    def("clearRect", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const double x = num(vm, args[0]), y = num(vm, args[1]);
        need(vm).clear(dx(x), dy(y), dx(x + num(vm, args[2])), dy(y + num(vm, args[3])));
        return Value::null();
    });
    def("fillRect", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const double x = num(vm, args[0]), y = num(vm, args[1]);
        need(vm).fillRect(static_cast<float>(dx(x)), static_cast<float>(dy(y)), static_cast<float>(dx(x + num(vm, args[2]))),
                          static_cast<float>(dy(y + num(vm, args[3]))), state().color);
        return Value::null();
    });
    // The outline of a rectangle, one unit thick: it covers the pixels from (x, y) to
    // (x + w, y + h), both included, as Java's drawRect does.
    def("drawRect", 4, 4, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        const double x = num(vm, args[0]), y = num(vm, args[1]), w = num(vm, args[2]), h = num(vm, args[3]);
        if (w < 0 || h < 0) return Value::null();
        const float t = static_cast<float>(thickness());
        const float left = static_cast<float>(dx(x)), top = static_cast<float>(dy(y)), right = static_cast<float>(dx(x + w)), bottom = static_cast<float>(dy(y + h));
        r.fillRect(left, top, right + t, top + t, state().color);
        r.fillRect(left, bottom, right + t, bottom + t, state().color);
        r.fillRect(left, top + t, left + t, bottom, state().color);
        r.fillRect(right, top + t, right + t, bottom, state().color);
        return Value::null();
    });
    // roundRect(x, y, w, h, arcWidth, arcHeight, fill)
    def("roundRect", 7, 7, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        const double x = num(vm, args[0]), y = num(vm, args[1]), w = num(vm, args[2]), h = num(vm, args[3]);
        const float radius = static_cast<float>(std::min(num(vm, args[4]), num(vm, args[5])) / 2 * state().scale);
        const float left = static_cast<float>(dx(x)), top = static_cast<float>(dy(y)), right = static_cast<float>(dx(x + w)), bottom = static_cast<float>(dy(y + h));
        const float t = static_cast<float>(thickness());
        if (args[6].truthy()) r.roundRect(left, top, right, bottom, radius, 0, state().color);
        else r.roundRect(left, top, right + t, bottom + t, radius + t / 2, t, state().color);
        return Value::null();
    });
    // oval(x, y, w, h, fill)
    def("oval", 5, 5, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        const double x = num(vm, args[0]), y = num(vm, args[1]), w = num(vm, args[2]), h = num(vm, args[3]);
        const float left = static_cast<float>(dx(x)), top = static_cast<float>(dy(y)), right = static_cast<float>(dx(x + w)), bottom = static_cast<float>(dy(y + h));
        const float t = static_cast<float>(thickness());
        if (args[4].truthy()) r.oval(left, top, right, bottom, 0, state().color);
        else r.oval(left, top, right + t, bottom + t, t, state().color);
        return Value::null();
    });
    // A line one unit thick from one point to another, both covered.
    def("line", 4, 4, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        const double x1 = num(vm, args[0]), y1 = num(vm, args[1]), x2 = num(vm, args[2]), y2 = num(vm, args[3]);
        const int t = thickness();
        const int ax = dx(x1), ay = dy(y1), bx = dx(x2), by = dy(y2);
        if (ax == bx || ay == by) {
            // Straight across or straight down: whole pixels, with nothing to smooth.
            r.fillRect(static_cast<float>(std::min(ax, bx)), static_cast<float>(std::min(ay, by)), static_cast<float>(std::max(ax, bx) + t),
                       static_cast<float>(std::max(ay, by) + t), state().color);
        } else {
            const float middle = static_cast<float>(t - 1) / 2;
            r.line(static_cast<float>(ax) + middle, static_cast<float>(ay) + middle, static_cast<float>(bx) + middle, static_cast<float>(by) + middle,
                   static_cast<float>(t), state().color);
        }
        return Value::null();
    });
    // polygon(xs, ys, count, fill, closed): filled, or its outline, or (not closed) a line through the points.
    def("polygon", 5, 5, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        if (!args[0].isObjType(ObjType::List) || !args[1].isObjType(ObjType::List)) throw vm.runtimeError("a polygon's points are two lists of numbers");
        const std::vector<Value> xs = vm.listCopy(asList(args[0]));
        const std::vector<Value> ys = vm.listCopy(asList(args[1]));
        const size_t count = static_cast<size_t>(std::clamp<int64_t>(vm.toInt(args[2], "number of points"), 0, static_cast<int64_t>(std::min(xs.size(), ys.size()))));
        if (count < 2) return Value::null();
        if (args[3].truthy()) {
            std::vector<float> points;
            points.reserve(count * 2);
            for (size_t i = 0; i < count; i++) {
                points.push_back(static_cast<float>(state().originX + num(vm, xs[i]) * state().scale));
                points.push_back(static_cast<float>(state().originY + num(vm, ys[i]) * state().scale));
            }
            r.polygon(points, state().color);
            return Value::null();
        }
        const int t = thickness();
        const float middle = static_cast<float>(t - 1) / 2;
        const size_t segments = args[4].truthy() ? count : count - 1;
        for (size_t i = 0; i < segments; i++) {
            const size_t next = (i + 1) % count;
            const int ax = dx(num(vm, xs[i])), ay = dy(num(vm, ys[i])), bx = dx(num(vm, xs[next])), by = dy(num(vm, ys[next]));
            if (ax == bx || ay == by)
                r.fillRect(static_cast<float>(std::min(ax, bx)), static_cast<float>(std::min(ay, by)), static_cast<float>(std::max(ax, bx) + t),
                           static_cast<float>(std::max(ay, by) + t), state().color);
            else
                r.line(static_cast<float>(ax) + middle, static_cast<float>(ay) + middle, static_cast<float>(bx) + middle, static_cast<float>(by) + middle,
                       static_cast<float>(t), state().color);
        }
        return Value::null();
    });
    // text(string, x, y): at x with its baseline at y, in the font and colour set. Its width in device pixels.
    def("text", 3, 3, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        if (state().font == nullptr) throw vm.runtimeError("no font has been set");
        return Value::integer(r.text(*state().font, vm.toStr(args[0], "text"), dx(num(vm, args[1])), dy(num(vm, args[2])), state().color));
    });
    // blit(surface, x, y, alpha): another surface as it is, pixel for pixel, its corner at (x, y).
    def("blit", 4, 4, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        jade::Surface& s = surfaceNeeded(vm, r, args[0]);
        const float left = static_cast<float>(dx(num(vm, args[1]))), top = static_cast<float>(dy(num(vm, args[2])));
        r.image(surfaceOf(vm, args[0]), 0, 0, static_cast<float>(s.width), static_cast<float>(s.height), left, top, left + static_cast<float>(s.width),
                top + static_cast<float>(s.height), static_cast<float>(num(vm, args[3])));
        return Value::null();
    });
    // image(surface, srcX, srcY, srcW, srcH, x, y, w, h, alpha): a part of another surface
    // (in its own pixels) scaled into a rectangle.
    def("image", 10, 10, [](VM& vm, int, Value* args) -> Value {
        Renderer& r = need(vm);
        surfaceNeeded(vm, r, args[0]);
        const float sx = static_cast<float>(num(vm, args[1])), sy = static_cast<float>(num(vm, args[2]));
        const double x = num(vm, args[5]), y = num(vm, args[6]);
        r.image(surfaceOf(vm, args[0]), sx, sy, sx + static_cast<float>(num(vm, args[3])), sy + static_cast<float>(num(vm, args[4])),
                static_cast<float>(dx(x)), static_cast<float>(dy(y)), static_cast<float>(dx(x + num(vm, args[7]))), static_cast<float>(dy(y + num(vm, args[8]))),
                static_cast<float>(num(vm, args[9])));
        return Value::null();
    });

    vm.defineGlobal("__jade", Value::object(lib));
    vm.markNotParallel("__jade", {"nanos"});
}

}  // namespace astra
