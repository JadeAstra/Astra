// M0 spike: an OpenGL window written directly against Windows, a small renderer for smooth
// shapes, and text drawn from our own engine (font.cpp) through a glyph atlas. It is exposed
// to scripts as the class `Spike`, so that a script can draw a frame, receive raw events and
// save what was drawn. Nothing here is the final design; it exists to prove the pieces.

#include "../vm.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "font.h"
#include "png.h"

namespace astra {

namespace {

using spike::Font;
using spike::GlyphBitmap;

// ---- OpenGL beyond version 1.1: fetched from the driver by name ----

const GLenum kFragmentShader = 0x8B30, kVertexShader = 0x8B31, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82;
const GLenum kArrayBuffer = 0x8892, kStreamDraw = 0x88E0, kTexture0 = 0x84C0, kClampToEdge = 0x812F;
const GLenum kFramebuffer = 0x8D40, kColorAttachment0 = 0x8CE0, kFramebufferComplete = 0x8CD5;
const GLenum kR8 = 0x8229, kRed = 0x1903, kRgba8 = 0x8058;

struct GlApi {
    GLuint(APIENTRY* CreateShader)(GLenum);
    void(APIENTRY* ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
    void(APIENTRY* CompileShader)(GLuint);
    void(APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
    GLuint(APIENTRY* CreateProgram)();
    void(APIENTRY* AttachShader)(GLuint, GLuint);
    void(APIENTRY* LinkProgram)(GLuint);
    void(APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* UseProgram)(GLuint);
    GLint(APIENTRY* GetUniformLocation)(GLuint, const char*);
    void(APIENTRY* Uniform1i)(GLint, GLint);
    void(APIENTRY* Uniform1f)(GLint, GLfloat);
    void(APIENTRY* Uniform2f)(GLint, GLfloat, GLfloat);
    void(APIENTRY* GenVertexArrays)(GLsizei, GLuint*);
    void(APIENTRY* BindVertexArray)(GLuint);
    void(APIENTRY* GenBuffers)(GLsizei, GLuint*);
    void(APIENTRY* BindBuffer)(GLenum, GLuint);
    void(APIENTRY* BufferData)(GLenum, ptrdiff_t, const void*, GLenum);
    void(APIENTRY* EnableVertexAttribArray)(GLuint);
    void(APIENTRY* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    void(APIENTRY* ActiveTexture)(GLenum);
    void(APIENTRY* GenFramebuffers)(GLsizei, GLuint*);
    void(APIENTRY* BindFramebuffer)(GLenum, GLuint);
    void(APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum(APIENTRY* CheckFramebufferStatus)(GLenum);
    void(APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*);
    void(APIENTRY* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
} gl;

template <typename F>
bool fetch(F& slot, const char* name) {
    slot = reinterpret_cast<F>(wglGetProcAddress(name));
    return slot != nullptr;
}

bool loadGl() {
    return fetch(gl.CreateShader, "glCreateShader") && fetch(gl.ShaderSource, "glShaderSource") &&
           fetch(gl.CompileShader, "glCompileShader") && fetch(gl.GetShaderiv, "glGetShaderiv") &&
           fetch(gl.GetShaderInfoLog, "glGetShaderInfoLog") && fetch(gl.CreateProgram, "glCreateProgram") &&
           fetch(gl.AttachShader, "glAttachShader") && fetch(gl.LinkProgram, "glLinkProgram") &&
           fetch(gl.GetProgramiv, "glGetProgramiv") && fetch(gl.UseProgram, "glUseProgram") &&
           fetch(gl.GetUniformLocation, "glGetUniformLocation") && fetch(gl.Uniform1i, "glUniform1i") &&
           fetch(gl.Uniform1f, "glUniform1f") && fetch(gl.Uniform2f, "glUniform2f") &&
           fetch(gl.GenVertexArrays, "glGenVertexArrays") && fetch(gl.BindVertexArray, "glBindVertexArray") &&
           fetch(gl.GenBuffers, "glGenBuffers") && fetch(gl.BindBuffer, "glBindBuffer") &&
           fetch(gl.BufferData, "glBufferData") && fetch(gl.EnableVertexAttribArray, "glEnableVertexAttribArray") &&
           fetch(gl.VertexAttribPointer, "glVertexAttribPointer") && fetch(gl.ActiveTexture, "glActiveTexture") &&
           fetch(gl.GenFramebuffers, "glGenFramebuffers") && fetch(gl.BindFramebuffer, "glBindFramebuffer") &&
           fetch(gl.FramebufferTexture2D, "glFramebufferTexture2D") &&
           fetch(gl.CheckFramebufferStatus, "glCheckFramebufferStatus") &&
           fetch(gl.DeleteFramebuffers, "glDeleteFramebuffers") && fetch(gl.BlendFuncSeparate, "glBlendFuncSeparate");
}

// One program draws everything. Each vertex says what its quad is: a plain fill, a rounded
// rectangle or an ellipse whose edge is smoothed from its distance to the pixel, or a glyph
// taken from the atlas.
const char* const kVertexSource = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec4 aBox;     // position within the shape, and the shape's half size
layout(location = 4) in vec4 aParams;  // kind, corner radius, outline width
uniform vec2 uViewport;
out vec2 vUv;
out vec4 vColor;
out vec4 vBox;
out vec4 vParams;
void main() {
    vUv = aUv;
    vColor = aColor;
    vBox = aBox;
    vParams = aParams;
    gl_Position = vec4(aPos.x / uViewport.x * 2.0 - 1.0, 1.0 - aPos.y / uViewport.y * 2.0, 0.0, 1.0);
})";

const char* const kFragmentSource = R"(#version 330 core
in vec2 vUv;
in vec4 vColor;
in vec4 vBox;
in vec4 vParams;
uniform sampler2D uAtlas;
uniform float uTextGamma;
out vec4 frag;
float roundedBox(vec2 p, vec2 halfSize, float radius) {
    vec2 q = abs(p) - halfSize + vec2(radius);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}
void main() {
    float kind = vParams.x;
    float alpha = 1.0;
    if (kind > 2.5) {
        alpha = pow(texture(uAtlas, vUv).r, uTextGamma);
    } else if (kind > 0.5) {
        float d;
        if (kind > 1.5) {
            float k = length(vBox.xy / vBox.zw);
            d = (k - 1.0) / max(length(vec2(dFdx(k), dFdy(k))), 0.00001);
        } else {
            d = roundedBox(vBox.xy, vBox.zw, vParams.y);
        }
        if (vParams.z > 0.0) d = abs(d + vParams.z * 0.5) - vParams.z * 0.5;
        alpha = clamp(0.5 - d, 0.0, 1.0);
    }
    frag = vec4(vColor.rgb, vColor.a * alpha);
})";

struct Vertex {
    float x, y, u, v;
    float r, g, b, a;
    float lx, ly, hx, hy;
    float kind, radius, stroke, unused;
};

struct AtlasGlyph {
    int x = 0, y = 0, w = 0, h = 0, left = 0, top = 0;
};

const int kAtlasSize = 1024;

struct State {
    VM* vm = nullptr;
    HWND hwnd = nullptr;
    HDC dc = nullptr;
    HGLRC context = nullptr;
    bool visible = false;
    bool open = false;
    int width = 0, height = 0;  // logical pixels
    float scale = 1.0f;         // device pixels per logical pixel

    GLuint program = 0, vao = 0, vbo = 0, atlasTexture = 0;
    GLint uViewport = -1, uAtlas = -1, uTextGamma = -1;
    float textGamma = 1.0f;
    std::vector<Vertex> batch;

    Font font;
    bool fontLoaded = false;
    std::vector<uint8_t> atlas;
    std::map<uint64_t, AtlasGlyph> glyphs;
    int shelfX = 1, shelfY = 1, shelfHeight = 0;
    bool atlasDirty = false;

    bool hasHandler = false;
    size_t handlerPin = 0;
    Value handler;
    bool painting = false;
    std::string snapshotPath;
    bool quit = false;

    bool hasThrow = false, hasExit = false;
    AstraThrow pendingThrow;
    int exitCode = 0;
} g;

// ---- Text ----

bool ensureFont() {
    if (g.fontLoaded) return true;
    wchar_t windows[MAX_PATH];
    GetWindowsDirectoryW(windows, MAX_PATH);
    std::string path;
    for (const wchar_t* c = windows; *c != 0; c++) path.push_back(static_cast<char>(*c));  // the Windows folder is ASCII
    g.fontLoaded = g.font.load(path + "\\Fonts\\segoeui.ttf");
    return g.fontLoaded;
}

// The code points of a UTF-8 string.
std::vector<uint32_t> decode(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        uint32_t cp = extra == 3 ? c & 0x07 : extra == 2 ? c & 0x0F : extra == 1 ? c & 0x1F : c;
        i++;
        for (; extra > 0 && i < s.size(); extra--, i++) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        out.push_back(cp);
    }
    return out;
}

// The width of a string by the one rule: the sum of its glyphs' whole-pixel widths.
int measure(const std::string& text, float size) {
    int width = 0;
    for (uint32_t cp : decode(text)) width += g.font.advance(g.font.glyphIndex(cp), size);
    return width;
}

const AtlasGlyph& atlasGlyph(int glyph, int devicePixels, bool hint) {
    const uint64_t key = (static_cast<uint64_t>(glyph) << 24) | (static_cast<uint64_t>(devicePixels) << 1) | (hint ? 1 : 0);
    auto found = g.glyphs.find(key);
    if (found != g.glyphs.end()) return found->second;

    GlyphBitmap bitmap = g.font.rasterize(glyph, static_cast<float>(devicePixels), hint);
    AtlasGlyph entry;
    if (bitmap.width > 0 && bitmap.height > 0) {
        if (g.atlas.empty()) g.atlas.assign(static_cast<size_t>(kAtlasSize) * kAtlasSize, 0);
        if (g.shelfX + bitmap.width + 1 > kAtlasSize) {
            g.shelfX = 1;
            g.shelfY += g.shelfHeight + 1;
            g.shelfHeight = 0;
        }
        if (g.shelfY + bitmap.height + 1 <= kAtlasSize) {
            for (int row = 0; row < bitmap.height; row++)
                std::copy_n(bitmap.coverage.begin() + static_cast<std::ptrdiff_t>(row) * bitmap.width, bitmap.width,
                            g.atlas.begin() + static_cast<std::ptrdiff_t>(g.shelfY + row) * kAtlasSize + g.shelfX);
            entry = AtlasGlyph{g.shelfX, g.shelfY, bitmap.width, bitmap.height, bitmap.left, bitmap.top};
            g.shelfX += bitmap.width + 1;
            g.shelfHeight = std::max(g.shelfHeight, bitmap.height);
            g.atlasDirty = true;
        }
    }
    return g.glyphs[key] = entry;
}

// ---- Drawing: everything becomes quads in one batch ----

struct Rgba {
    float r, g, b, a;
};

Rgba color(int64_t rgb, double alpha) {
    return Rgba{static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                static_cast<float>(rgb & 0xFF) / 255.0f, static_cast<float>(alpha)};
}

void quad(const float px[4], const float py[4], const float lx[4], const float ly[4], const float u[4], const float v[4],
          Rgba c, float hx, float hy, float kind, float radius, float stroke) {
    static const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i : order)
        g.batch.push_back(Vertex{px[i], py[i], u[i], v[i], c.r, c.g, c.b, c.a, lx[i], ly[i], hx, hy, kind, radius, stroke, 0});
}

// A shape in the rectangle (x, y, w, h), in logical pixels. kind 0 fills it exactly; kinds 1
// and 2 are a rounded rectangle and an ellipse whose edges are smoothed in the shader.
void shape(float x, float y, float w, float h, Rgba c, float kind, float radius, float stroke) {
    const float s = g.scale;
    x *= s, y *= s, w *= s, h *= s, radius *= s, stroke *= s;
    const float margin = kind > 0.5f ? 1.0f : 0.0f;  // room for the smoothed edge
    const float hx = w / 2, hy = h / 2, cx = x + hx, cy = y + hy;
    const float ex = hx + margin, ey = hy + margin;
    const float px[4] = {cx - ex, cx + ex, cx + ex, cx - ex}, py[4] = {cy - ey, cy - ey, cy + ey, cy + ey};
    const float lx[4] = {-ex, ex, ex, -ex}, ly[4] = {-ey, -ey, ey, ey};
    const float zero[4] = {0, 0, 0, 0};
    quad(px, py, lx, ly, zero, zero, c, hx, hy, kind, radius, stroke);
}

void line(float x1, float y1, float x2, float y2, float width, Rgba c) {
    const float s = g.scale;
    // Coordinates name pixels; a line runs through their centres.
    x1 = (x1 + 0.5f) * s, y1 = (y1 + 0.5f) * s, x2 = (x2 + 0.5f) * s, y2 = (y2 + 0.5f) * s, width *= s;
    const float dx = x2 - x1, dy = y2 - y1, length = std::hypot(dx, dy);
    if (length <= 0) return;
    const float ux = dx / length, uy = dy / length, nx = -uy, ny = ux;
    const float hx = length / 2 + width / 2, hy = width / 2, ex = hx + 1, ey = hy + 1;
    const float cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
    const float sx[4] = {-ex, ex, ex, -ex}, sy[4] = {-ey, -ey, ey, ey};
    float px[4], py[4];
    for (int i = 0; i < 4; i++) {
        px[i] = cx + ux * sx[i] + nx * sy[i];
        py[i] = cy + uy * sx[i] + ny * sy[i];
    }
    const float zero[4] = {0, 0, 0, 0};
    quad(px, py, sx, sy, zero, zero, c, hx, hy, 1, 0, 0);
}

// Draws `text` with its baseline at (x, y) and returns its width, both in logical pixels.
int drawText(const std::string& text, float x, float y, float size, Rgba c, bool hint) {
    const int devicePixels = std::max(1, static_cast<int>(std::lround(size * g.scale)));
    const float baseline = std::round(y * g.scale);
    int pen = 0;
    for (uint32_t cp : decode(text)) {
        const int glyph = g.font.glyphIndex(cp);
        const AtlasGlyph& a = atlasGlyph(glyph, devicePixels, hint);
        if (a.w > 0) {
            const float left = std::round((x + static_cast<float>(pen)) * g.scale) + static_cast<float>(a.left);
            const float top = baseline - static_cast<float>(a.top);
            const float px[4] = {left, left + a.w, left + a.w, left}, py[4] = {top, top, top + a.h, top + a.h};
            const float inv = 1.0f / kAtlasSize;
            const float u[4] = {a.x * inv, (a.x + a.w) * inv, (a.x + a.w) * inv, a.x * inv};
            const float v[4] = {a.y * inv, a.y * inv, (a.y + a.h) * inv, (a.y + a.h) * inv};
            const float zero[4] = {0, 0, 0, 0};
            quad(px, py, zero, zero, u, v, c, 0, 0, 3, 0, 0);
        }
        pen += g.font.advance(glyph, size);  // the one rule: whole logical pixels
    }
    return pen;
}

void drawBatch(int deviceWidth, int deviceHeight) {
    glViewport(0, 0, deviceWidth, deviceHeight);
    gl.UseProgram(g.program);
    gl.Uniform2f(g.uViewport, static_cast<float>(deviceWidth), static_cast<float>(deviceHeight));
    gl.Uniform1i(g.uAtlas, 0);
    gl.Uniform1f(g.uTextGamma, g.textGamma);
    gl.ActiveTexture(kTexture0);
    glBindTexture(GL_TEXTURE_2D, g.atlasTexture);
    if (g.atlasDirty) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, kR8, kAtlasSize, kAtlasSize, 0, kRed, GL_UNSIGNED_BYTE, g.atlas.data());
        g.atlasDirty = false;
    }
    gl.BindVertexArray(g.vao);
    gl.BindBuffer(kArrayBuffer, g.vbo);
    gl.BufferData(kArrayBuffer, static_cast<ptrdiff_t>(g.batch.size() * sizeof(Vertex)), g.batch.data(), kStreamDraw);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(g.batch.size()));
}

