// Jade's native layer on Windows: see platform.h.
//
// A plain Win32 window class, a window procedure that turns Windows' messages into raw
// events, and one OpenGL 3.3 context that is made current on whichever window is being
// drawn. No library between this and the system.

#ifdef _WIN32

#include "gl.h"  // brings in windows.h

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "platform.h"

namespace astra::jade {

namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

std::string narrow(const wchar_t* wide, int length) {
    if (length <= 0) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, length, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

const wchar_t* const kWindowClass = L"AstraJadeWindow";
const UINT_PTR kIdleTimer = 1;

// ---- Keys ----

// Which key a message is about, by where the key is on the keyboard (its scan code), so
// that it is the same key whatever the layout: on a French keyboard the key that types "a"
// is Key::Q, and the letter that goes with the event says "A".
Key keyOf(WPARAM vk, LPARAM lParam) {
    const int scan = (lParam >> 16) & 0xFF;
    const bool extended = (lParam & (1 << 24)) != 0;

    // A few keys are told apart best by what Windows calls them.
    switch (vk) {
        case VK_PAUSE: return Key::Pause;
        case VK_NUMLOCK: return Key::NumLock;
        case VK_SNAPSHOT: return Key::PrintScreen;
        case VK_APPS: return Key::Menu;
        case VK_LWIN: return Key::MetaLeft;
        case VK_RWIN: return Key::MetaRight;
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(vk - VK_F1));

    if (extended) {
        switch (scan) {
            case 0x1C: return Key::NumpadEnter;
            case 0x1D: return Key::ControlRight;
            case 0x38: return Key::AltRight;
            case 0x35: return Key::NumpadDivide;
            case 0x52: return Key::Insert;
            case 0x53: return Key::Delete;
            case 0x4D: return Key::Right;
            case 0x4B: return Key::Left;
            case 0x50: return Key::Down;
            case 0x48: return Key::Up;
            case 0x49: return Key::PageUp;
            case 0x51: return Key::PageDown;
            case 0x47: return Key::Home;
            case 0x4F: return Key::End;
            default: break;
        }
    } else {
        static const struct {
            int scan;
            Key key;
        } table[] = {
            {0x1E, Key::A}, {0x30, Key::B}, {0x2E, Key::C}, {0x20, Key::D}, {0x12, Key::E}, {0x21, Key::F}, {0x22, Key::G}, {0x23, Key::H},
            {0x17, Key::I}, {0x24, Key::J}, {0x25, Key::K}, {0x26, Key::L}, {0x32, Key::M}, {0x31, Key::N}, {0x18, Key::O}, {0x19, Key::P},
            {0x10, Key::Q}, {0x13, Key::R}, {0x1F, Key::S}, {0x14, Key::T}, {0x16, Key::U}, {0x2F, Key::V}, {0x11, Key::W}, {0x2D, Key::X},
            {0x15, Key::Y}, {0x2C, Key::Z},
            {0x0B, Key::Digit0}, {0x02, Key::Digit1}, {0x03, Key::Digit2}, {0x04, Key::Digit3}, {0x05, Key::Digit4}, {0x06, Key::Digit5},
            {0x07, Key::Digit6}, {0x08, Key::Digit7}, {0x09, Key::Digit8}, {0x0A, Key::Digit9},
            {0x39, Key::Space}, {0x28, Key::Apostrophe}, {0x33, Key::Comma}, {0x0C, Key::Minus}, {0x34, Key::Period}, {0x35, Key::Slash},
            {0x27, Key::Semicolon}, {0x0D, Key::Equal}, {0x1A, Key::LeftBracket}, {0x2B, Key::Backslash}, {0x1B, Key::RightBracket},
            {0x29, Key::GraveAccent}, {0x01, Key::Escape}, {0x1C, Key::Enter}, {0x0F, Key::Tab}, {0x0E, Key::Backspace},
            {0x3A, Key::CapsLock}, {0x46, Key::ScrollLock},
            {0x52, Key::Numpad0}, {0x4F, Key::Numpad1}, {0x50, Key::Numpad2}, {0x51, Key::Numpad3}, {0x4B, Key::Numpad4}, {0x4C, Key::Numpad5},
            {0x4D, Key::Numpad6}, {0x47, Key::Numpad7}, {0x48, Key::Numpad8}, {0x49, Key::Numpad9}, {0x53, Key::NumpadDecimal},
            {0x37, Key::NumpadMultiply}, {0x4A, Key::NumpadSubtract}, {0x4E, Key::NumpadAdd},
            {0x2A, Key::ShiftLeft}, {0x36, Key::ShiftRight}, {0x1D, Key::ControlLeft}, {0x38, Key::AltLeft},
        };
        for (const auto& entry : table)
            if (entry.scan == scan) return entry.key;
    }

    // A message with no scan code (one a program made up): by Windows' name for the key.
    if (vk >= 'A' && vk <= 'Z') return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<Key>(static_cast<int>(Key::Digit0) + static_cast<int>(vk - '0'));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return static_cast<Key>(static_cast<int>(Key::Numpad0) + static_cast<int>(vk - VK_NUMPAD0));
    switch (vk) {
        case VK_SPACE: return Key::Space;
        case VK_ESCAPE: return Key::Escape;
        case VK_RETURN: return Key::Enter;
        case VK_TAB: return Key::Tab;
        case VK_BACK: return Key::Backspace;
        case VK_INSERT: return Key::Insert;
        case VK_DELETE: return Key::Delete;
        case VK_RIGHT: return Key::Right;
        case VK_LEFT: return Key::Left;
        case VK_DOWN: return Key::Down;
        case VK_UP: return Key::Up;
        case VK_PRIOR: return Key::PageUp;
        case VK_NEXT: return Key::PageDown;
        case VK_HOME: return Key::Home;
        case VK_END: return Key::End;
        case VK_CAPITAL: return Key::CapsLock;
        case VK_SCROLL: return Key::ScrollLock;
        case VK_SHIFT: case VK_LSHIFT: return Key::ShiftLeft;
        case VK_RSHIFT: return Key::ShiftRight;
        case VK_CONTROL: case VK_LCONTROL: return Key::ControlLeft;
        case VK_RCONTROL: return Key::ControlRight;
        case VK_MENU: case VK_LMENU: return Key::AltLeft;
        case VK_RMENU: return Key::AltRight;
        case VK_DECIMAL: return Key::NumpadDecimal;
        case VK_DIVIDE: return Key::NumpadDivide;
        case VK_MULTIPLY: return Key::NumpadMultiply;
        case VK_SUBTRACT: return Key::NumpadSubtract;
        case VK_ADD: return Key::NumpadAdd;
        case VK_OEM_COMMA: return Key::Comma;
        case VK_OEM_MINUS: return Key::Minus;
        case VK_OEM_PERIOD: return Key::Period;
        case VK_OEM_PLUS: return Key::Equal;
        default: return Key::Unknown;
    }
}

// The letter or digit a key types in the layout in use, in upper case; 0 for any other key.
uint32_t letterOf(WPARAM vk) {
    const UINT typed = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0x7FFFFFFF;  // the top bit marks a dead key
    if (typed == 0) return 0;
    wchar_t upper = static_cast<wchar_t>(typed);
    CharUpperBuffW(&upper, 1);
    return IsCharAlphaNumericW(upper) ? static_cast<uint32_t>(upper) : 0;
}

int modifiersNow() {
    int bits = 0;
    if (GetKeyState(VK_SHIFT) < 0) bits |= kShift;
    if (GetKeyState(VK_CONTROL) < 0) bits |= kControl;
    if (GetKeyState(VK_MENU) < 0) bits |= kAlt;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) bits |= kMeta;
    return bits;
}

int buttonsOf(WPARAM wParam) {
    int bits = 0;
    if (wParam & MK_LBUTTON) bits |= 1;
    if (wParam & MK_RBUTTON) bits |= 2;
    if (wParam & MK_MBUTTON) bits |= 4;
    if (wParam & MK_XBUTTON1) bits |= 8;
    if (wParam & MK_XBUTTON2) bits |= 16;
    return bits;
}

class WinPlatform;

class WinWindow final : public PlatformWindow {
public:
    WinPlatform& platform;
    HWND hwnd = nullptr;
    HDC dc = nullptr;
    DWORD style = 0, exStyle = 0;
    double fixedScale = 0;  // a scale given by a test, in place of the screen's
    int minWidth = 0, minHeight = 0, maxWidth = 0, maxHeight = 0;
    Cursor cursor = Cursor::Default;
    PointerMode pointerMode = PointerMode::Normal;
    bool tracking = false;      // asked to be told when the pointer leaves
    bool inside = false;
    int lastX = 0, lastY = 0;
    bool haveLast = false;
    wchar_t highSurrogate = 0;
    HICON icon = nullptr;

