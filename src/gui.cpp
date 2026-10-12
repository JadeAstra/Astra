// The native half of the GUI library. lib/gui.astra builds windows, widgets and layout on
// top of the small handle-based interface defined here (the GuiBackend class). Every window
// and control is identified by an integer; events are reported by calling one Astra function,
// the dispatcher, with the id and the event's name.
//
// Sizes and positions are in logical pixels (96 per inch) on the Astra side and are scaled
// to the display's DPI here.

#include "vm.h"

#ifdef _WIN32

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_map>

#pragma comment(lib, "comctl32.lib")
// Version 6 of the common controls gives buttons, text boxes and lists their modern look.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace astra {

namespace {

const wchar_t* const kWindowClass = L"AstraWindow";
const wchar_t* const kCanvasClass = L"AstraCanvas";
const wchar_t* const kTimerClass = L"AstraTimers";

// Ids start above IDOK and IDCANCEL, which Enter and Escape send as command ids.
const int kFirstId = 100;

enum class Kind { Window, Button, CheckBox, Label, TextBox, TextArea, ListBox, Canvas };

struct Item {
    HWND hwnd = nullptr;
    Kind kind = Kind::Window;
    int window = 0;  // id of the window a control belongs to
    bool alive = true;

    // Canvas only: everything is drawn into this bitmap, which WM_PAINT copies to the screen.
    HDC memDC = nullptr;
    HBITMAP bitmap = nullptr;
    HBITMAP previousBitmap = nullptr;
    int bufferWidth = 0;
    int bufferHeight = 0;
};

struct State {
    VM* vm = nullptr;
    bool ready = false;
    std::vector<Item> items;  // the item with id N is items[N - kFirstId]
    std::unordered_map<HWND, int> ids;

    Value dispatcher;
    bool hasDispatcher = false;
    size_t dispatcherPin = 0;

    int dpi = 96;
    HFONT font = nullptr;
    std::map<int, HFONT> canvasFonts;  // by logical size
    HWND timerWindow = nullptr;
    UINT_PTR nextTimer = 1;
    int openWindows = 0;
    int suppressChange = 0;  // > 0 while text is being set from code