// ---- Events ----

void dispatch(const char* kind, Value a = Value::null(), Value b = Value::null(), Value c = Value::null()) {
    if (!g.hasHandler || g.hasThrow || g.hasExit) return;
    try {
        Value args[4] = {Value::object(g.vm->intern(kind)), a, b, c};
        g.vm->call(g.handler, 4, args);
    } catch (AstraThrow& thrown) {
        g.pendingThrow = thrown;
        g.hasThrow = true;
        g.quit = true;
    } catch (AstraExit& exit) {
        g.exitCode = exit.code;
        g.hasExit = true;
        g.quit = true;
    }
}

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

// Draws one frame: the script paints into the batch, which is then drawn to the window and,
// if a snapshot was asked for, first into a framebuffer of its own that is read back.
void frame() {
    if (!g.open || g.painting) return;
    g.painting = true;
    g.batch.clear();
    dispatch("paint", Value::integer(g.width), Value::integer(g.height));
    g.painting = false;

    const int deviceWidth = std::max(1, static_cast<int>(std::lround(g.width * g.scale)));
    const int deviceHeight = std::max(1, static_cast<int>(std::lround(g.height * g.scale)));

    if (!g.snapshotPath.empty()) {
        GLuint texture = 0, framebuffer = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, kRgba8, deviceWidth, deviceHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.GenFramebuffers(1, &framebuffer);
        gl.BindFramebuffer(kFramebuffer, framebuffer);
        gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, texture, 0);
        if (gl.CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete) {
            glClearColor(1, 1, 1, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            drawBatch(deviceWidth, deviceHeight);
            std::vector<uint8_t> pixels(static_cast<size_t>(deviceWidth) * deviceHeight * 4), flipped(pixels.size());
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, deviceWidth, deviceHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            const size_t rowBytes = static_cast<size_t>(deviceWidth) * 4;
            for (int row = 0; row < deviceHeight; row++)
                std::copy_n(pixels.begin() + static_cast<std::ptrdiff_t>((deviceHeight - 1 - row) * rowBytes), rowBytes,
                            flipped.begin() + static_cast<std::ptrdiff_t>(row * rowBytes));
            spike::writePng(g.snapshotPath, deviceWidth, deviceHeight, flipped);
        }
        gl.BindFramebuffer(kFramebuffer, 0);
        gl.DeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
        g.snapshotPath.clear();
    }

    if (g.visible) {
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        drawBatch(deviceWidth, deviceHeight);
        SwapBuffers(g.dc);
    }
}