    explicit WinWindow(WinPlatform& p) : platform(p) {}
    ~WinWindow() override;

    void emit(const RawEvent& event) {
        if (onEvent) onEvent(event);
    }

    void show() override {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    void hide() override { ShowWindow(hwnd, SW_HIDE); }
    bool visible() const override { return IsWindowVisible(hwnd) != 0; }
    void setTitle(const std::string& title) override { SetWindowTextW(hwnd, widen(title).c_str()); }
    int width() const override {
        RECT r;
        GetClientRect(hwnd, &r);
        return r.right;
    }
    int height() const override {
        RECT r;
        GetClientRect(hwnd, &r);
        return r.bottom;
    }
    // From the size of what can be drawn on to the size of the whole window.
    SIZE outerSize(int width, int height) const {
        RECT r = {0, 0, width, height};
        AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, GetDpiForWindow(hwnd));
        return SIZE{r.right - r.left, r.bottom - r.top};
    }
    void setSize(int width, int height) override {
        const SIZE outer = outerSize(std::max(width, 1), std::max(height, 1));
        SetWindowPos(hwnd, nullptr, 0, 0, outer.cx, outer.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void position(int& x, int& y) const override {
        RECT r;
        GetWindowRect(hwnd, &r);
        x = r.left;
        y = r.top;
    }
    void setPosition(int x, int y) override { SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }
    void setLimits(int minW, int minH, int maxW, int maxH) override {
        minWidth = minW;
        minHeight = minH;
        maxWidth = maxW;
        maxHeight = maxH;
    }
    void setResizable(bool resizable) override {
        const DWORD sizing = WS_THICKFRAME | WS_MAXIMIZEBOX;
        style = resizable ? (style | sizing) : (style & ~sizing);
        SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(style | (visible() ? WS_VISIBLE : 0)));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    void toFront() override {
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    }
    void requestAttention() override { FlashWindow(hwnd, TRUE); }
    double scale() const override { return fixedScale > 0 ? fixedScale : GetDpiForWindow(hwnd) / 96.0; }
    bool focused() const override { return GetFocus() == hwnd; }

    void setIcon(const std::vector<uint8_t>& rgba, int width, int height) override {
        if (width <= 0 || height <= 0 || rgba.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4) return;
        BITMAPV5HEADER header = {};
        header.bV5Size = sizeof header;
        header.bV5Width = width;
        header.bV5Height = -height;
        header.bV5Planes = 1;
        header.bV5BitCount = 32;
        header.bV5Compression = BI_BITFIELDS;
        header.bV5RedMask = 0x00FF0000;
        header.bV5GreenMask = 0x0000FF00;
        header.bV5BlueMask = 0x000000FF;
        header.bV5AlphaMask = 0xFF000000;
        uint8_t* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (color == nullptr) return;
        for (size_t i = 0; i < static_cast<size_t>(width) * static_cast<size_t>(height); i++) {
            bits[i * 4] = rgba[i * 4 + 2];
            bits[i * 4 + 1] = rgba[i * 4 + 1];
            bits[i * 4 + 2] = rgba[i * 4];
            bits[i * 4 + 3] = rgba[i * 4 + 3];
        }
        HBITMAP mask = CreateBitmap(width, height, 1, 1, nullptr);
        ICONINFO info = {};
        info.fIcon = TRUE;
        info.hbmColor = color;
        info.hbmMask = mask;
        HICON made = CreateIconIndirect(&info);
        DeleteObject(color);
        DeleteObject(mask);
        if (made == nullptr) return;
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(made));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(made));
        if (icon != nullptr) DestroyIcon(icon);
        icon = made;
    }