    // An error raised by an Astra event handler cannot travel through Windows' own frames, so
    // it is parked here and thrown again once control is back in a GuiBackend function.
    bool hasThrow = false;
    AstraThrow pendingThrow;
    bool hasExit = false;
    int exitCode = 0;
};

State g;

// ---- Small helpers ----

int toDevice(double logical) { return static_cast<int>(std::lround(logical * g.dpi / 96.0)); }
int toLogical(int device) { return MulDiv(device, 96, g.dpi); }

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

COLORREF toColor(int64_t rgb) {
    return RGB(static_cast<int>((rgb >> 16) & 0xFF), static_cast<int>((rgb >> 8) & 0xFF), static_cast<int>(rgb & 0xFF));
}

Item* find(int id) {
    int index = id - kFirstId;
    if (index < 0 || static_cast<size_t>(index) >= g.items.size()) return nullptr;
    Item& item = g.items[static_cast<size_t>(index)];
    return item.alive ? &item : nullptr;
}

int idOf(HWND hwnd) {
    auto it = g.ids.find(hwnd);
    return it == g.ids.end() ? 0 : it->second;
}

Item& need(VM& vm, Value id) {
    Item* item = id.isInt() ? find(static_cast<int>(id.as.i)) : nullptr;
    if (item == nullptr) throw vm.runtimeError("this window or widget no longer exists");
    return *item;
}

Item& needCanvas(VM& vm, Value id) {
    Item& item = need(vm, id);
    if (item.kind != Kind::Canvas || item.memDC == nullptr) throw vm.runtimeError("this widget is not a canvas");
    return item;
}

int addItem(HWND hwnd, Kind kind, int window) {
    Item item;
    item.hwnd = hwnd;
    item.kind = kind;
    item.window = window;
    g.items.push_back(item);
    int id = static_cast<int>(g.items.size()) - 1 + kFirstId;
    g.ids[hwnd] = id;
    return id;
}

void releaseCanvas(Item& item) {
    if (item.memDC == nullptr) return;
    SelectObject(item.memDC, item.previousBitmap);
    DeleteObject(item.bitmap);
    DeleteDC(item.memDC);
    item.memDC = nullptr;
    item.bitmap = nullptr;
}

// ---- Events ----

Value dispatch(int id, const char* event, Value a = Value::null(), Value b = Value::null(), Value c = Value::null()) {
    if (!g.hasDispatcher || g.hasThrow || g.hasExit) return Value::null();
    try {
        Value args[5] = {Value::integer(id), Value::object(g.vm->intern(event)), a, b, c};
        return g.vm->call(g.dispatcher, 5, args);
    } catch (AstraThrow& thrown) {
        g.pendingThrow = thrown;
        g.hasThrow = true;
    } catch (AstraExit& exit) {
        g.exitCode = exit.code;
        g.hasExit = true;
    }
    return Value::null();
}

// Ends every GuiBackend function: reports an error that an event handler raised meanwhile.
Value done(Value result = Value::null()) {
    if (g.hasThrow) {
        g.hasThrow = false;
        AstraThrow thrown = g.pendingThrow;
        g.pendingThrow = AstraThrow();
        throw thrown;
    }
    if (g.hasExit) {
        g.hasExit = false;
        throw AstraExit{g.exitCode};
    }
    return result;
}

std::string keyName(WPARAM key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::string(1, static_cast<char>(key));
    if (key >= VK_F1 && key <= VK_F12) return "F" + std::to_string(key - VK_F1 + 1);
    switch (key) {
        case VK_LEFT: return "Left";
        case VK_RIGHT: return "Right";
        case VK_UP: return "Up";
        case VK_DOWN: return "Down";
        case VK_SPACE: return "Space";
        case VK_RETURN: return "Enter";
        case VK_ESCAPE: return "Escape";
        case VK_TAB: return "Tab";
        case VK_BACK: return "Backspace";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        case VK_SHIFT: return "Shift";
        case VK_CONTROL: return "Control";
        case VK_MENU: return "Alt";
        default: return "Key" + std::to_string(key);
    }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    int id = idOf(hwnd);
    switch (message) {
        case WM_COMMAND: {
            int control = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (lParam == 0) {
                // Sent by the keyboard handling in the message loop for Enter and Escape.
                if (control == IDOK) dispatch(id, "enter");
                else if (control == IDCANCEL) dispatch(id, "escape");
                return 0;
            }
            Item* item = find(control);
            if (item == nullptr) break;
            switch (item->kind) {
                case Kind::Button:
                case Kind::CheckBox:
                    if (code == BN_CLICKED) dispatch(control, "click");
                    break;
                case Kind::TextBox:
                case Kind::TextArea:
                    if (code == EN_CHANGE && g.suppressChange == 0) dispatch(control, "change");
                    break;
                case Kind::ListBox:
                    if (code == LBN_SELCHANGE) dispatch(control, "select");
                    else if (code == LBN_DBLCLK) dispatch(control, "activate");
                    break;
                default:
                    break;
            }
            return 0;
        }

        case WM_SIZE:
            if (id != 0 && wParam != SIZE_MINIMIZED)
                dispatch(id, "resize", Value::integer(toLogical(LOWORD(lParam))), Value::integer(toLogical(HIWORD(lParam))));
            return 0;

        case WM_CLOSE: {
            // A handler that returns false keeps the window open.
            Value answer = dispatch(id, "close");
            if (answer.isBool() && !answer.as.b) return 0;
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            if (id == 0) break;
            for (Item& item : g.items) {
                if (!item.alive || (item.window != id && item.hwnd != hwnd)) continue;
                releaseCanvas(item);
                item.alive = false;
                g.ids.erase(item.hwnd);
            }
            g.openWindows--;
            dispatch(id, "closed");
            if (g.openWindows <= 0) PostQuitMessage(0);
            return 0;
        }

        case WM_CTLCOLORSTATIC:
            // Labels and check boxes blend into the window's background.
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// Gives a canvas a backing bitmap of the given size, keeping what was already drawn.
void resizeCanvas(Item& item, int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (item.memDC != nullptr && width == item.bufferWidth && height == item.bufferHeight) return;

    HDC screen = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    ReleaseDC(nullptr, screen);
    HBITMAP previous = static_cast<HBITMAP>(SelectObject(memDC, bitmap));

    RECT all = {0, 0, width, height};
    FillRect(memDC, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    if (item.memDC != nullptr) BitBlt(memDC, 0, 0, item.bufferWidth, item.bufferHeight, item.memDC, 0, 0, SRCCOPY);

    releaseCanvas(item);
    item.memDC = memDC;
    item.bitmap = bitmap;
    item.previousBitmap = previous;
    item.bufferWidth = width;
    item.bufferHeight = height;
}

LRESULT CALLBACK canvasProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    int id = idOf(hwnd);
    Item* item = find(id);

    auto mouse = [&](const char* event, int button) {
        Value x = Value::integer(toLogical(static_cast<short>(LOWORD(lParam))));
        Value y = Value::integer(toLogical(static_cast<short>(HIWORD(lParam))));
        dispatch(id, event, x, y, Value::integer(button));
    };

    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (item != nullptr && item->memDC != nullptr)
                BitBlt(dc, 0, 0, item->bufferWidth, item->bufferHeight, item->memDC, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (item != nullptr) {
                resizeCanvas(*item, LOWORD(lParam), HIWORD(lParam));
                dispatch(id, "resize", Value::integer(toLogical(LOWORD(lParam))), Value::integer(toLogical(HIWORD(lParam))));
            }
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS;  // arrow keys and Tab reach the canvas

        case WM_LBUTTONDOWN: SetFocus(hwnd); SetCapture(hwnd); mouse("mouseDown", 1); return 0;
        case WM_RBUTTONDOWN: SetFocus(hwnd); mouse("mouseDown", 2); return 0;
        case WM_MBUTTONDOWN: SetFocus(hwnd); mouse("mouseDown", 3); return 0;
        case WM_LBUTTONUP: ReleaseCapture(); mouse("mouseUp", 1); return 0;
        case WM_RBUTTONUP: mouse("mouseUp", 2); return 0;
        case WM_MBUTTONUP: mouse("mouseUp", 3); return 0;
        case WM_MOUSEMOVE:
            mouse("mouseMove", (wParam & MK_LBUTTON) ? 1 : (wParam & MK_RBUTTON) ? 2 : (wParam & MK_MBUTTON) ? 3 : 0);
            return 0;

        case WM_KEYDOWN:
            dispatch(id, "keyDown", g.vm->str(keyName(wParam)));
            return 0;
        case WM_KEYUP:
            dispatch(id, "keyUp", g.vm->str(keyName(wParam)));
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK timerProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TIMER) {
        dispatch(0, "timer", Value::integer(static_cast<int64_t>(wParam)));
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void registerClass(const wchar_t* name, WNDPROC proc, HBRUSH background) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

// One-time setup, on first use so that programs without a GUI pay nothing for it.
void initialize(VM& vm) {
    if (g.ready) return;
    g.ready = true;
    g.vm = &vm;

    SetProcessDPIAware();
    HDC screen = GetDC(nullptr);
    g.dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    if (g.dpi <= 0) g.dpi = 96;

    INITCOMMONCONTROLSEX controls = {sizeof controls, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof metrics;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0))
        g.font = CreateFontIndirectW(&metrics.lfMessageFont);
    if (g.font == nullptr) g.font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    registerClass(kWindowClass, windowProc, GetSysColorBrush(COLOR_WINDOW));
    registerClass(kCanvasClass, canvasProc, nullptr);
    registerClass(kTimerClass, timerProc, nullptr);
    g.timerWindow = CreateWindowExW(0, kTimerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
}

SIZE textExtent(const std::wstring& text, HFONT font) {
    HDC screen = GetDC(nullptr);
    HFONT previous = static_cast<HFONT>(SelectObject(screen, font));
    RECT bounds = {0, 0, 0, 0};
    DrawTextW(screen, text.empty() ? L" " : text.c_str(), -1, &bounds, DT_CALCRECT | DT_NOPREFIX);
    SelectObject(screen, previous);
    ReleaseDC(nullptr, screen);
    return SIZE{bounds.right - bounds.left, bounds.bottom - bounds.top};
}

HFONT canvasFont(int logicalSize) {
    auto it = g.canvasFonts.find(logicalSize);
    if (it != g.canvasFonts.end()) return it->second;
    HFONT font = CreateFontW(-toDevice(logicalSize), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g.canvasFonts[logicalSize] = font;
    return font;
}

// Writes a bitmap as a 24-bit .bmp file. The bitmap must not be selected into a DC.
bool saveBitmap(HBITMAP bitmap, int width, int height, const std::string& path) {
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;  // bottom-up, the order .bmp files use
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;

    size_t rowBytes = (static_cast<size_t>(width) * 3 + 3) & ~static_cast<size_t>(3);
    std::vector<char> pixels(rowBytes * static_cast<size_t>(height));
    HDC screen = GetDC(nullptr);
    int copied = GetDIBits(screen, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (copied == 0) return false;

    BITMAPFILEHEADER header = {};
    header.bfType = 0x4D42;  // "BM"
    header.bfOffBits = sizeof header + sizeof info.bmiHeader;
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());

    std::ofstream out(widen(path), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&header), sizeof header);
    out.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof info.bmiHeader);
    out.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));
    out.flush();
    return static_cast<bool>(out);
}

// Draws on a canvas with a pen and brush that are released afterwards.
struct Drawing {
    Item& item;
    HPEN pen = nullptr;
    HBRUSH brush = nullptr;
    HGDIOBJ previousPen = nullptr;
    HGDIOBJ previousBrush = nullptr;

    // `outline` < 0 means no outline; `fill` < 0 means no fill.
    Drawing(Item& canvas, int64_t outline, double lineWidth, int64_t fill) : item(canvas) {
        if (outline >= 0) pen = CreatePen(PS_SOLID, std::max(1, toDevice(lineWidth)), toColor(outline));
        if (fill >= 0) brush = CreateSolidBrush(toColor(fill));
        previousPen = SelectObject(item.memDC, pen != nullptr ? pen : GetStockObject(NULL_PEN));
        previousBrush = SelectObject(item.memDC, brush != nullptr ? brush : GetStockObject(NULL_BRUSH));
    }
    ~Drawing() {
        SelectObject(item.memDC, previousPen);
        SelectObject(item.memDC, previousBrush);
        if (pen != nullptr) DeleteObject(pen);
        if (brush != nullptr) DeleteObject(brush);
        InvalidateRect(item.hwnd, nullptr, FALSE);
    }
};

int px(VM& vm, Value v, const char* what) { return toDevice(vm.toNumber(v, what)); }

Value pairOf(VM& vm, int64_t first, int64_t second) {
    ObjList* list = vm.alloc<ObjList>();
    list->items.push_back(Value::integer(first));
    list->items.push_back(Value::integer(second));
    return Value::object(list);
}

// ---- The GuiBackend class ----

void registerNatives(VM& vm) {
    ObjClass* backend = vm.alloc<ObjClass>(vm.intern("GuiBackend"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        backend->statics.set(vm.intern(name),
                             Value::object(vm.alloc<ObjNative>(std::string("GuiBackend.") + name, minArgs, maxArgs, fn)));
    };

    // setDispatcher(fn): fn(id, event, a, b, c) receives every event.
    def("setDispatcher", 1, 1, [](VM& vm, int, Value* args) -> Value {
        initialize(vm);
        if (g.hasDispatcher) {
            vm.pinned[g.dispatcherPin] = args[0];
        } else {
            g.dispatcherPin = vm.pinned.size();
            vm.pinned.push_back(args[0]);
        }
        g.dispatcher = args[0];
        g.hasDispatcher = true;
        return Value::null();
    });

    // window(title, width, height) -> id. The size is that of the content area. Starts hidden.
    def("window", 3, 3, [](VM& vm, int, Value* args) -> Value {
        initialize(vm);
        std::wstring title = widen(vm.toStr(args[0], "window title"));
        DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        RECT frame = {0, 0, px(vm, args[1], "window width"), px(vm, args[2], "window height")};
        AdjustWindowRectEx(&frame, style, FALSE, 0);
        HWND hwnd = CreateWindowExW(0, kWindowClass, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left,
                                    frame.bottom - frame.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (hwnd == nullptr) throw vm.runtimeError("could not create a window");
        g.openWindows++;
        return done(Value::integer(addItem(hwnd, Kind::Window, 0)));
    });

    // control(kind, window, text) -> id. Created with no size; move() places it.
    def("control", 3, 3, [](VM& vm, int, Value* args) -> Value {
        initialize(vm);
        const std::string& kindName = vm.toStr(args[0], "control kind");
        Item& window = need(vm, args[1]);
        if (window.kind != Kind::Window) throw vm.runtimeError("a widget's parent must be a window");
        int windowId = static_cast<int>(args[1].as.i);
        HWND parent = window.hwnd;
        std::string text = vm.toStr(args[2], "control text");

        Kind kind = Kind::Label;
        const wchar_t* cls = L"STATIC";
        DWORD style = WS_CHILD | WS_VISIBLE;
        DWORD exStyle = 0;
        if (kindName == "button") {
            kind = Kind::Button, cls = L"BUTTON", style |= BS_PUSHBUTTON | WS_TABSTOP;
        } else if (kindName == "checkbox") {
            kind = Kind::CheckBox, cls = L"BUTTON", style |= BS_AUTOCHECKBOX | WS_TABSTOP;
        } else if (kindName == "label") {
            kind = Kind::Label, cls = L"STATIC", style |= SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE;
        } else if (kindName == "textbox") {
            kind = Kind::TextBox, cls = L"EDIT", style |= ES_AUTOHSCROLL | WS_TABSTOP, exStyle = WS_EX_CLIENTEDGE;
        } else if (kindName == "textarea") {
            kind = Kind::TextArea, cls = L"EDIT", exStyle = WS_EX_CLIENTEDGE;
            style |= ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP;
            text = replaceAll(replaceAll(text, "\r\n", "\n"), "\n", "\r\n");
        } else if (kindName == "listbox") {
            kind = Kind::ListBox, cls = L"LISTBOX", exStyle = WS_EX_CLIENTEDGE;
            style |= LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP;
        } else if (kindName == "canvas") {
            kind = Kind::Canvas, cls = kCanvasClass, style |= WS_TABSTOP;
        } else {
            throw vm.runtimeError("unknown kind of widget '" + kindName + "'");
        }
        // In a button, '&' would mark a keyboard shortcut instead of being shown.
        if (kind == Kind::Button || kind == Kind::CheckBox) text = replaceAll(text, "&", "&&");

        int id = static_cast<int>(g.items.size()) + kFirstId;
        HWND hwnd = CreateWindowExW(exStyle, cls, widen(text).c_str(), style, 0, 0, 0, 0, parent,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (hwnd == nullptr) throw vm.runtimeError("could not create a " + kindName);
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
        addItem(hwnd, kind, windowId);
        if (kind == Kind::Canvas) resizeCanvas(g.items.back(), 1, 1);
        return done(Value::integer(id));
    });

    def("setText", 2, 2, [](VM& vm, int, Value* args) -> Value {
        Item& item = need(vm, args[0]);
        std::string text = vm.toStr(args[1], "text");
        if (item.kind == Kind::TextArea) text = replaceAll(replaceAll(text, "\r\n", "\n"), "\n", "\r\n");
        if (item.kind == Kind::Button || item.kind == Kind::CheckBox) text = replaceAll(text, "&", "&&");
        g.suppressChange++;
        SetWindowTextW(item.hwnd, widen(text).c_str());
        g.suppressChange--;
        return done();
    });

    def("getText", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Item& item = need(vm, args[0]);
        int length = GetWindowTextLengthW(item.hwnd);
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        int copied = GetWindowTextW(item.hwnd, text.data(), length + 1);
        text.resize(static_cast<size_t>(std::max(copied, 0)));
        std::string utf8 = narrow(text);
        if (item.kind == Kind::TextArea) utf8 = replaceAll(utf8, "\r\n", "\n");
        if (item.kind == Kind::Button || item.kind == Kind::CheckBox) utf8 = replaceAll(utf8, "&&", "&");
        return done(vm.str(std::move(utf8)));
    });

    // move(id, x, y, width, height): a control's position within its window.
    def("move", 5, 5, [](VM& vm, int, Value* args) -> Value {
        Item& item = need(vm, args[0]);
        SetWindowPos(item.hwnd, nullptr, px(vm, args[1], "x"), px(vm, args[2], "y"), std::max(0, px(vm, args[3], "width")),
                     std::max(0, px(vm, args[4], "height")), SWP_NOZORDER | SWP_NOACTIVATE);
        return done();
    });

    // windowSize(id) -> [width, height] of the content area.
    def("windowSize", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Item& item = need(vm, args[0]);
        RECT client;
        GetClientRect(item.hwnd, &client);
        return done(pairOf(vm, toLogical(client.right), toLogical(client.bottom)));
    });

    def("setWindowSize", 3, 3, [](VM& vm, int, Value* args) -> Value {
        Item& item = need(vm, args[0]);
        RECT frame = {0, 0, px(vm, args[1], "width"), px(vm, args[2], "height")};
        AdjustWindowRectEx(&frame, static_cast<DWORD>(GetWindowLongPtrW(item.hwnd, GWL_STYLE)), FALSE, 0);
        SetWindowPos(item.hwnd, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return done();
    });

    def("show", 2, 2, [](VM& vm, int, Value* args) -> Value {
        Item& item = need(vm, args[0]);
        ShowWindow(item.hwnd, args[1].truthy() ? SW_SHOW : SW_HIDE);
        if (args[1].truthy() && item.kind == Kind::Window) UpdateWindow(item.hwnd);
        return done();
    });

    def("enable", 2, 2, [](VM& vm, int, Value* args) -> Value {
        EnableWindow(need(vm, args[0]).hwnd, args[1].truthy());
        return done();
    });

    def("focus", 1, 1, [](VM& vm, int, Value* args) -> Value {
        SetFocus(need(vm, args[0]).hwnd);
        return done();
    });

    def("close", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Item* item = args[0].isInt() ? find(static_cast<int>(args[0].as.i)) : nullptr;
        if (item != nullptr && item->kind == Kind::Window) DestroyWindow(item->hwnd);
        (void)vm;
        return done();
    });

    // click(id): as if the user had clicked the button or check box.
    def("click", 1, 1, [](VM& vm, int, Value* args) -> Value {
        Item& item = need(vm, args[0]);
        if (item.kind != Kind::Button && item.kind != Kind::CheckBox) throw vm.runtimeError("only buttons and check boxes can be clicked");
        if (!IsWindowEnabled(item.hwnd)) return done();
        if (item.kind == Kind::CheckBox) {
            bool checked = SendMessageW(item.hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SendMessageW(item.hwnd, BM_SETCHECK, checked ? BST_UNCHECKED : BST_CHECKED, 0);
        }
        SendMessageW(GetParent(item.hwnd), WM_COMMAND, MAKEWPARAM(static_cast<int>(args[0].as.i), BN_CLICKED),
                     reinterpret_cast<LPARAM>(item.hwnd));
        return done();
    });

    def("isChecked", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return done(Value::boolean(SendMessageW(need(vm, args[0]).hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED));
    });

    def("setChecked", 2, 2, [](VM& vm, int, Value* args) -> Value {
        SendMessageW(need(vm, args[0]).hwnd, BM_SETCHECK, args[1].truthy() ? BST_CHECKED : BST_UNCHECKED, 0);
        return done();
    });

    // ---- List boxes ----

    def("listAdd", 2, 2, [](VM& vm, int, Value* args) -> Value {
        SendMessageW(need(vm, args[0]).hwnd, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(vm.toStr(args[1], "list item")).c_str()));
        return done();
    });

    def("listRemove", 2, 2, [](VM& vm, int, Value* args) -> Value {
        SendMessageW(need(vm, args[0]).hwnd, LB_DELETESTRING, static_cast<WPARAM>(vm.toInt(args[1], "list index")), 0);
        return done();
    });

    def("listClear", 1, 1, [](VM& vm, int, Value* args) -> Value {
        SendMessageW(need(vm, args[0]).hwnd, LB_RESETCONTENT, 0, 0);
        return done();
    });

    // listSelected(id) -> index, or -1 when nothing is selected.
    def("listSelected", 1, 1, [](VM& vm, int, Value* args) -> Value {
        LRESULT index = SendMessageW(need(vm, args[0]).hwnd, LB_GETCURSEL, 0, 0);
        return done(Value::integer(index == LB_ERR ? -1 : static_cast<int64_t>(index)));
    });

    def("listSelect", 2, 2, [](VM& vm, int, Value* args) -> Value {
        SendMessageW(need(vm, args[0]).hwnd, LB_SETCURSEL, static_cast<WPARAM>(vm.toInt(args[1], "list index")), 0);
        return done();
    });

    // textSize(text) -> [width, height] in the font the widgets use.
    def("textSize", 1, 1, [](VM& vm, int, Value* args) -> Value {
        initialize(vm);
        SIZE size = textExtent(widen(vm.toStr(args[0], "text")), g.font);
        return done(pairOf(vm, toLogical(size.cx) + 1, toLogical(size.cy) + 1));
    });

    // ---- Running ----

    // run(): handles events until every window has been closed.
    def("run", 0, 0, [](VM& vm, int, Value*) -> Value {
        initialize(vm);
        vm.output->flush();
        MSG message;
        while (g.openWindows > 0 && !g.hasThrow && !g.hasExit) {
            if (GetMessageW(&message, nullptr, 0, 0) <= 0) break;
            // Tab, Enter and Escape get their usual meaning inside our windows.
            HWND root = GetAncestor(message.hwnd, GA_ROOT);
            if (root != nullptr && idOf(root) != 0 && IsDialogMessageW(root, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return done();
    });

    // quit(): closes every window, which ends run().
    def("quit", 0, 0, [](VM&, int, Value*) -> Value {
        std::vector<HWND> windows;
        for (const Item& item : g.items)
            if (item.alive && item.kind == Kind::Window) windows.push_back(item.hwnd);
        for (HWND hwnd : windows) DestroyWindow(hwnd);
        return done();
    });

    // timer(milliseconds) -> id. Fires "timer" events on id 0 until cancelled.
    def("timer", 1, 1, [](VM& vm, int, Value* args) -> Value {
        initialize(vm);
        int64_t ms = vm.toInt(args[0], "timer interval");
        if (ms < 1) ms = 1;
        UINT_PTR id = g.nextTimer++;
        SetTimer(g.timerWindow, id, static_cast<UINT>(std::min<int64_t>(ms, 0x7FFFFFFF)), nullptr);
        return done(Value::integer(static_cast<int64_t>(id)));
    });

    def("cancelTimer", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (g.timerWindow != nullptr) KillTimer(g.timerWindow, static_cast<UINT_PTR>(vm.toInt(args[0], "timer")));
        return done();
    });

    def("alert", 2, 2, [](VM& vm, int, Value* args) -> Value {
        initialize(vm);
        MessageBoxW(GetActiveWindow(), widen(vm.toStr(args[1], "message")).c_str(), widen(vm.toStr(args[0], "title")).c_str(),
                    MB_OK | MB_ICONINFORMATION);
        return done();
    });

    def("confirm", 2, 2, [](VM& vm, int, Value* args) -> Value {
        initialize(vm);
        int answer = MessageBoxW(GetActiveWindow(), widen(vm.toStr(args[1], "message")).c_str(),
                                 widen(vm.toStr(args[0], "title")).c_str(), MB_YESNO | MB_ICONQUESTION);
        return done(Value::boolean(answer == IDYES));
    });

    // ---- Canvas drawing. Colours are 0xRRGGBB. ----

    def("canvasClear", 2, 2, [](VM& vm, int, Value* args) -> Value {
        Item& item = needCanvas(vm, args[0]);
        HBRUSH brush = CreateSolidBrush(toColor(vm.toInt(args[1], "color")));
        RECT all = {0, 0, item.bufferWidth, item.bufferHeight};
        FillRect(item.memDC, &all, brush);
        DeleteObject(brush);
        InvalidateRect(item.hwnd, nullptr, FALSE);
        return done();
    });

    // canvasLine(id, x1, y1, x2, y2, color, width)
    def("canvasLine", 7, 7, [](VM& vm, int, Value* args) -> Value {
        Drawing drawing(needCanvas(vm, args[0]), vm.toInt(args[5], "color"), vm.toNumber(args[6], "line width"), -1);
        MoveToEx(drawing.item.memDC, px(vm, args[1], "x1"), px(vm, args[2], "y1"), nullptr);
        LineTo(drawing.item.memDC, px(vm, args[3], "x2"), px(vm, args[4], "y2"));
        return done();
    });

    // canvasRect / canvasEllipse(id, x, y, width, height, color, lineWidth, filled)
    def("canvasRect", 8, 8, [](VM& vm, int, Value* args) -> Value {
        int64_t color = vm.toInt(args[5], "color");
        bool filled = args[7].truthy();
        Drawing drawing(needCanvas(vm, args[0]), filled ? -1 : color, vm.toNumber(args[6], "line width"), filled ? color : -1);
        int x = px(vm, args[1], "x"), y = px(vm, args[2], "y");
        // With no outline, GDI stops one pixel short of the right and bottom edges.
        int extra = filled ? 1 : 0;
        Rectangle(drawing.item.memDC, x, y, x + px(vm, args[3], "width") + extra, y + px(vm, args[4], "height") + extra);
        return done();
    });

    def("canvasEllipse", 8, 8, [](VM& vm, int, Value* args) -> Value {
        int64_t color = vm.toInt(args[5], "color");
        bool filled = args[7].truthy();
        // A filled shape also gets an outline in its own colour, for smooth edges.
        Drawing drawing(needCanvas(vm, args[0]), color, filled ? 1 : vm.toNumber(args[6], "line width"), filled ? color : -1);
        int x = px(vm, args[1], "x"), y = px(vm, args[2], "y");
        Ellipse(drawing.item.memDC, x, y, x + px(vm, args[3], "width"), y + px(vm, args[4], "height"));
        return done();
    });

    // canvasText(id, x, y, text, color, size)
    def("canvasText", 6, 6, [](VM& vm, int, Value* args) -> Value {
        Item& item = needCanvas(vm, args[0]);
        std::wstring text = widen(vm.toStr(args[3], "text"));
        HFONT font = canvasFont(static_cast<int>(std::clamp<int64_t>(vm.toInt(args[5], "text size"), 4, 400)));
        HFONT previous = static_cast<HFONT>(SelectObject(item.memDC, font));
        SetBkMode(item.memDC, TRANSPARENT);
        SetTextColor(item.memDC, toColor(vm.toInt(args[4], "color")));
        TextOutW(item.memDC, px(vm, args[1], "x"), px(vm, args[2], "y"), text.c_str(), static_cast<int>(text.size()));
        SelectObject(item.memDC, previous);
        InvalidateRect(item.hwnd, nullptr, FALSE);
        return done();
    });

    // saveImage(id, path): a picture of a window's contents, or of a canvas, as a .bmp file.
    def("saveImage", 2, 2, [](VM& vm, int, Value* args) -> Value {
        Item& item = need(vm, args[0]);
        const std::string& path = vm.toStr(args[1], "file path");
        bool saved = false;

        if (item.kind == Kind::Canvas && item.memDC != nullptr) {
            SelectObject(item.memDC, item.previousBitmap);
            saved = saveBitmap(item.bitmap, item.bufferWidth, item.bufferHeight, path);
            SelectObject(item.memDC, item.bitmap);
        } else {
            RECT client;
            GetClientRect(item.hwnd, &client);
            int width = std::max<int>(client.right, 1), height = std::max<int>(client.bottom, 1);
            HDC screen = GetDC(nullptr);
            HDC memDC = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
            ReleaseDC(nullptr, screen);
            HGDIOBJ previous = SelectObject(memDC, bitmap);
            UpdateWindow(item.hwnd);
            PrintWindow(item.hwnd, memDC, PW_CLIENTONLY | 0x2 /* PW_RENDERFULLCONTENT */);
            SelectObject(memDC, previous);
            saved = saveBitmap(bitmap, width, height, path);
            DeleteObject(bitmap);
            DeleteDC(memDC);
        }
        if (!saved) throw vm.runtimeError("cannot write image '" + path + "'");
        return done();
    });

    vm.defineGlobal("GuiBackend", Value::object(backend));
}

}  // namespace

void registerGui(VM& vm) { registerNatives(vm); }

}  // namespace astra

#else  // not Windows

namespace astra {

// The GUI library needs Windows. Elsewhere `import "gui"` fails on its first use of GuiBackend.
void registerGui(VM&) {}

}  // namespace astra

#endif