int logical(LPARAM coordinate) { return static_cast<int>(std::floor(static_cast<float>(static_cast<short>(coordinate)) / g.scale)); }

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            frame();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            g.width = static_cast<int>(std::lround(LOWORD(lParam) / g.scale));
            g.height = static_cast<int>(std::lround(HIWORD(lParam) / g.scale));
            dispatch("resize", Value::integer(g.width), Value::integer(g.height));
            frame();  // Windows runs its own loop while a window is being resized: draw from here
            return 0;
        case WM_MOUSEMOVE:
            dispatch("mouseMove", Value::integer(logical(LOWORD(lParam))), Value::integer(logical(HIWORD(lParam))));
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            dispatch("mouseDown", Value::integer(logical(LOWORD(lParam))), Value::integer(logical(HIWORD(lParam))),
                     Value::integer(message == WM_LBUTTONDOWN ? 1 : 2));
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            dispatch("mouseUp", Value::integer(logical(LOWORD(lParam))), Value::integer(logical(HIWORD(lParam))),
                     Value::integer(message == WM_LBUTTONUP ? 1 : 2));
            return 0;
        case WM_MOUSEWHEEL:
            dispatch("wheel", Value::integer(GET_WHEEL_DELTA_WPARAM(wParam)));
            return 0;
        case WM_KEYDOWN:
            dispatch("keyDown", Value::integer(static_cast<int64_t>(wParam)), Value::boolean((lParam & (1 << 30)) != 0));
            return 0;
        case WM_KEYUP:
            dispatch("keyUp", Value::integer(static_cast<int64_t>(wParam)));
            return 0;
        case WM_CHAR:
            dispatch("char", Value::integer(static_cast<int64_t>(wParam)));
            return 0;
        case WM_CLOSE:
            dispatch("close");
            g.quit = true;
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

