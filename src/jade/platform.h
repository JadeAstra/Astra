// Jade's native layer: the part written once for each system.
//
// A window with an OpenGL context to draw in, the events the system sends it, the pointer's
// shape, the clipboard, and waiting. Everything above this file (drawing, text, the toolkit
// itself) is the same code everywhere, so running on another system means writing this
// interface again for it and nothing else. platform_win.cpp is the one for Windows.
//
// Sizes and positions here are in device pixels, the screen's own. A window says how many
// of them make one of the layout's units (its scale: 1, 1.25, 1.5, 2).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace astra::jade {

// The physical keys, in the order of the toolkit's KeyCode. The names are in keyName().
enum class Key : int {
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24, F25,
    Space, Apostrophe, Comma, Minus, Period, Slash, Semicolon, Equal, LeftBracket, Backslash, RightBracket, GraveAccent,
    Escape, Enter, Tab, Backspace, Insert, Delete, Right, Left, Down, Up, PageUp, PageDown, Home, End,
    CapsLock, ScrollLock, NumLock, PrintScreen, Pause, Menu,
    Numpad0, Numpad1, Numpad2, Numpad3, Numpad4, Numpad5, Numpad6, Numpad7, Numpad8, Numpad9,
    NumpadDecimal, NumpadDivide, NumpadMultiply, NumpadSubtract, NumpadAdd, NumpadEnter, NumpadEqual,
    ShiftLeft, ControlLeft, AltLeft, MetaLeft, ShiftRight, ControlRight, AltRight, MetaRight,
    Unknown,
};

// The name a key has in the toolkit: "a", "digit0", "pageUp", "shiftLeft", "unknown".
const char* keyName(Key key);
// The key of that name; Unknown if there is none.
Key keyNamed(const std::string& name);

enum Modifier : int { kShift = 1, kControl = 2, kAlt = 4, kMeta = 8 };

// What the system reports about a window, as it happens and with nothing decided: which
// widget a press belongs to, what a double click is and when a key repeats are the
// toolkit's business.
struct RawEvent {
    enum Kind {
        PointerMove,   // x, y; buttons (bit 0 left, 1 right, 2 middle, 3 back, 4 forward); modifiers
        PointerDown,   // x, y; button (0 left, 1 right, 2 middle, 3 back, 4 forward); modifiers
        PointerUp,
        PointerLeave,  // the pointer has left the window
        Wheel,         // x, y; dx, dy in notches (one click of a wheel is 1; up and left are positive)
        KeyDown,       // key; letter (what the key types in the keyboard's layout, upper case, or 0); modifiers; repeat
        KeyUp,
        Text,          // text: what was typed, after the layout and any composing
        Focus,         // on: the window has the keyboard, or has lost it
        Resize,        // width, height of what can be drawn on
        Scale,         // scale: the window is now on a screen of another density
        Close,         // the user asked for the window to close; nothing has been closed
        FilesDropped,  // files; x, y
    };
    Kind kind = PointerMove;
    int x = 0, y = 0;
    int button = 0, buttons = 0, modifiers = 0;
    double dx = 0, dy = 0;
    Key key = Key::Unknown;
    uint32_t letter = 0;
    bool repeat = false, on = false;
    int width = 0, height = 0;
    double scale = 1;
    std::string text;
    std::vector<std::string> files;
};

// The shapes a pointer can take, in the order of the toolkit's CursorKind.
enum class Cursor : int { Default, ResizeH, ResizeV, Text, Hand, Move, ResizeNwse, ResizeNesw, Crosshair };
enum class PointerMode : int { Normal, Hidden, Locked };

struct WindowSettings {
    std::string title;
    int width = 900, height = 640;  // of what can be drawn on, in layout units
    bool resizable = true;
    bool dialog = false;            // no minimise or maximise buttons
    double scale = 0;               // 0: the screen's own. Anything else is for tests.
};

class PlatformWindow {
public:
    virtual ~PlatformWindow() = default;

    // Where events go. Called on the thread that pumps events, and for a resize from inside
    // the system's own loop while an edge is being dragged.
    std::function<void(const RawEvent&)> onEvent;
    // Asked when the system wants the window's picture again (it was uncovered, or resized).
    std::function<void()> onExpose;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual bool visible() const = 0;
    virtual void setTitle(const std::string& title) = 0;
    // What can be drawn on, in device pixels.
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual void setSize(int width, int height) = 0;
    // The window's outer corner on the screen.
    virtual void position(int& x, int& y) const = 0;
    virtual void setPosition(int x, int y) = 0;
    // Limits on what can be drawn on; 0 for none.
    virtual void setLimits(int minWidth, int minHeight, int maxWidth, int maxHeight) = 0;
    virtual void setResizable(bool resizable) = 0;
    virtual void toFront() = 0;
    virtual void requestAttention() = 0;
    virtual double scale() const = 0;
    virtual bool focused() const = 0;
    // Pictures for the title bar and the task bar: red, green, blue, alpha, four bytes a pixel.
    virtual void setIcon(const std::vector<uint8_t>& rgba, int width, int height) = 0;

    virtual void setCursor(Cursor cursor) = 0;
    virtual void setPointerMode(PointerMode mode) = 0;

    // For tests: puts a message in the window's queue as the system would, to be handled
    // when events are next pumped. "moveBegan" and "moveEnded" are what the system says
    // around a drag of the title bar or an edge; "close" is a request to close.
    virtual void postForTest(const std::string& what) = 0;

    // Drawing: the one OpenGL context made current on this window, and its picture shown.
    virtual bool makeCurrent() = 0;
    virtual void swapBuffers() = 0;
};

class Platform {
public:
    // The system this program runs on. Null, with `error`, if it has no windows to offer
    // (no display, or OpenGL 3.3 is missing).
    static Platform* get(std::string& error);

    virtual ~Platform() = default;

    // A window, hidden until shown. `owner` (may be null) is the window it belongs to and
    // stays in front of. Null, with `error`, if it cannot be made.
    virtual std::unique_ptr<PlatformWindow> createWindow(const WindowSettings& settings, PlatformWindow* owner, std::string& error) = 0;

    // The OpenGL context made current with no window, for drawing into pictures.
    virtual bool makeCurrentOffscreen() = 0;

    // Handles every event that is waiting, calling the windows' onEvent.
    virtual void pumpEvents() = 0;
    // Waits for an event, or wake(), for at most `millis` (-1: for as long as it takes).
    virtual void waitEvents(int64_t millis) = 0;
    // Ends a waitEvents() going on in the pumping thread. May be called from any thread.
    virtual void wake() = 0;
    // While the system runs a loop of its own (an edge or the title bar being dragged), it
    // calls this now and then so that the program's timers go on.
    std::function<void()> onIdle;

    virtual std::string clipboardText() = 0;
    virtual void setClipboardText(const std::string& text) = 0;

    // How long, in milliseconds, two presses may be apart and still be a double click.
    virtual int doubleClickTime() = 0;
    // The scale of the main screen.
    virtual double screenScale() = 0;
};

}  // namespace astra::jade