    HCURSOR systemCursor() const {
        if (pointerMode != PointerMode::Normal) return nullptr;
        switch (cursor) {
            case Cursor::ResizeH: return LoadCursorW(nullptr, IDC_SIZEWE);
            case Cursor::ResizeV: return LoadCursorW(nullptr, IDC_SIZENS);
            case Cursor::Text: return LoadCursorW(nullptr, IDC_IBEAM);
            case Cursor::Hand: return LoadCursorW(nullptr, IDC_HAND);
            case Cursor::Move: return LoadCursorW(nullptr, IDC_SIZEALL);
            case Cursor::ResizeNwse: return LoadCursorW(nullptr, IDC_SIZENWSE);
            case Cursor::ResizeNesw: return LoadCursorW(nullptr, IDC_SIZENESW);
            case Cursor::Crosshair: return LoadCursorW(nullptr, IDC_CROSS);
            default: return LoadCursorW(nullptr, IDC_ARROW);
        }
    }
    bool pointerOverClient() const {
        POINT p;
        if (!GetCursorPos(&p) || WindowFromPoint(p) != hwnd) return false;
        ScreenToClient(hwnd, &p);
        RECT r;
        GetClientRect(hwnd, &r);
        return PtInRect(&r, p) != 0;
    }
    void setCursor(Cursor shape) override {
        cursor = shape;
        if (pointerOverClient()) SetCursor(systemCursor());
    }
    void centre(POINT& client) const {
        RECT r;
        GetClientRect(hwnd, &r);
        client.x = r.right / 2;
        client.y = r.bottom / 2;
    }
    void setPointerMode(PointerMode mode) override {
        if (mode == pointerMode) return;
        pointerMode = mode;
        if (mode == PointerMode::Locked) {
            // Held in the middle of the window: each movement is reported and the pointer put back.
            RECT r;
            GetClientRect(hwnd, &r);
            MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
            ClipCursor(&r);
            POINT middle;
            centre(middle);
            lastX = middle.x;
            lastY = middle.y;
            ClientToScreen(hwnd, &middle);
            SetCursorPos(middle.x, middle.y);
        } else {
            ClipCursor(nullptr);
        }
        if (pointerOverClient()) SetCursor(systemCursor());
    }