GLuint compile(GLenum kind, const char* source, std::string& log) {
    GLuint shader = gl.CreateShader(kind);
    gl.ShaderSource(shader, 1, &source, nullptr);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, kCompileStatus, &ok);
    if (!ok) {
        char buffer[1024] = {0};
        gl.GetShaderInfoLog(shader, sizeof buffer - 1, nullptr, buffer);
        log = buffer;
    }
    return shader;
}

// Creates the window and an OpenGL 3.3 core context on it. Returns an error text, or "".
std::string openWindow(const std::wstring& title, int width, int height, bool visible, float scale) {
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof wc;
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = windowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        if (wc.hIcon == nullptr) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.lpszClassName = L"AstraSpikeWindow";
        RegisterClassExW(&wc);
        registered = true;
    }

    g.scale = scale;
    g.width = width;
    g.height = height;
    g.visible = visible;
    RECT rect = {0, 0, static_cast<LONG>(std::lround(width * scale)), static_cast<LONG>(std::lround(height * scale))};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rect, style, FALSE);
    g.hwnd = CreateWindowExW(0, L"AstraSpikeWindow", title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (g.hwnd == nullptr) return "cannot create a window";
    g.dc = GetDC(g.hwnd);

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    const int format = ChoosePixelFormat(g.dc, &pfd);
    if (format == 0 || !SetPixelFormat(g.dc, format, &pfd)) return "no usable pixel format";

    // A context of the old kind is needed first, only to ask for the function that makes a new one.
    HGLRC legacy = wglCreateContext(g.dc);
    if (legacy == nullptr || !wglMakeCurrent(g.dc, legacy)) return "cannot create an OpenGL context";
    using CreateContextAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
    auto createContextAttribs = reinterpret_cast<CreateContextAttribs>(wglGetProcAddress("wglCreateContextAttribsARB"));
    if (createContextAttribs == nullptr) return "this OpenGL driver cannot create a version 3.3 context";
    const int attributes[] = {0x2091, 3, 0x2092, 3, 0x9126, 0x00000001, 0};  // major, minor, core profile
    g.context = createContextAttribs(g.dc, nullptr, attributes);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(legacy);
    if (g.context == nullptr || !wglMakeCurrent(g.dc, g.context)) return "OpenGL 3.3 is not available";
    if (!loadGl()) return "an OpenGL function is missing";

    std::string log;
    GLuint vs = compile(kVertexShader, kVertexSource, log), fs = compile(kFragmentShader, kFragmentSource, log);
    if (!log.empty()) return "shader: " + log;
    g.program = gl.CreateProgram();
    gl.AttachShader(g.program, vs);
    gl.AttachShader(g.program, fs);
    gl.LinkProgram(g.program);
    GLint linked = 0;
    gl.GetProgramiv(g.program, kLinkStatus, &linked);
    if (!linked) return "the shader program does not link";
    g.uViewport = gl.GetUniformLocation(g.program, "uViewport");
    g.uAtlas = gl.GetUniformLocation(g.program, "uAtlas");
    g.uTextGamma = gl.GetUniformLocation(g.program, "uTextGamma");

    gl.GenVertexArrays(1, &g.vao);
    gl.BindVertexArray(g.vao);
    gl.GenBuffers(1, &g.vbo);
    gl.BindBuffer(kArrayBuffer, g.vbo);
    const GLsizei stride = sizeof(Vertex);
    for (GLuint i = 0; i < 5; i++) gl.EnableVertexAttribArray(i);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, x)));
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, u)));
    gl.VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, r)));
    gl.VertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, lx)));
    gl.VertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, kind)));

    glGenTextures(1, &g.atlasTexture);
    glBindTexture(GL_TEXTURE_2D, g.atlasTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(kClampToEdge));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(kClampToEdge));
    if (g.atlas.empty()) g.atlas.assign(static_cast<size_t>(kAtlasSize) * kAtlasSize, 0);
    g.atlasDirty = true;

    glEnable(GL_BLEND);
    gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    g.open = true;
    g.quit = false;
    if (visible) {
        ShowWindow(g.hwnd, SW_SHOW);
        UpdateWindow(g.hwnd);
    }
    return "";
}

