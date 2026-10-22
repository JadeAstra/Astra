// Jade's native layer: drawing.
//
// Everything is drawn by the graphics card, into pictures it holds ("surfaces"): a window's
// own picture, which is kept and shown again whenever the system asks; and pictures off
// the screen, which widgets cache themselves in and tests read back. A window is shown by
// copying its surface to the screen. With no window at all the same drawing goes on into a
// surface, which is how a picture is made with nothing on screen.
//
// What can be drawn is what the toolkit's canvas has: rectangles, rounded rectangles, ovals
// and lines with smoothed edges, polygons, text, and other surfaces. Text comes from the
// text engine (src/text): a glyph is rasterised once, kept in a texture (the "atlas"), and
// drawn from there. Shapes become one small record each, sent to the card in batches.
//
// All of it is in device pixels, with y growing downwards. A surface's colours are kept
// multiplied by their alpha, which is what makes one surface draw correctly onto another.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../text/engine.h"
#include "gl.h"
#include "platform.h"

namespace astra::jade {

struct Surface {
    int width = 0, height = 0;
    GLuint texture = 0, framebuffer = 0;
};

class Renderer {
public:
    // The one renderer of the program. Null, with `error`, if there is no OpenGL to draw with.
    static Renderer* get(std::string& error);

    Platform& platform() { return *platform_; }

    // -- Surfaces --
    // A new one, every pixel clear. 0 if it cannot be made (too large).
    int createSurface(int width, int height);
    void destroySurface(int id);
    // Destroys one later, from the drawing thread: for the collector, which runs on any thread.
    static void releaseLater(int64_t id);
    Surface* surface(int id);
    // Gives a surface another size. What was drawn on it stays, from the top left corner, as
    // much as fits; a part that is new is clear.
    bool resizeSurface(int id, int width, int height);
    // Replaces what a surface holds with a picture of its size: red, green, blue, alpha,
    // four bytes a pixel, the colours not multiplied by the alpha.
    void upload(int id, const uint8_t* rgba);
    // Only the rectangle (x, y, w, h) of a picture `rgbaWidth` pixels wide, into the same place:
    // what an Image has changed since its copy was made (D124).
    void uploadPart(int id, const uint8_t* rgba, int rgbaWidth, int x, int y, int w, int h);
    // The same the other way.
    bool read(int id, std::vector<uint8_t>& rgba);
    int maxSurfaceSize() const { return maxTexture_; }

    // -- Drawing --
    // Where the calls that follow draw, and the rectangle outside which they leave no mark.
    void setTarget(int surface, int clipLeft, int clipTop, int clipRight, int clipBottom);
    // Makes a rectangle clear again (within the clip).
    void clear(int left, int top, int right, int bottom);
    // A rectangle filled exactly: its edges are on pixel edges.
    void fillRect(float left, float top, float right, float bottom, uint32_t rgba);
    // A rounded rectangle or an oval in a box, with smoothed edges. `stroke` 0 fills it;
    // more draws an outline that wide, inside the box.
    void roundRect(float left, float top, float right, float bottom, float radius, float stroke, uint32_t rgba);
    void oval(float left, float top, float right, float bottom, float stroke, uint32_t rgba);
    // A line from the middle of one point to the middle of another.
    void line(float x1, float y1, float x2, float y2, float width, uint32_t rgba);
    // A filled polygon: x, y pairs.
    void polygon(const std::vector<float>& points, uint32_t rgba);
    // A line of text with its start at x and its baseline at y. Returns its width.
    int text(text::Font& font, std::string_view utf8, int x, int baseline, uint32_t rgba);
    // Another surface, or a part of it, drawn into a rectangle, as see-through as `alpha` says.
    void image(int source, float srcLeft, float srcTop, float srcRight, float srcBottom, float left, float top, float right, float bottom, float alpha);

    // Sends what has been drawn so far to the card.
    void flush();
    // Draws a surface into a window. It shows once the window's buffers are swapped, which
    // waits for the screen's next refresh: the caller does that, and may let other threads
    // run meanwhile. False if there was nothing to draw.
    bool present(int surface, PlatformWindow& window);

    // How many shapes have been sent to the card, and in how many batches, since the start.
    int64_t shapesDrawn() const { return shapesDrawn_; }
    int64_t batchesDrawn() const { return batchesDrawn_; }
    size_t surfaceCount() const { return surfaces_.size(); }
    std::string description() const;

private:
    Renderer() = default;
    bool start(std::string& error);

    // One shape, as the card gets it.
    struct Instance {
        float cx, cy, hx, hy;        // its middle and half its size
        float ax, ay, kind, radius;  // which way it lies; what it is; how round its corners are
        float u0, v0, u1, v1;        // for a glyph or a picture: where in the texture
        uint8_t r, g, b, a;
        float clip[4];
        float stroke, margin;
    };
    struct Run {
        GLuint texture;
        size_t first, count;
    };
    struct AtlasEntry {
        int x = 0, y = 0, w = 0, h = 0, left = 0, top = 0;
        bool color = false;
    };
    struct Atlas {
        GLuint texture = 0;
        bool color = false;
        int size = 0;
        std::vector<uint8_t> pixels;
        int shelfX = 1, shelfY = 1, shelfHeight = 0;
        int dirtyTop = 0, dirtyBottom = 0;  // rows changed since they were last sent
        bool place(int w, int h, int& x, int& y);
        void reset();
    };
    struct GlyphKey {
        const text::Font* font;
        uint32_t glyph, foreground;
        bool operator==(const GlyphKey& o) const { return font == o.font && glyph == o.glyph && foreground == o.foreground; }
    };
    struct GlyphKeyHash {
        size_t operator()(const GlyphKey& k) const {
            return std::hash<const void*>()(k.font) ^ (static_cast<size_t>(k.glyph) * 0x9E3779B1u) ^ (static_cast<size_t>(k.foreground) << 7);
        }
    };

    Platform* platform_ = nullptr;
    GLuint program_ = 0, vao_ = 0, vbo_ = 0;
    GLint uTarget_ = -1, uFlip_ = -1, uTex_ = -1;
    int maxTexture_ = 2048;

    // By number. A number is never used twice, so one that is let go of twice (by the
    // program, and again by the collector) does no harm.
    std::unordered_map<int, std::unique_ptr<Surface>> surfaces_;
    int nextSurface_ = 1;

    int target_ = 0;
    float clip_[4] = {0, 0, 0, 0};
    std::vector<Instance> batch_;
    std::vector<Run> runs_;
    std::vector<GLuint> temporaries_;  // textures made for one batch

    Atlas coverage_, colors_;
    std::unordered_map<GlyphKey, AtlasEntry, GlyphKeyHash> glyphs_;
    std::unordered_map<uint64_t, AtlasEntry> shapes_;  // polygons, by what they look like
    int64_t shapesDrawn_ = 0, batchesDrawn_ = 0;

    Instance& add(GLuint texture);
    void push(float cx, float cy, float hx, float hy, float kind, float radius, float stroke, float margin, uint32_t rgba, GLuint texture = 0);
    const AtlasEntry& glyphEntry(text::Font& font, int glyph, uint32_t foreground);
    AtlasEntry store(const text::GlyphImage& image);
    void resetAtlases();
    void sendAtlas(Atlas& atlas);
    void drawRuns(int width, int height, float flip);
    void drainReleased();
};

}  // namespace astra::jade