    void postForTest(const std::string& what) override {
        if (what == "moveBegan") PostMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
        else if (what == "moveEnded") PostMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
        else if (what == "close") PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    bool makeCurrent() override;
    void swapBuffers() override { SwapBuffers(dc); }

    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
};

class WinPlatform final : public Platform {
public:
    HINSTANCE instance = GetModuleHandleW(nullptr);
    DWORD thread = GetCurrentThreadId();
    HWND helper = nullptr;  // never shown: what the context is current on when no window is being drawn
    HDC helperDc = nullptr;
    HGLRC context = nullptr;
    int pixelFormat = 0;
    HDC currentDc = nullptr;
    std::map<HWND, WinWindow*> windows;

    bool start(std::string& error) {
        // Sizes follow each screen's own density, and Windows tells us when a window moves
        // to a screen of another.
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof wc;
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = &WinPlatform::windowProc;
        wc.hInstance = instance;
        wc.hCursor = nullptr;  // set as the pointer moves
        // A program built with --icon carries its icon as resource 1.
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        if (wc.hIcon == nullptr) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&wc)) {
            error = "cannot register a window class";
            return false;
        }

        helper = CreateWindowExW(0, kWindowClass, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, instance, nullptr);
        if (helper == nullptr) {
            error = "cannot create a window (is there a desktop to put it on?)";
            return false;
        }
        helperDc = GetDC(helper);
        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof pfd;
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        pixelFormat = ChoosePixelFormat(helperDc, &pfd);
        if (pixelFormat == 0 || !SetPixelFormat(helperDc, pixelFormat, &pfd)) {
            error = "no pixel format that OpenGL can draw in";
            return false;
        }