void closeWindow() {
    if (!g.open) return;
    g.open = false;
    wglMakeCurrent(nullptr, nullptr);
    if (g.context != nullptr) wglDeleteContext(g.context);
    if (g.hwnd != nullptr) {
        ReleaseDC(g.hwnd, g.dc);
        DestroyWindow(g.hwnd);
    }
    g.context = nullptr;
    g.hwnd = nullptr;
    g.dc = nullptr;
    g.glyphs.clear();
    g.atlas.clear();
    g.shelfX = g.shelfY = 1;
    g.shelfHeight = 0;
}

// ---- A picture comparing our text with Windows' own, made without OpenGL ----

struct Image {
    int width, height;
    std::vector<uint8_t> rgba;
    Image(int w, int h, uint8_t grey) : width(w), height(h), rgba(static_cast<size_t>(w) * h * 4, grey) {
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    }
    void blend(int x, int y, int r, int gr, int b, float alpha) {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        uint8_t* p = &rgba[(static_cast<size_t>(y) * width + x) * 4];
        p[0] = static_cast<uint8_t>(std::lround(p[0] + (r - p[0]) * alpha));
        p[1] = static_cast<uint8_t>(std::lround(p[1] + (gr - p[1]) * alpha));
        p[2] = static_cast<uint8_t>(std::lround(p[2] + (b - p[2]) * alpha));
    }
    void fill(int x, int y, int w, int h, int r, int gr, int b) {
        for (int yy = y; yy < y + h; yy++)
            for (int xx = x; xx < x + w; xx++) blend(xx, yy, r, gr, b, 1.0f);
    }
};