        // A context of the old kind is needed first, only to ask for the function that makes a new one.
        HGLRC legacy = wglCreateContext(helperDc);
        if (legacy == nullptr || !wglMakeCurrent(helperDc, legacy)) {
            error = "cannot create an OpenGL context";
            return false;
        }
        using CreateContextAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
        auto createContextAttribs = reinterpret_cast<CreateContextAttribs>(reinterpret_cast<void*>(wglGetProcAddress("wglCreateContextAttribsARB")));
        if (createContextAttribs != nullptr) {
            const int attributes[] = {0x2091, 3, 0x2092, 3, 0x9126, 0x00000001, 0};  // major, minor, core profile
            context = createContextAttribs(helperDc, nullptr, attributes);
        }
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(legacy);
        if (context == nullptr || !wglMakeCurrent(helperDc, context)) {
            error = "OpenGL 3.3 is not available on this machine";
            return false;
        }
        currentDc = helperDc;
        const char* missing = nullptr;
        if (!loadGl([](const char* name) -> void* { return reinterpret_cast<void*>(wglGetProcAddress(name)); }, missing)) {
            error = std::string("the OpenGL driver lacks ") + missing;
            return false;
        }
        // Showing a picture waits for the screen's next refresh, which paces an animation.
        using SwapInterval = BOOL(WINAPI*)(int);
        if (auto swapInterval = reinterpret_cast<SwapInterval>(reinterpret_cast<void*>(wglGetProcAddress("wglSwapIntervalEXT")))) swapInterval(1);
        return true;
    }

    bool current(HDC dc) {
        if (currentDc == dc) return true;
        if (!wglMakeCurrent(dc, context)) return false;
        currentDc = dc;
        return true;
    }

    std::unique_ptr<PlatformWindow> createWindow(const WindowSettings& settings, PlatformWindow* owner, std::string& error) override {
        auto window = std::make_unique<WinWindow>(*this);
        WinWindow& w = *window;
        w.fixedScale = settings.scale;
        w.style = settings.dialog ? (WS_POPUP | WS_CAPTION | WS_SYSMENU) : WS_OVERLAPPEDWINDOW;
        if (!settings.resizable) w.style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        else if (settings.dialog) w.style |= WS_THICKFRAME;
        w.exStyle = settings.dialog ? WS_EX_DLGMODALFRAME : 0;
        HWND parent = owner != nullptr ? static_cast<WinWindow*>(owner)->hwnd : nullptr;
        w.hwnd = CreateWindowExW(w.exStyle, kWindowClass, widen(settings.title).c_str(), w.style, CW_USEDEFAULT, CW_USEDEFAULT, 200, 200, parent, nullptr,
                                 instance, nullptr);
        if (w.hwnd == nullptr) {
            error = "cannot create a window";
            return nullptr;
        }
        w.dc = GetDC(w.hwnd);
        PIXELFORMATDESCRIPTOR pfd = {};
        DescribePixelFormat(w.dc, pixelFormat, sizeof pfd, &pfd);
        if (!SetPixelFormat(w.dc, pixelFormat, &pfd)) {
            error = "cannot prepare the window for OpenGL";
            return nullptr;  // the window is destroyed with the object
        }
        windows[w.hwnd] = &w;
        DragAcceptFiles(w.hwnd, TRUE);
        // The size asked for is of what can be drawn on, in layout units.
        const double scale = w.scale();
        w.setSize(static_cast<int>(std::lround(settings.width * scale)), static_cast<int>(std::lround(settings.height * scale)));
        return window;
    }

    bool makeCurrentOffscreen() override { return current(helperDc); }

    void pumpEvents() override {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    void waitEvents(int64_t millis) override {
        const DWORD limit = millis < 0 ? INFINITE : static_cast<DWORD>(std::min<int64_t>(millis, 0x7FFFFFFF));
        MsgWaitForMultipleObjectsEx(0, nullptr, limit, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    void wake() override { PostThreadMessageW(thread, WM_NULL, 0, 0); }

    std::string clipboardText() override {
        std::string text;
        if (!OpenClipboard(helper)) return text;
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* wide = static_cast<const wchar_t*>(GlobalLock(data))) {
                text = narrow(wide, static_cast<int>(wcslen(wide)));
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        return text;
    }
    void setClipboardText(const std::string& text) override {
        const std::wstring wide = widen(text);
        if (!OpenClipboard(helper)) return;
        EmptyClipboard();
        const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* to = GlobalLock(block)) {
                memcpy(to, wide.c_str(), bytes);
                GlobalUnlock(block);
                if (SetClipboardData(CF_UNICODETEXT, block) == nullptr) GlobalFree(block);
            } else {
                GlobalFree(block);
            }
        }
        CloseClipboard();
    }

    int doubleClickTime() override { return static_cast<int>(GetDoubleClickTime()); }
    double screenScale() override { return GetDpiForSystem() / 96.0; }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};

WinPlatform* thePlatform = nullptr;

WinWindow::~WinWindow() {
    if (hwnd == nullptr) return;
    if (pointerMode == PointerMode::Locked) ClipCursor(nullptr);
    if (platform.currentDc == dc) platform.current(platform.helperDc);
    platform.windows.erase(hwnd);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    if (icon != nullptr) DestroyIcon(icon);
}

bool WinWindow::makeCurrent() { return platform.current(dc); }

LRESULT CALLBACK WinPlatform::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (thePlatform != nullptr) {
        auto found = thePlatform->windows.find(hwnd);
        if (found != thePlatform->windows.end()) return found->second->handle(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT WinWindow::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    RawEvent e;
    switch (message) {
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            if (onExpose) onExpose();
            return 0;
        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            e.kind = RawEvent::Resize;
            e.width = LOWORD(lParam);
            e.height = HIWORD(lParam);
            emit(e);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            if (minWidth > 0 || minHeight > 0) {
                const SIZE least = outerSize(std::max(minWidth, 1), std::max(minHeight, 1));
                info->ptMinTrackSize.x = least.cx;
                info->ptMinTrackSize.y = least.cy;
            }
            if (maxWidth > 0 && maxHeight > 0) {
                const SIZE most = outerSize(maxWidth, maxHeight);
                info->ptMaxTrackSize.x = most.cx;
                info->ptMaxTrackSize.y = most.cy;
            }
            return 0;
        }
        case WM_DPICHANGED: {
            // The window has moved to a screen of another density: Windows says where it
            // should now be, and the size event that follows says how large.
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            e.kind = RawEvent::Scale;
            e.scale = scale();
            emit(e);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        // While an edge or the title bar is dragged Windows runs a loop of its own and ours
        // gets no turn: a timer keeps the program's own timers and tasks going meanwhile.
        case WM_ENTERSIZEMOVE:
            SetTimer(hwnd, kIdleTimer, 10, nullptr);
            return 0;
        case WM_EXITSIZEMOVE:
            KillTimer(hwnd, kIdleTimer);
            return 0;
        case WM_TIMER:
            if (wParam == kIdleTimer && platform.onIdle) platform.onIdle();
            return 0;

        case WM_MOUSEMOVE: {
            const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            if (!tracking) {
                TRACKMOUSEEVENT track = {sizeof track, TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                tracking = true;
            }
            if (!inside) {
                inside = true;
                SetCursor(systemCursor());
            }
            e.kind = RawEvent::PointerMove;
            e.x = x;
            e.y = y;
            e.dx = haveLast ? x - lastX : 0;
            e.dy = haveLast ? y - lastY : 0;
            e.buttons = buttonsOf(wParam);
            e.modifiers = modifiersNow();
            haveLast = true;
            lastX = x;
            lastY = y;
            if (pointerMode == PointerMode::Locked) {
                POINT middle;
                centre(middle);
                if (x == middle.x && y == middle.y) return 0;  // our own putting it back
                lastX = middle.x;
                lastY = middle.y;
                ClientToScreen(hwnd, &middle);
                SetCursorPos(middle.x, middle.y);
            }
            emit(e);
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            inside = false;
            haveLast = false;
            e.kind = RawEvent::PointerLeave;
            emit(e);
            return 0;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: {
            const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
            e.kind = down ? RawEvent::PointerDown : RawEvent::PointerUp;
            e.x = GET_X_LPARAM(lParam);
            e.y = GET_Y_LPARAM(lParam);
            if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) e.button = 0;
            else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) e.button = 1;
            else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) e.button = 2;
            else e.button = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4;
            e.buttons = buttonsOf(wParam);
            e.modifiers = modifiersNow();
            // A press holds the pointer to this window until every button is up again, so a
            // drag that leaves the window is still followed.
            if (down) SetCapture(hwnd);
            else if (e.buttons == 0 && GetCapture() == hwnd) ReleaseCapture();
            emit(e);
            return message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ? TRUE : 0;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT at = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};  // on the screen
            ScreenToClient(hwnd, &at);
            const double notches = GET_WHEEL_DELTA_WPARAM(wParam) / static_cast<double>(WHEEL_DELTA);
            e.kind = RawEvent::Wheel;
            e.x = at.x;
            e.y = at.y;
            if (message == WM_MOUSEWHEEL) e.dy = notches;
            else e.dx = -notches;  // Windows counts a tilt to the right as positive
            e.modifiers = modifiersNow();
            emit(e);
            return 0;
        }

        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
            const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            e.kind = down ? RawEvent::KeyDown : RawEvent::KeyUp;
            e.key = keyOf(wParam, lParam);
            e.letter = letterOf(wParam);
            e.modifiers = modifiersNow();
            e.repeat = down && (lParam & (1 << 30)) != 0;
            emit(e);
            // Alt with a key is also Windows' own (Alt+F4 closes, Alt+Space opens the window's menu).
            if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) return DefWindowProcW(hwnd, message, wParam, lParam);
            return 0;
        }
        case WM_SYSCOMMAND:
            // Alt or F10 on its own would put the window into a menu it does not have.
            if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0) return 0;
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_CHAR: {
            const wchar_t unit = static_cast<wchar_t>(wParam);
            if (unit >= 0xD800 && unit < 0xDC00) {
                highSurrogate = unit;  // the first half of a character outside the basic plane
                return 0;
            }
            wchar_t units[2] = {unit, 0};
            int count = 1;
            if (unit >= 0xDC00 && unit < 0xE000) {
                if (highSurrogate == 0) return 0;
                units[0] = highSurrogate;
                units[1] = unit;
                count = 2;
            }
            highSurrogate = 0;
            if (count == 1 && (unit < 32 || unit == 127)) return 0;  // Enter, Backspace and the like come as keys
            e.kind = RawEvent::Text;
            e.text = narrow(units, count);
            emit(e);
            return 0;
        }

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            e.kind = RawEvent::Focus;
            e.on = message == WM_SETFOCUS;
            if (!e.on && pointerMode == PointerMode::Locked) ClipCursor(nullptr);
            emit(e);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(systemCursor());
                return TRUE;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);

        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            POINT at = {0, 0};
            DragQueryPoint(drop, &at);
            e.kind = RawEvent::FilesDropped;
            e.x = at.x;
            e.y = at.y;
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; i++) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(length + 1, L'\0');
                DragQueryFileW(drop, i, path.data(), length + 1);
                e.files.push_back(narrow(path.c_str(), static_cast<int>(length)));
            }
            DragFinish(drop);
            emit(e);
            return 0;
        }

        case WM_CLOSE:
            e.kind = RawEvent::Close;
            emit(e);
            return 0;  // closing is the program's to do
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

}  // namespace

Platform* Platform::get(std::string& error) {
    static std::string failure;
    static bool tried = false;
    if (!tried) {
        tried = true;
        auto* platform = new WinPlatform();  // lives as long as the program
        if (platform->start(failure)) thePlatform = platform;
    }
    if (thePlatform == nullptr) error = failure;
    return thePlatform;
}

}  // namespace astra::jade

#else

#include "platform.h"

namespace astra::jade {

Platform* Platform::get(std::string& error) {
    error = "windows are there on Windows only so far";
    return nullptr;
}

}  // namespace astra::jade

#endif