int cpuText(Image& image, const std::string& text, int x, int baseline, float size, bool hint, float gamma, int rgb) {
    int pen = 0;
    for (uint32_t cp : decode(text)) {
        const int glyph = g.font.glyphIndex(cp);
        GlyphBitmap bitmap = g.font.rasterize(glyph, size, hint);
        for (int row = 0; row < bitmap.height; row++)
            for (int col = 0; col < bitmap.width; col++) {
                const float coverage = bitmap.coverage[static_cast<size_t>(row) * bitmap.width + col] / 255.0f;
                if (coverage > 0)
                    image.blend(x + pen + bitmap.left + col, baseline - bitmap.top + row, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF,
                                rgb & 0xFF, std::pow(coverage, gamma));
            }
        pen += g.font.advance(glyph, size);
    }
    return pen;
}

// The same text drawn by Windows (GDI), copied into the picture. Returns the width Windows gives it.
int gdiText(Image& image, const std::string& text, int x, int top, int size, DWORD quality, int rgb, int background) {
    const int w = image.width - x, h = size * 2 + 8;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, quality, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT all = {0, 0, w, h};
    HBRUSH brush = CreateSolidBrush(RGB((background >> 16) & 0xFF, (background >> 8) & 0xFF, background & 0xFF));
    FillRect(dc, &all, brush);
    DeleteObject(brush);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
    std::wstring wide;
    for (uint32_t cp : decode(text)) wide.push_back(static_cast<wchar_t>(cp));
    TextOutW(dc, 0, 0, wide.c_str(), static_cast<int>(wide.size()));
    SIZE extent = {};
    GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()), &extent);
    GdiFlush();
    const uint8_t* source = static_cast<const uint8_t*>(bits);
    for (int row = 0; row < h && top + row < image.height; row++)
        for (int col = 0; col < w; col++) {
            const uint8_t* p = source + (static_cast<size_t>(row) * w + col) * 4;  // blue, green, red
            image.blend(x + col, top + row, p[2], p[1], p[0], 1.0f);
        }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return extent.cx;
}

int need(VM& vm, Value v, const char* what) { return static_cast<int>(vm.toInt(v, what)); }
float real(VM& vm, Value v, const char* what) { return static_cast<float>(vm.toNumber(v, what)); }

void requireOpen(VM& vm) {
    if (!g.open) throw vm.runtimeError("no Spike window is open");
    if (!ensureFont()) throw vm.runtimeError("cannot load the font: " + g.font.error());
}

void registerNatives(VM& vm) {
    g.vm = &vm;
    ObjClass* cls = vm.alloc<ObjClass>(vm.intern("Spike"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        cls->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("Spike.") + name, minArgs, maxArgs, fn)));
    };

    // open(title, width, height, visible, scale): scale 0 means the display's own.
    def("open", 5, 5, [](VM& vm, int, Value* args) -> Value {
        if (g.open) throw vm.runtimeError("a Spike window is already open");
        const std::string& title = vm.toStr(args[0], "title");
        float scale = real(vm, args[4], "scale");
        if (scale <= 0) {
            HDC screen = GetDC(nullptr);
            scale = static_cast<float>(GetDeviceCaps(screen, LOGPIXELSX)) / 96.0f;
            ReleaseDC(nullptr, screen);
        }
        std::string problem = openWindow(std::wstring(title.begin(), title.end()), need(vm, args[1], "width"),
                                         need(vm, args[2], "height"), args[3].truthy(), scale);
        if (!problem.empty()) {
            closeWindow();
            throw vm.runtimeError("Spike.open: " + problem);
        }
        return Value::number(g.scale);
    });

    def("onEvent", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (g.hasHandler) {
            vm.pinned[g.handlerPin] = args[0];
        } else {
            g.handlerPin = vm.pinned.size();
            vm.pinned.push_back(args[0]);
        }
        g.handler = args[0];
        g.hasHandler = true;
        return Value::null();
    });

    def("glInfo", 0, 0, [](VM& vm, int, Value*) -> Value {
        requireOpen(vm);
        return vm.str(std::string(reinterpret_cast<const char*>(glGetString(GL_VERSION))) + " | " +
                      reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    });

    def("fillRect", 6, 6, [](VM& vm, int, Value* args) -> Value {
        requireOpen(vm);
        shape(real(vm, args[0], "x"), real(vm, args[1], "y"), real(vm, args[2], "width"), real(vm, args[3], "height"),
              color(vm.toInt(args[4], "color"), vm.toNumber(args[5], "alpha")), 0, 0, 0);
        return Value::null();
    });
    // roundRect(x, y, w, h, radius, outline, color, alpha): outline 0 fills it.
    def("roundRect", 8, 8, [](VM& vm, int, Value* args) -> Value {
        requireOpen(vm);
        shape(real(vm, args[0], "x"), real(vm, args[1], "y"), real(vm, args[2], "width"), real(vm, args[3], "height"),
              color(vm.toInt(args[6], "color"), vm.toNumber(args[7], "alpha")), 1, real(vm, args[4], "radius"),
              real(vm, args[5], "outline"));
        return Value::null();
    });
    def("oval", 7, 7, [](VM& vm, int, Value* args) -> Value {
        requireOpen(vm);
        shape(real(vm, args[0], "x"), real(vm, args[1], "y"), real(vm, args[2], "width"), real(vm, args[3], "height"),
              color(vm.toInt(args[5], "color"), vm.toNumber(args[6], "alpha")), 2, 0, real(vm, args[4], "outline"));
        return Value::null();
    });
    def("line", 7, 7, [](VM& vm, int, Value* args) -> Value {
        requireOpen(vm);
        line(real(vm, args[0], "x1"), real(vm, args[1], "y1"), real(vm, args[2], "x2"), real(vm, args[3], "y2"),
             real(vm, args[4], "width"), color(vm.toInt(args[5], "color"), vm.toNumber(args[6], "alpha")));
        return Value::null();
    });
    // text(string, x, baseline, size, color, hinted) -> width
    def("text", 6, 6, [](VM& vm, int, Value* args) -> Value {
        requireOpen(vm);
        return Value::integer(drawText(vm.toStr(args[0], "text"), real(vm, args[1], "x"), real(vm, args[2], "y"),
                                       real(vm, args[3], "size"), color(vm.toInt(args[4], "color"), 1.0), args[5].truthy()));
    });
    def("measure", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!ensureFont()) throw vm.runtimeError("cannot load the font: " + g.font.error());
        return Value::integer(measure(vm.toStr(args[0], "text"), real(vm, args[1], "size")));
    });
    def("lineHeight", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (!ensureFont()) throw vm.runtimeError("cannot load the font: " + g.font.error());
        return Value::integer(g.font.lineHeight(real(vm, args[0], "size")));
    });
    def("textGamma", 1, 1, [](VM& vm, int, Value* args) -> Value {
        g.textGamma = real(vm, args[0], "gamma");
        return Value::null();
    });

    def("snapshot", 1, 1, [](VM& vm, int, Value* args) -> Value {
        g.snapshotPath = vm.toStr(args[0], "file path");
        return Value::null();
    });
    def("frame", 0, 0, [](VM& vm, int, Value*) -> Value {
        requireOpen(vm);
        frame();
        return done();
    });
    def("redraw", 0, 0, [](VM&, int, Value*) -> Value {
        if (g.open && g.visible) InvalidateRect(g.hwnd, nullptr, FALSE);
        return Value::null();
    });
    def("quit", 0, 0, [](VM&, int, Value*) -> Value {
        g.quit = true;
        return Value::null();
    });
    // run(milliseconds): handles events until the window is closed, or for that long if not 0.
    def("run", 1, 1, [](VM& vm, int, Value* args) -> Value {
        requireOpen(vm);
        const int64_t limit = vm.toInt(args[0], "milliseconds");
        const ULONGLONG start = GetTickCount64();
        if (limit > 0) SetTimer(g.hwnd, 1, 50, nullptr);
        MSG message;
        while (!g.quit && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (limit > 0 && GetTickCount64() - start >= static_cast<ULONGLONG>(limit)) g.quit = true;
        }
        closeWindow();
        return done();
    });
    def("close", 0, 0, [](VM&, int, Value*) -> Value {
        closeWindow();
        return done();
    });

    // fontCheck(path, face): reads any font file and rasterises its printable ASCII at 13
    // pixels. Says what happened, for trying the reader on every font a system has.
    def("fontCheck", 2, 2, [](VM& vm, int, Value* args) -> Value {
        Font font;
        if (!font.load(vm.toStr(args[0], "font path"), need(vm, args[1], "face"))) return vm.str("unreadable: " + font.error());
        int drawn = 0;
        long long ink = 0;
        for (uint32_t cp = 33; cp < 127; cp++) {
            const int glyph = font.glyphIndex(cp);
            if (glyph == 0) continue;
            GlyphBitmap bitmap = font.rasterize(glyph, 13.0f, true);
            if (bitmap.width > 0) drawn++;
            for (uint8_t c : bitmap.coverage) ink += c;
        }
        return vm.str("ok: " + std::to_string(drawn) + " of 94 glyphs drawn, ink " + std::to_string(ink / 255) +
                      ", line height " + std::to_string(font.lineHeight(13.0f)));
    });

    // compareText(path, zoomPath): our text beside Windows' own, at several sizes, as pictures.
    def("compareText", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!ensureFont()) throw vm.runtimeError("cannot load the font: " + g.font.error());
        const std::string sample = "The quick brown fox jumps over the lazy dog 0123456789 Hamburgefonstiv Illinois (rename) [x] {y}";
        const int sizes[] = {11, 12, 13, 14, 16, 20, 28};
        const char* labels[] = {"jade, hinted", "jade, not hinted", "Windows, greyscale", "Windows, ClearType"};
        const int labelWidth = 150, width = 1500;
        int height = 12;
        for (int size : sizes) height += 4 * (size + 10) + 14;
        std::string report;

        auto render = [&](int background, int ink, const std::string& path) {
            Image image(width, height, static_cast<uint8_t>(background & 0xFF));
            int y = 12;
            for (int size : sizes) {
                for (int row = 0; row < 4; row++) {
                    const int top = y + row * (size + 10);
                    const int baseline = top + g.font.ascent(static_cast<float>(size));
                    cpuText(image, std::string(labels[row]) + " " + std::to_string(size), 8, top + 11, 11.0f, true, 1.0f, 0x808080);
                    int w = 0;
                    if (row == 0) w = cpuText(image, sample, labelWidth, baseline, static_cast<float>(size), true, 1.0f, ink);
                    else if (row == 1) w = cpuText(image, sample, labelWidth, baseline, static_cast<float>(size), false, 1.0f, ink);
                    else w = gdiText(image, sample, labelWidth, top, size, row == 2 ? ANTIALIASED_QUALITY : CLEARTYPE_QUALITY, ink, background);
                    if (background == 0xFFFFFF && (row == 0 || row == 2))
                        report += (row == 0 ? "size " + std::to_string(size) + ": jade " : ", Windows ") + std::to_string(w) + " px";
                    if (background == 0xFFFFFF && row == 2) report += "\n";
                }
                y += 4 * (size + 10) + 14;
            }
            spike::writePng(path, image.width, image.height, image.rgba);
            return image;
        };

        const std::string path = vm.toStr(args[0], "file path"), zoomPath = vm.toStr(args[1], "zoom path");
        Image light = render(0xFFFFFF, 0x000000, path);
        render(0x202020, 0xF0F0F0, path.substr(0, path.size() - 4) + "_dark.png");

        // The 12 and 13 pixel rows again at three times the size, each pixel a 3 by 3 block.
        const int zoom = 3, cropWidth = 500;
        int from = 12 + (4 * (11 + 10) + 14), to = from + (4 * (12 + 10) + 14) + (4 * (13 + 10) + 14);
        Image zoomed(cropWidth * zoom, (to - from) * zoom, 255);
        for (int yy = 0; yy < (to - from) * zoom; yy++)
            for (int xx = 0; xx < cropWidth * zoom; xx++) {
                const uint8_t* p = &light.rgba[(static_cast<size_t>(from + yy / zoom) * light.width + xx / zoom) * 4];
                zoomed.blend(xx, yy, p[0], p[1], p[2], 1.0f);
            }
        spike::writePng(zoomPath, zoomed.width, zoomed.height, zoomed.rgba);
        return vm.str(report);
    });

    vm.defineGlobal("Spike", Value::object(cls));
}

}  // namespace

void registerSpike(VM& vm) { registerNatives(vm); }

}  // namespace astra

#else

namespace astra {
void registerSpike(VM&) {}
}  // namespace astra

#endif
