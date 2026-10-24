// The text engine and pictures as Astra programs see them: the built-in __text, which
// src/std/fonts.astra is written on.
//
// Fonts are known to a program by number. A font file once read stays read, and a font at a
// size keeps the pictures of its glyphs, for as long as the program runs: there are only as
// many as the program asks for. A picture's pixels, which is where the memory goes, are an
// ordinary `bytes` value that the collector frees.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "../builtins.h"
#include "../image/png.h"
#include "../vm.h"
#include "engine.h"
#include "fonts.h"

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace astra {

namespace {

using text::Face;
using text::Font;
using text::FontBook;

// The fonts handed out to programs. Shared by every runtime of the process (workers have
// their own), so a lock goes with it.
struct Handles {
    std::mutex mutex;
    std::vector<std::shared_ptr<Face>> faces;
    std::unordered_map<const Face*, int64_t> faceNumbers;
    std::vector<std::shared_ptr<Font>> fonts;
    std::unordered_map<const Font*, int64_t> fontNumbers;
};

Handles& handles() {
    static auto* h = new Handles();  // never torn down: a thread may still be drawing at exit
    return *h;
}

int64_t numberOf(const std::shared_ptr<Face>& face) {
    Handles& h = handles();
    std::lock_guard<std::mutex> hold(h.mutex);
    auto known = h.faceNumbers.find(face.get());
    if (known != h.faceNumbers.end()) return known->second;
    h.faces.push_back(face);
    return h.faceNumbers[face.get()] = static_cast<int64_t>(h.faces.size() - 1);
}

int64_t numberOf(const std::shared_ptr<Font>& font) {
    Handles& h = handles();
    std::lock_guard<std::mutex> hold(h.mutex);
    auto known = h.fontNumbers.find(font.get());
    if (known != h.fontNumbers.end()) return known->second;
    h.fonts.push_back(font);
    return h.fontNumbers[font.get()] = static_cast<int64_t>(h.fonts.size() - 1);
}

std::shared_ptr<Face> faceOf(VM& vm, Value v) {
    const int64_t number = vm.toInt(v, "typeface");
    Handles& h = handles();
    std::lock_guard<std::mutex> hold(h.mutex);
    if (number < 0 || static_cast<size_t>(number) >= h.faces.size()) throw vm.runtimeError(Thrown::IllegalStateException, "not a typeface");
    return h.faces[static_cast<size_t>(number)];
}

std::shared_ptr<Font> fontOf(VM& vm, Value v) {
    const int64_t number = vm.toInt(v, "font");
    Handles& h = handles();
    std::lock_guard<std::mutex> hold(h.mutex);
    if (number < 0 || static_cast<size_t>(number) >= h.fonts.size()) throw vm.runtimeError(Thrown::IllegalStateException, "not a font");
    return h.fonts[static_cast<size_t>(number)];
}

// A character given as a char, a one-character string or its number.
uint32_t characterOf(VM& vm, Value v) {
    if (v.isChar()) return static_cast<uint32_t>(v.as.i);
    if (v.isInt()) {
        const int64_t cp = v.as.i;
        if (cp < 0 || cp > 0x10FFFF) throw vm.runtimeError(Thrown::IllegalArgumentException, "not a character: " + std::to_string(cp));
        return static_cast<uint32_t>(cp);
    }
    const std::string& s = vm.toStr(v, "character");
    if (s.empty()) throw vm.runtimeError(Thrown::NoSuchElementException, "a character is wanted, and the string is empty");
    size_t at = 0;
    return text::nextCodepoint(s, at);
}

// From 0xAARRGGBB, as programs write colours, to 0xRRGGBBAA.
uint32_t colorOf(VM& vm, Value v) {
    const uint64_t argb = static_cast<uint64_t>(vm.toInt(v, "colour"));
    return static_cast<uint32_t>(((argb & 0xFFFFFF) << 8) | ((argb >> 24) & 0xFF));
}

// A picture's pixels: the bytes value, checked against the size it is said to have.
std::vector<uint8_t>& pixelsOf(VM& vm, Value* args, int& width, int& height) {
    // Bytes, or the cells of a Grid<Color>, which are kept as the same four bytes a pixel.
    const bool cells = args[0].isObjType(ObjType::List) && asList(args[0])->packed() && asList(args[0])->pack->width == 4;
    if (!args[0].isObjType(ObjType::Bytes) && !cells) throw vm.runtimeError(Thrown::ClassCastException, "a picture's pixels are bytes");
    const int64_t w = vm.toInt(args[1], "width"), h = vm.toInt(args[2], "height");
    std::vector<uint8_t>& data = cells ? asList(args[0])->pack->block : asBytes(args[0])->data;
    if (w < 0 || h < 0 || w > 32768 || h > 32768 || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4 != data.size())
        throw vm.runtimeError(Thrown::IllegalArgumentException, "the picture's pixels do not fit its size");
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return data;
}

int clampInt(int64_t v) { return static_cast<int>(std::clamp<int64_t>(v, -(int64_t{1} << 30), int64_t{1} << 30)); }

Value listOf(VM& vm, std::vector<Value> items) {
    ObjList* list = newList(vm, std::move(items));
    vm.account(list->values().size() * sizeof(Value));
    return Value::object(list);
}

Value matchValue(VM& vm, const text::Match& match) {
    if (!match) return Value::null();
    return listOf(vm, {Value::integer(numberOf(match.face)), Value::integer(match.weight), Value::boolean(match.fakeBold), Value::boolean(match.fakeItalic)});
}

std::string tagName(uint32_t tag) {
    std::string s;
    for (int shift = 24; shift >= 0; shift -= 8) s += static_cast<char>((tag >> shift) & 0xFF);
    return s;
}

// ---- The software engine: what an Image's canvas draws with (D123) ----
//
// An image's pixels are four bytes each, red, green, blue and alpha, not multiplied by the
// alpha: the cells of its Grid<Color>, or a Bytes. A canvas on it has an origin and a clip in
// those pixels and a scale of pixels to the unit; what it draws is given in units. Shapes are
// made outlines in pixels and filled with the text engine's rasterizer, which gives each pixel
// the share of it the shape covers, so edges are smooth; rectangles and straight lines are put
// on whole pixels first, so they are crisp.

struct Target {
    uint8_t* rgba = nullptr;
    int width = 0, height = 0;             // the picture, in pixels
    double scale = 1;                      // pixels to the unit
    double originX = 0, originY = 0;       // where the canvas's (0, 0) is, in pixels
    int clipLeft = 0, clipTop = 0, clipRight = 0, clipBottom = 0;
};

// The pixels of a picture: a Bytes, or the cells of a Grid<Color>, checked against its size.
std::vector<uint8_t>& rgbaOf(VM& vm, Value v, int64_t w, int64_t h) {
    std::vector<uint8_t>* data = nullptr;
    if (v.isObjType(ObjType::Bytes)) data = &asBytes(v)->data;
    else if (v.isObjType(ObjType::List) && asList(v)->packed() && asList(v)->pack->width == 4 && asList(v)->pack->store == ListStore::Items) data = &asList(v)->pack->block;
    if (data == nullptr) throw vm.runtimeError(Thrown::ClassCastException, "a picture's pixels are bytes or a grid of colours");
    if (w < 0 || h < 0 || w > 32768 || h > 32768 || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4 != data->size())
        throw vm.runtimeError(Thrown::IllegalArgumentException, "the picture's pixels do not fit its size");
    return *data;
}

// target(pixels, width, height, scale, originX, originY, clipLeft, clipTop, clipRight, clipBottom): the first ten arguments.
Target targetOf(VM& vm, Value* args) {
    Target t;
    t.width = static_cast<int>(vm.toInt(args[1], "width"));
    t.height = static_cast<int>(vm.toInt(args[2], "height"));
    t.rgba = rgbaOf(vm, args[0], t.width, t.height).data();
    t.scale = vm.toNumber(args[3], "scale");
    t.originX = vm.toNumber(args[4], "origin");
    t.originY = vm.toNumber(args[5], "origin");
    t.clipLeft = std::max(0, clampInt(vm.toInt(args[6], "clip")));
    t.clipTop = std::max(0, clampInt(vm.toInt(args[7], "clip")));
    t.clipRight = std::min(t.width, clampInt(vm.toInt(args[8], "clip")));
    t.clipBottom = std::min(t.height, clampInt(vm.toInt(args[9], "clip")));
    if (!(t.scale > 0)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a canvas's scale is more than 0");
    return t;
}

double num(VM& vm, Value v) { return vm.toNumber(v, "a coordinate"); }

// Lays one pixel of colour (0xRRGGBBAA, its alpha already multiplied by any coverage: `sa`) over
// what is there, as drawGlyph does.
inline void blendPixel(uint8_t* d, uint32_t r, uint32_t g, uint32_t b, uint32_t sa) {
    if (sa == 0) return;
    const uint32_t da = d[3];
    if (sa == 255 || da == 0) {
        d[0] = static_cast<uint8_t>(r);
        d[1] = static_cast<uint8_t>(g);
        d[2] = static_cast<uint8_t>(b);
        d[3] = static_cast<uint8_t>(sa);
        return;
    }
    // Source over: what is underneath shows through by (255 - sa).
    const uint32_t keep = (da * (255 - sa) + 127) / 255;
    const uint32_t outA = sa + keep;
    d[0] = static_cast<uint8_t>((r * sa + d[0] * keep + outA / 2) / outA);
    d[1] = static_cast<uint8_t>((g * sa + d[1] * keep + outA / 2) / outA);
    d[2] = static_cast<uint8_t>((b * sa + d[2] * keep + outA / 2) / outA);
    d[3] = static_cast<uint8_t>(outA);
}

// A glyph or a shape's coverage laid over the target at (x, baseline) as drawGlyph places it,
// inside the clip; a colour glyph in its own colours, faded by the colour's alpha.
void blendImage(const Target& t, const text::GlyphImage& image, int x, int baseline, uint32_t color) {
    if (image.empty()) return;
    const int left = x + image.left, top = baseline - image.top;
    const int fromCol = std::max(0, t.clipLeft - left), toCol = std::min(image.width, t.clipRight - left);
    const int fromRow = std::max(0, t.clipTop - top), toRow = std::min(image.height, t.clipBottom - top);
    if (fromCol >= toCol || fromRow >= toRow) return;
    const uint32_t red = (color >> 24) & 0xFF, green = (color >> 16) & 0xFF, blue = (color >> 8) & 0xFF, alpha = color & 0xFF;
    for (int row = fromRow; row < toRow; row++) {
        uint8_t* d = t.rgba + (static_cast<size_t>(top + row) * static_cast<size_t>(t.width) + static_cast<size_t>(left + fromCol)) * 4;
        for (int col = fromCol; col < toCol; col++, d += 4) {
            if (image.color) {
                const uint8_t* s = &image.pixels[(static_cast<size_t>(row) * static_cast<size_t>(image.width) + static_cast<size_t>(col)) * 4];
                const uint32_t sa = s[3] * alpha / 255;
                if (sa == 0) continue;
                // Its colours are multiplied by its alpha: taken out again for the blend.
                blendPixel(d, std::min<uint32_t>(255, s[0] * 255 / std::max<uint32_t>(s[3], 1)), std::min<uint32_t>(255, s[1] * 255 / std::max<uint32_t>(s[3], 1)),
                           std::min<uint32_t>(255, s[2] * 255 / std::max<uint32_t>(s[3], 1)), sa);
            } else {
                const uint32_t coverage = image.pixels[static_cast<size_t>(row) * static_cast<size_t>(image.width) + static_cast<size_t>(col)];
                if (coverage != 0) blendPixel(d, red, green, blue, (coverage * alpha + 127) / 255);
            }
        }
    }
}

// An outline in the target's pixels (y growing downwards) filled in a colour.
void fillPath(const Target& t, const text::Path& downward, uint32_t color) {
    if (downward.empty()) return;
    // The rasterizer has y growing upwards: the outline turned over, and the result put back.
    text::Path up = downward;
    for (size_t i = 1; i < up.points.size(); i += 2) up.points[i] = -up.points[i];
    const text::GlyphImage image = text::rasterize(up, text::kShapeFineness);
    blendImage(t, image, 0, 0, color);
}

// A rectangle in pixels, (l, t) to (r, b), clockwise; or anticlockwise to cut it out of another.
void rectPath(text::Path& p, double l, double tp, double r, double b, bool reverse = false) {
    p.moveTo(static_cast<float>(l), static_cast<float>(tp));
    if (!reverse) {
        p.lineTo(static_cast<float>(r), static_cast<float>(tp));
        p.lineTo(static_cast<float>(r), static_cast<float>(b));
        p.lineTo(static_cast<float>(l), static_cast<float>(b));
    } else {
        p.lineTo(static_cast<float>(l), static_cast<float>(b));
        p.lineTo(static_cast<float>(r), static_cast<float>(b));
        p.lineTo(static_cast<float>(r), static_cast<float>(tp));
    }
    p.close();
}

// A rectangle with its corners rounded by quarter ellipses of radii rx, ry (an ellipse when they
// are half its size), in pixels; anticlockwise with `reverse`.
void roundPath(text::Path& p, double l, double tp, double r, double b, double rx, double ry, bool reverse = false) {
    rx = std::clamp(rx, 0.0, (r - l) / 2);
    ry = std::clamp(ry, 0.0, (b - tp) / 2);
    if (r <= l || b <= tp) return;
    const double k = 0.5522847498;  // how far a quarter circle's control points are, of its radius
    const double cx = rx * k, cy = ry * k;
    auto f = [](double v) { return static_cast<float>(v); };
    if (!reverse) {
        p.moveTo(f(l + rx), f(tp));
        p.lineTo(f(r - rx), f(tp));
        p.cubicTo(f(r - rx + cx), f(tp), f(r), f(tp + ry - cy), f(r), f(tp + ry));
        p.lineTo(f(r), f(b - ry));
        p.cubicTo(f(r), f(b - ry + cy), f(r - rx + cx), f(b), f(r - rx), f(b));
        p.lineTo(f(l + rx), f(b));
        p.cubicTo(f(l + rx - cx), f(b), f(l), f(b - ry + cy), f(l), f(b - ry));
        p.lineTo(f(l), f(tp + ry));
        p.cubicTo(f(l), f(tp + ry - cy), f(l + rx - cx), f(tp), f(l + rx), f(tp));
    } else {
        p.moveTo(f(l + rx), f(tp));
        p.cubicTo(f(l + rx - cx), f(tp), f(l), f(tp + ry - cy), f(l), f(tp + ry));
        p.lineTo(f(l), f(b - ry));
        p.cubicTo(f(l), f(b - ry + cy), f(l + rx - cx), f(b), f(l + rx), f(b));
        p.lineTo(f(r - rx), f(b));
        p.cubicTo(f(r - rx + cx), f(b), f(r), f(b - ry + cy), f(r), f(b - ry));
        p.lineTo(f(r), f(tp + ry));
        p.cubicTo(f(r), f(tp + ry - cy), f(r - rx + cx), f(tp), f(r - rx), f(tp));
    }
    p.close();
}

// Units to the target's pixels, on whole pixels for what sits on pixel edges.
inline double px(const Target& t, double units) { return units * t.scale; }
inline double edgeX(const Target& t, double x) { return std::round(t.originX + x * t.scale); }
inline double edgeY(const Target& t, double y) { return std::round(t.originY + y * t.scale); }
// How thick a line of one unit is, in whole pixels.
inline double thickness(const Target& t) { return std::max(1.0, std::round(t.scale)); }

// A line one unit thick through the middles of the pixels at its two ends, with square ends, as
// Java draws it; across and up and down it is on whole pixels.
void linePath(const Target& t, text::Path& p, double x1, double y1, double x2, double y2) {
    const double w = thickness(t);
    if (y1 == y2 || x1 == x2) {
        const double l = edgeX(t, std::min(x1, x2)), r = edgeX(t, std::max(x1, x2)) + w;
        const double tp = edgeY(t, std::min(y1, y2)), b = edgeY(t, std::max(y1, y2)) + w;
        rectPath(p, l, tp, r, b);
        return;
    }
    const double ax = t.originX + (x1 + 0.5) * t.scale, ay = t.originY + (y1 + 0.5) * t.scale;
    const double bx = t.originX + (x2 + 0.5) * t.scale, by = t.originY + (y2 + 0.5) * t.scale;
    const double dx = bx - ax, dy = by - ay, length = std::sqrt(dx * dx + dy * dy);
    const double ux = dx / length * w / 2, uy = dy / length * w / 2;  // along, half the thickness
    auto f = [](double v) { return static_cast<float>(v); };
    p.moveTo(f(ax - ux - uy), f(ay - uy + ux));
    p.lineTo(f(bx + ux - uy), f(by + uy + ux));
    p.lineTo(f(bx + ux + uy), f(by + uy - ux));
    p.lineTo(f(ax - ux + uy), f(ay - uy - ux));
    p.close();
}

// A colour given as 0xAARRGGBB, as the drawing engine wants it.
uint32_t engineColor(VM& vm, Value v) { return colorOf(vm, v); }

}  // namespace

// For the drawing code of the native layer, which is handed fonts by the same numbers.
std::shared_ptr<text::Font> textFontOf(VM& vm, Value v) { return fontOf(vm, v); }

void registerText(VM& vm) {
    ObjClass* lib = vm.alloc<ObjClass>(vm.intern("text"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        lib->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("text.") + name, minArgs, maxArgs, fn)));
    };

    // ---- The fonts there are ----

    def("families", 0, 0, [](VM& vm, int, Value*) -> Value {
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (std::string& family : FontBook::installed().families()) list->values().push_back(vm.str(std::move(family)));
        vm.account(list->values().size() * sizeof(Value));
        return Value::object(list);
    });

    def("hasFamily", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return Value::boolean(static_cast<bool>(FontBook::installed().matchExactly(vm.toStr(args[0], "font family"), 400, false)));
    });

    // The typefaces of a family, or every one installed if the family is null.
    def("typefaces", 1, 1, [](VM& vm, int, Value* args) -> Value {
        FontBook& book = FontBook::installed();
        const std::string wanted = args[0].isNull() ? std::string() : text::lowerCase(vm.toStr(args[0], "font family"));
        std::vector<Value> out;
        const std::vector<text::FaceInfo> faces = book.faces();
        for (size_t i = 0; i < faces.size(); i++) {
            if (!args[0].isNull() && text::lowerCase(faces[i].family) != wanted && text::lowerCase(faces[i].legacyFamily) != wanted) continue;
            if (auto face = book.face(static_cast<int>(i))) out.push_back(Value::integer(numberOf(face)));
        }
        return listOf(vm, std::move(out));
    });

    // How many fonts a file holds; 0 if it is not a font file.
    def("countIn", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::integer(Face::count(vm.toStr(args[0], "file path"))); });

    def("load", 2, 2, [](VM& vm, int, Value* args) -> Value {
        std::string error;
        auto face = Face::load(vm.toStr(args[0], "file path"), clampInt(vm.toInt(args[1], "index")), error);
        if (face == nullptr) throw vm.errorOf("IOException", error);
        return Value::integer(numberOf(face));
    });

    def("fromBytes", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::ClassCastException, "a font is read from bytes, not " + vm.stringify(args[0]));
        std::string error;
        auto face = Face::fromBytes(asBytes(args[0])->data, clampInt(vm.toInt(args[1], "index")), "<bytes>", error);
        if (face == nullptr) throw vm.errorOf("IOException", error);
        return Value::integer(numberOf(face));
    });

    // find(family, weight, italic): [typeface, weight, fakeBold, fakeItalic], or null if no
    // such family is installed.
    def("find", 3, 3, [](VM& vm, int, Value* args) -> Value {
        return matchValue(vm, FontBook::installed().matchExactly(vm.toStr(args[0], "font family"), clampInt(vm.toInt(args[1], "weight")), args[2].truthy()));
    });

    // The same for a character a font lacks: the font to take it from.
    def("fallback", 3, 3, [](VM& vm, int, Value* args) -> Value {
        return matchValue(vm, FontBook::installed().fallback(characterOf(vm, args[0]), clampInt(vm.toInt(args[1], "weight")), args[2].truthy()));
    });

    // ---- A typeface ----

    def("faceInfo", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto face = faceOf(vm, args[0]);
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        auto& out = list->values();
        out.push_back(vm.str(face->path()));
        out.push_back(Value::integer(face->index()));
        out.push_back(vm.str(face->family()));
        out.push_back(vm.str(face->style()));
        out.push_back(vm.str(face->fullName()));
        out.push_back(Value::integer(face->weight()));
        out.push_back(Value::integer(face->stretch()));
        out.push_back(Value::boolean(face->italic()));
        out.push_back(Value::boolean(face->monospace()));
        out.push_back(Value::boolean(face->symbol()));
        out.push_back(Value::boolean(face->variable()));
        out.push_back(Value::boolean(face->cubic()));
        out.push_back(Value::boolean(face->hasColor()));
        out.push_back(Value::integer(face->glyphCount()));
        out.push_back(Value::integer(face->unitsPerEm()));
        out.push_back(vm.str(face->legacyFamily()));
        vm.account(out.size() * sizeof(Value));
        return Value::object(list);
    });

    // Each axis of a variable font: [tag, least, default, most].
    def("faceAxes", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto face = faceOf(vm, args[0]);
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (const text::Axis& axis : face->axes()) {
            ObjList* one = newList(vm);
            list->values().push_back(Value::object(one));
            one->values().push_back(vm.str(tagName(axis.tag)));
            one->values().push_back(Value::number(axis.minimum));
            one->values().push_back(Value::number(axis.defaultValue));
            one->values().push_back(Value::number(axis.maximum));
        }
        return Value::object(list);
    });

    def("faceGlyph", 2, 2, [](VM& vm, int, Value* args) -> Value { return Value::integer(faceOf(vm, args[0])->glyphIndex(characterOf(vm, args[1]))); });

    // Measurements in the font's own units, for a weight: [ascent, descent, lineGap, xHeight, capHeight].
    def("faceMetrics", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto face = faceOf(vm, args[0]);
        const text::Coords coords = face->instance(static_cast<float>(vm.toNumber(args[1], "weight")));
        return listOf(vm, {Value::number(face->ascent(coords)), Value::number(face->descent(coords)), Value::number(face->lineGap(coords)),
                           Value::number(face->xHeight(coords)), Value::number(face->capHeight(coords))});
    });

    // A glyph's width in the font's units, for a weight.
    def("faceAdvance", 3, 3, [](VM& vm, int, Value* args) -> Value {
        auto face = faceOf(vm, args[0]);
        return Value::number(face->advance(clampInt(vm.toInt(args[1], "glyph")), face->instance(static_cast<float>(vm.toNumber(args[2], "weight")))));
    });

    // A glyph's outline for a weight, as a list: "M" x y, "L" x y, "Q" cx cy x y,
    // "C" c1x c1y c2x c2y x y, "Z". In the font's units, y upwards. Empty if it has none.
    def("faceOutline", 3, 3, [](VM& vm, int, Value* args) -> Value {
        auto face = faceOf(vm, args[0]);
        text::Path path;
        face->outline(clampInt(vm.toInt(args[1], "glyph")), face->instance(static_cast<float>(vm.toNumber(args[2], "weight"))), path);
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        static const char* const names[] = {"M", "L", "Q", "C", "Z"};
        static const int counts[] = {2, 2, 4, 6, 0};
        size_t p = 0;
        for (uint8_t verb : path.verbs) {
            list->values().push_back(vm.str(names[verb]));
            for (int i = 0; i < counts[verb]; i++) list->values().push_back(Value::number(path.points[p++]));
        }
        vm.account(list->values().size() * sizeof(Value));
        return Value::object(list);
    });

    // Reads the outlines of glyphs `first`, `first + step`, ... at a weight, as a test of
    // the reader: [how many have an outline, how many points those have, how many are not
    // inside a sensible box].
    def("faceCheck", 4, 4, [](VM& vm, int, Value* args) -> Value {
        auto face = faceOf(vm, args[0]);
        const int first = std::max(0, clampInt(vm.toInt(args[1], "first glyph"))), step = std::max(1, clampInt(vm.toInt(args[2], "step")));
        const text::Coords coords = face->instance(static_cast<float>(vm.toNumber(args[3], "weight")));
        int64_t drawn = 0, points = 0, wild = 0;
        const float limit = static_cast<float>(face->unitsPerEm()) * 16.0f;
        text::Path path;
        for (int glyph = first; glyph < face->glyphCount(); glyph += step) {
            path.clear();
            if (!face->outline(glyph, coords, path)) continue;
            drawn++;
            points += static_cast<int64_t>(path.points.size() / 2);
            float minX, minY, maxX, maxY;
            if (path.bounds(minX, minY, maxX, maxY) && !(minX > -limit && minY > -limit && maxX < limit && maxY < limit)) wild++;
            const float advance = face->advance(glyph, coords);
            if (!(advance >= 0 && advance < limit)) wild++;
        }
        return listOf(vm, {Value::integer(drawn), Value::integer(points), Value::integer(wild)});
    });

    // ---- A font: a typeface at a size ----

    // font(typeface, size, scale, weight, fakeBold, fakeItalic, hinting)
    def("font", 7, 7, [](VM& vm, int, Value* args) -> Value {
        text::Match match;
        match.face = faceOf(vm, args[0]);
        text::FontRequest request;
        request.size = static_cast<float>(vm.toNumber(args[1], "font size"));
        request.scale = static_cast<float>(vm.toNumber(args[2], "scale"));
        if (!(request.size > 0)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a font's size is more than 0");
        if (!(request.scale > 0)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a scale is more than 0");
        match.weight = clampInt(vm.toInt(args[3], "weight"));
        match.fakeBold = args[4].truthy();
        match.fakeItalic = args[5].truthy();
        request.weight = match.weight;
        request.italic = match.face->italic() || match.fakeItalic;
        request.hinting = args[6].truthy();
        auto font = Font::of(match, request);
        if (font == nullptr) throw vm.runtimeError(Thrown::IllegalStateException, "the font cannot be made");
        return Value::integer(numberOf(font));
    });

    // [ascent, descent, lineGap, xHeight, capHeight, weight], in device pixels.
    def("fontInfo", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto font = fontOf(vm, args[0]);
        return listOf(vm, {Value::integer(font->ascent()), Value::integer(font->descent()), Value::integer(font->lineGap()), Value::integer(font->xHeight()),
                           Value::integer(font->capHeight()), Value::integer(font->request().weight)});
    });

    def("width", 2, 2, [](VM& vm, int, Value* args) -> Value { return Value::integer(fontOf(vm, args[0])->width(vm.toStr(args[1], "text"))); });

    def("charWidth", 2, 2, [](VM& vm, int, Value* args) -> Value { return Value::integer(fontOf(vm, args[0])->charWidth(characterOf(vm, args[1]))); });

    // Where each character of a text starts, and where the last ends: one more number than
    // the text has characters.
    def("offsets", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto font = fontOf(vm, args[0]);
        const std::string& s = vm.toStr(args[1], "text");
        std::vector<Value> out;
        int x = 0;
        for (size_t at = 0; at < s.size();) {
            out.push_back(Value::integer(x));
            x += font->charWidth(text::nextCodepoint(s, at));
        }
        out.push_back(Value::integer(x));
        return listOf(vm, std::move(out));
    });

    // The typeface a character is taken from: the font's own, or another that has it.
    def("faceFor", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto font = fontOf(vm, args[0]);
        int glyph = 0;
        Font* used = font->fontFor(characterOf(vm, args[1]), glyph);
        if (glyph == 0) return Value::null();
        return Value::integer(numberOf(used->sharedFace()));
    });

    // What a character's picture takes up: [left, top, width, height, colour, advance], placed
    // as in drawText (left from the pen, top above the baseline). Null if nothing is drawn.
    def("glyphBox", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto font = fontOf(vm, args[0]);
        const uint32_t cp = characterOf(vm, args[1]);
        if (text::invisible(cp)) return Value::null();
        int glyph = 0;
        Font* used = font->fontFor(cp, glyph);
        const text::GlyphImage& image = used->image(glyph);
        if (image.empty()) return Value::null();
        // The picture has a margin: the box is of the pixels with something in them.
        int minX = image.width, minY = image.height, maxX = -1, maxY = -1;
        const size_t bytes = image.color ? 4 : 1;
        for (int y = 0; y < image.height; y++)
            for (int x = 0; x < image.width; x++) {
                const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * bytes;
                if (image.pixels[at + (image.color ? 3 : 0)] == 0) continue;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        if (maxX < 0) return Value::null();
        return listOf(vm, {Value::integer(image.left + minX), Value::integer(image.top - minY), Value::integer(maxX - minX + 1), Value::integer(maxY - minY + 1),
                           Value::boolean(image.color), Value::integer(used->advance(glyph))});
    });

    // ---- Pictures ----

    // drawText(pixels, width, height, font, text, x, baseline, colour): the text's width.
    def("drawText", 8, 8, [](VM& vm, int, Value* args) -> Value {
        int width, height;
        std::vector<uint8_t>& pixels = pixelsOf(vm, args, width, height);
        auto font = fontOf(vm, args[3]);
        const std::string& s = vm.toStr(args[4], "text");
        return Value::integer(text::drawText(pixels.data(), width, height, *font, s, clampInt(vm.toInt(args[5], "x")), clampInt(vm.toInt(args[6], "y")), colorOf(vm, args[7])));
    });

    // fill(pixels, width, height, x, y, w, h, colour): sets every pixel of a rectangle.
    def("fill", 8, 8, [](VM& vm, int, Value* args) -> Value {
        int width, height;
        std::vector<uint8_t>& pixels = pixelsOf(vm, args, width, height);
        const int64_t x = vm.toInt(args[3], "x"), y = vm.toInt(args[4], "y"), w = vm.toInt(args[5], "width"), h = vm.toInt(args[6], "height");
        const uint32_t color = colorOf(vm, args[7]);
        const int x0 = static_cast<int>(std::clamp<int64_t>(x, 0, width)), x1 = static_cast<int>(std::clamp<int64_t>(x + std::max<int64_t>(w, 0), 0, width));
        const int y0 = static_cast<int>(std::clamp<int64_t>(y, 0, height)), y1 = static_cast<int>(std::clamp<int64_t>(y + std::max<int64_t>(h, 0), 0, height));
        for (int row = y0; row < y1; row++) {
            uint8_t* d = &pixels[(static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(x0)) * 4];
            for (int col = x0; col < x1; col++, d += 4) {
                d[0] = static_cast<uint8_t>(color >> 24);
                d[1] = static_cast<uint8_t>(color >> 16);
                d[2] = static_cast<uint8_t>(color >> 8);
                d[3] = static_cast<uint8_t>(color);
            }
        }
        return Value::null();
    });

    // blend(pixels, width, height, x, y, w, h, colour): lays a colour over a rectangle.
    def("blend", 8, 8, [](VM& vm, int, Value* args) -> Value {
        int width, height;
        std::vector<uint8_t>& pixels = pixelsOf(vm, args, width, height);
        const int64_t x = vm.toInt(args[3], "x"), y = vm.toInt(args[4], "y"), w = vm.toInt(args[5], "width"), h = vm.toInt(args[6], "height");
        const int x0 = static_cast<int>(std::clamp<int64_t>(x, 0, width)), x1 = static_cast<int>(std::clamp<int64_t>(x + std::max<int64_t>(w, 0), 0, width));
        const int y0 = static_cast<int>(std::clamp<int64_t>(y, 0, height)), y1 = static_cast<int>(std::clamp<int64_t>(y + std::max<int64_t>(h, 0), 0, height));
        if (x1 <= x0 || y1 <= y0) return Value::null();
        // A rectangle is a glyph that covers all of its box.
        text::GlyphImage box;
        box.width = x1 - x0;
        box.height = 1;
        box.pixels.assign(static_cast<size_t>(box.width), 255);
        for (int row = y0; row < y1; row++) text::drawGlyph(pixels.data(), width, height, box, x0, row, colorOf(vm, args[7]));
        return Value::null();
    });

    // draw(pixels, width, height, other, otherWidth, otherHeight, x, y): lays another
    // picture over this one with its top-left corner at (x, y).
    def("draw", 8, 8, [](VM& vm, int, Value* args) -> Value {
        int width, height, otherWidth, otherHeight;
        std::vector<uint8_t>& pixels = pixelsOf(vm, args, width, height);
        const std::vector<uint8_t>& other = pixelsOf(vm, args + 3, otherWidth, otherHeight);
        // As a colour glyph, which is the same thing: colours multiplied by their alpha.
        text::GlyphImage image;
        image.width = otherWidth;
        image.height = otherHeight;
        image.color = true;
        image.pixels.resize(other.size());
        for (size_t i = 0; i + 3 < other.size(); i += 4) {
            const uint32_t a = other[i + 3];
            image.pixels[i] = static_cast<uint8_t>((other[i] * a + 127) / 255);
            image.pixels[i + 1] = static_cast<uint8_t>((other[i + 1] * a + 127) / 255);
            image.pixels[i + 2] = static_cast<uint8_t>((other[i + 2] * a + 127) / 255);
            image.pixels[i + 3] = static_cast<uint8_t>(a);
        }
        text::drawGlyph(pixels.data(), width, height, image, clampInt(vm.toInt(args[6], "x")), clampInt(vm.toInt(args[7], "y")), 0x000000FF);
        return Value::null();
    });

    def("encodePng", 3, 3, [](VM& vm, int, Value* args) -> Value {
        int width, height;
        const std::vector<uint8_t>& pixels = pixelsOf(vm, args, width, height);
        if (width == 0 || height == 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "a picture with no pixels cannot be written as PNG");
        ObjBytes* made = vm.alloc<ObjBytes>();
        made->data = image::encodePng(pixels.data(), width, height);
        vm.account(made->data.size());
        return Value::object(made);
    });

    // decodePng(bytes): [width, height, pixels].
    def("decodePng", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::ClassCastException, "a PNG picture is read from bytes, not " + vm.stringify(args[0]));
        const std::vector<uint8_t>& data = asBytes(args[0])->data;
        ObjBytes* made = vm.alloc<ObjBytes>();
        TempRoot keep(vm, Value::object(made));
        int width = 0, height = 0;
        std::string error;
        if (!image::decodePng(data.data(), data.size(), made->data, width, height, error)) throw vm.errorOf("IOException", error);
        vm.account(made->data.size());
        return listOf(vm, {Value::integer(width), Value::integer(height), Value::object(made)});
    });

    // ---- The software engine (D123): a canvas on an Image ----
    // Each takes the target first: pixels, width, height, scale, originX, originY and the clip
    // (left, top, right, bottom) in pixels; then a colour where one is drawn, then units.

    // fillRect covers w by h units; drawRect is an outline one unit thick that covers w + 1 by h + 1,
    // as Java's does.
    def("canvasFillRect", 15, 15, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const double x = num(vm, args[11]), y = num(vm, args[12]), w = num(vm, args[13]), h = num(vm, args[14]);
        if (!(w > 0) || !(h > 0)) return Value::null();
        text::Path p;
        rectPath(p, edgeX(t, x), edgeY(t, y), edgeX(t, x + w), edgeY(t, y + h));
        fillPath(t, p, engineColor(vm, args[10]));
        return Value::null();
    });
    def("canvasDrawRect", 15, 15, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const double x = num(vm, args[11]), y = num(vm, args[12]), w = num(vm, args[13]), h = num(vm, args[14]);
        if (w < 0 || h < 0) return Value::null();
        const double line = thickness(t);
        const double l = edgeX(t, x), tp = edgeY(t, y), r = edgeX(t, x + w) + line, b = edgeY(t, y + h) + line;
        text::Path p;
        rectPath(p, l, tp, r, b);
        if (r - l > 2 * line && b - tp > 2 * line) rectPath(p, l + line, tp + line, r - line, b - line, true);
        fillPath(t, p, engineColor(vm, args[10]));
        return Value::null();
    });
    // clear(target..., x, y, w, h): the pixels clear again, what is under the picture showing.
    def("canvasClear", 14, 14, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const double x = num(vm, args[10]), y = num(vm, args[11]), w = num(vm, args[12]), h = num(vm, args[13]);
        const int l = std::max(t.clipLeft, static_cast<int>(edgeX(t, x))), r = std::min(t.clipRight, static_cast<int>(edgeX(t, x + w)));
        const int tp = std::max(t.clipTop, static_cast<int>(edgeY(t, y))), b = std::min(t.clipBottom, static_cast<int>(edgeY(t, y + h)));
        for (int row = tp; row < b; row++)
            if (r > l) std::memset(t.rgba + (static_cast<size_t>(row) * static_cast<size_t>(t.width) + static_cast<size_t>(l)) * 4, 0, static_cast<size_t>(r - l) * 4);
        return Value::null();
    });
    // roundRect(target..., colour, x, y, w, h, arcW, arcH, fill)
    def("canvasRoundRect", 18, 18, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const double x = num(vm, args[11]), y = num(vm, args[12]), w = num(vm, args[13]), h = num(vm, args[14]);
        const double rx = px(t, num(vm, args[15])) / 2, ry = px(t, num(vm, args[16])) / 2;
        text::Path p;
        if (args[17].truthy()) {
            roundPath(p, t.originX + px(t, x), t.originY + px(t, y), t.originX + px(t, x + w), t.originY + px(t, y + h), rx, ry);
        } else {
            const double line = thickness(t);
            const double l = t.originX + px(t, x), tp = t.originY + px(t, y), r = t.originX + px(t, x + w) + line, b = t.originY + px(t, y + h) + line;
            roundPath(p, l, tp, r, b, rx + line / 2, ry + line / 2);
            roundPath(p, l + line, tp + line, r - line, b - line, rx - line / 2, ry - line / 2, true);
        }
        fillPath(t, p, engineColor(vm, args[10]));
        return Value::null();
    });
    // oval(target..., colour, x, y, w, h, fill): the oval inside the rectangle; an outline one
    // unit thick, covering w + 1 by h + 1.
    def("canvasOval", 16, 16, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const double x = num(vm, args[11]), y = num(vm, args[12]), w = num(vm, args[13]), h = num(vm, args[14]);
        text::Path p;
        if (args[15].truthy()) {
            const double l = t.originX + px(t, x), tp = t.originY + px(t, y), r = t.originX + px(t, x + w), b = t.originY + px(t, y + h);
            roundPath(p, l, tp, r, b, (r - l) / 2, (b - tp) / 2);
        } else {
            const double line = thickness(t);
            const double l = t.originX + px(t, x), tp = t.originY + px(t, y), r = t.originX + px(t, x + w) + line, b = t.originY + px(t, y + h) + line;
            roundPath(p, l, tp, r, b, (r - l) / 2, (b - tp) / 2);
            if (r - l > 2 * line && b - tp > 2 * line) roundPath(p, l + line, tp + line, r - line, b - line, (r - l) / 2 - line, (b - tp) / 2 - line, true);
        }
        fillPath(t, p, engineColor(vm, args[10]));
        return Value::null();
    });
    // line(target..., colour, x1, y1, x2, y2)
    def("canvasLine", 15, 15, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        text::Path p;
        linePath(t, p, num(vm, args[11]), num(vm, args[12]), num(vm, args[13]), num(vm, args[14]));
        fillPath(t, p, engineColor(vm, args[10]));
        return Value::null();
    });
    // polygon(target..., colour, xs, ys, n, fill, closed): filled, or its sides drawn as lines.
    def("canvasPolygon", 16, 16, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        if (!args[11].isObjType(ObjType::List) || !args[12].isObjType(ObjType::List)) throw vm.runtimeError(Thrown::ClassCastException, "a polygon's points are two lists");
        ObjList* xs = asList(args[11]);
        ObjList* ys = asList(args[12]);
        const int64_t n = std::min<int64_t>({vm.toInt(args[13], "count"), static_cast<int64_t>(xs->size()), static_cast<int64_t>(ys->size())});
        if (n < 2) return Value::null();
        text::Path p;
        auto at = [&](ObjList* list, int64_t i) { return num(vm, vm.listGet(list, static_cast<size_t>(i))); };
        if (args[14].truthy()) {
            for (int64_t i = 0; i < n; i++) {
                const float x = static_cast<float>(t.originX + px(t, at(xs, i))), y = static_cast<float>(t.originY + px(t, at(ys, i)));
                if (i == 0) p.moveTo(x, y);
                else p.lineTo(x, y);
            }
            p.close();
            // Each part filled once, however the sides cross: drawn one side at a time would
            // cover twice where they meet.
            fillPath(t, p, engineColor(vm, args[10]));
            return Value::null();
        }
        const int64_t sides = args[15].truthy() ? n : n - 1;
        for (int64_t i = 0; i < sides; i++) {
            text::Path line;
            linePath(t, line, at(xs, i), at(ys, i), at(xs, (i + 1) % n), at(ys, (i + 1) % n));
            fillPath(t, line, engineColor(vm, args[10]));
        }
        return Value::null();
    });
    // text(target..., colour, font, text, x, baseline): the width it took, in pixels.
    def("canvasText", 15, 15, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        auto font = fontOf(vm, args[11]);
        const std::string& s = vm.toStr(args[12], "text");
        const uint32_t color = engineColor(vm, args[10]);
        const int x = static_cast<int>(std::lround(t.originX + px(t, num(vm, args[13]))));
        const int baseline = static_cast<int>(std::lround(t.originY + px(t, num(vm, args[14]))));
        std::vector<text::PlacedGlyph> glyphs;
        const int total = font->place(s, glyphs);
        for (const text::PlacedGlyph& g : glyphs) {
            if (x + g.x > t.clipRight + 4096 || x + g.x + g.advance < t.clipLeft - 4096) continue;
            blendImage(t, g.font->image(g.glyph, color), x + g.x, baseline, color);
        }
        return Value::integer(total);
    });
    // picture(target..., from, fromWidth, fromHeight, sx, sy, sw, sh, x, y, w, h, alpha): a part
    // of another picture, its own pixels, put into a rectangle of units, smoothly when it is
    // made larger or smaller, see-through by `alpha` (0.0 to 1.0).
    def("canvasPicture", 22, 22, [](VM& vm, int, Value* args) -> Value {
        const Target t = targetOf(vm, args);
        const int64_t fw = vm.toInt(args[11], "width"), fh = vm.toInt(args[12], "height");
        const std::vector<uint8_t>& from = rgbaOf(vm, args[10], fw, fh);
        const double sx = num(vm, args[13]), sy = num(vm, args[14]), sw = num(vm, args[15]), sh = num(vm, args[16]);
        const double dl = t.originX + px(t, num(vm, args[17])), dt = t.originY + px(t, num(vm, args[18]));
        const double dw = px(t, num(vm, args[19])), dh = px(t, num(vm, args[20]));
        const double alpha = std::clamp(num(vm, args[21]), 0.0, 1.0);
        if (!(dw > 0) || !(dh > 0) || !(sw > 0) || !(sh > 0) || alpha == 0) return Value::null();
        const int l = std::max(t.clipLeft, static_cast<int>(std::floor(dl))), r = std::min(t.clipRight, static_cast<int>(std::ceil(dl + dw)));
        const int tp = std::max(t.clipTop, static_cast<int>(std::floor(dt))), b = std::min(t.clipBottom, static_cast<int>(std::ceil(dt + dh)));
        const bool same = std::abs(dw - sw) < 1e-9 && std::abs(dh - sh) < 1e-9 && dl == std::floor(dl) && dt == std::floor(dt) &&
                          sx == std::floor(sx) && sy == std::floor(sy);
        auto sample = [&](int64_t cx, int64_t cy, int c) -> double {
            cx = std::clamp<int64_t>(cx, 0, fw - 1);
            cy = std::clamp<int64_t>(cy, 0, fh - 1);
            return from[(static_cast<size_t>(cy) * static_cast<size_t>(fw) + static_cast<size_t>(cx)) * 4 + static_cast<size_t>(c)];
        };
        for (int row = tp; row < b; row++) {
            for (int col = l; col < r; col++) {
                double rgbaOut[4];
                if (same) {
                    const int64_t cx = static_cast<int64_t>(sx) + (col - static_cast<int64_t>(dl)), cy = static_cast<int64_t>(sy) + (row - static_cast<int64_t>(dt));
                    if (cx < 0 || cy < 0 || cx >= fw || cy >= fh) continue;
                    for (int c = 0; c < 4; c++) rgbaOut[c] = sample(cx, cy, c);
                } else {
                    // Where the middle of this pixel falls in the picture, and the four pixels round
                    // it weighed by how near, their colours multiplied by their alpha.
                    const double u = sx + ((col + 0.5) - dl) / dw * sw - 0.5, v = sy + ((row + 0.5) - dt) / dh * sh - 0.5;
                    if (u < sx - 0.5 || v < sy - 0.5 || u > sx + sw - 0.5 || v > sy + sh - 0.5) continue;
                    const int64_t x0 = static_cast<int64_t>(std::floor(u)), y0 = static_cast<int64_t>(std::floor(v));
                    const double fx = u - x0, fy = v - y0;
                    double acc[4] = {0, 0, 0, 0};
                    const double weights[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                    const int64_t cxs[4] = {x0, x0 + 1, x0, x0 + 1}, cys[4] = {y0, y0, y0 + 1, y0 + 1};
                    for (int k = 0; k < 4; k++) {
                        const double a = sample(cxs[k], cys[k], 3) / 255.0;
                        for (int c = 0; c < 3; c++) acc[c] += sample(cxs[k], cys[k], c) * a * weights[k];
                        acc[3] += a * weights[k];
                    }
                    if (acc[3] <= 0) continue;
                    for (int c = 0; c < 3; c++) rgbaOut[c] = acc[c] / acc[3];
                    rgbaOut[3] = acc[3] * 255;
                }
                const uint32_t sa = static_cast<uint32_t>(std::lround(rgbaOut[3] * alpha));
                if (sa == 0) continue;
                blendPixel(t.rgba + (static_cast<size_t>(row) * static_cast<size_t>(t.width) + static_cast<size_t>(col)) * 4,
                           static_cast<uint32_t>(std::lround(std::clamp(rgbaOut[0], 0.0, 255.0))), static_cast<uint32_t>(std::lround(std::clamp(rgbaOut[1], 0.0, 255.0))),
                           static_cast<uint32_t>(std::lround(std::clamp(rgbaOut[2], 0.0, 255.0))), std::min<uint32_t>(sa, 255));
            }
        }
        return Value::null();
    });

    // ---- An Image's pixels as a whole ----

    // crop(pixels, width, height, x, y, w, h, into): the part (x, y, w, h) into another picture's
    // pixels of w by h, what is outside the picture clear.
    def("imageCrop", 8, 8, [](VM& vm, int, Value* args) -> Value {
        const int64_t w = vm.toInt(args[1], "width"), h = vm.toInt(args[2], "height");
        const std::vector<uint8_t>& from = rgbaOf(vm, args[0], w, h);
        const int64_t x = vm.toInt(args[3], "x"), y = vm.toInt(args[4], "y"), cw = vm.toInt(args[5], "width"), ch = vm.toInt(args[6], "height");
        std::vector<uint8_t>& to = rgbaOf(vm, args[7], cw, ch);
        for (int64_t row = 0; row < ch; row++) {
            const int64_t fy = y + row;
            if (fy < 0 || fy >= h) continue;
            const int64_t fromCol = std::max<int64_t>(0, -x), toCol = std::min<int64_t>(cw, w - x);
            if (toCol <= fromCol) continue;
            std::memcpy(&to[static_cast<size_t>((row * cw + fromCol) * 4)], &from[static_cast<size_t>((fy * w + x + fromCol) * 4)], static_cast<size_t>(toCol - fromCol) * 4);
        }
        return Value::null();
    });
    // inkBox(pixels, width, height, background): [x, y, w, h] of the smallest rectangle outside
    // which every pixel is the background (0xAARRGGBB); null if every one is.
    def("imageInkBox", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const int64_t w = vm.toInt(args[1], "width"), h = vm.toInt(args[2], "height");
        const std::vector<uint8_t>& px = rgbaOf(vm, args[0], w, h);
        const uint32_t bg = colorOf(vm, args[3]);
        const uint8_t want[4] = {static_cast<uint8_t>(bg >> 24), static_cast<uint8_t>(bg >> 16), static_cast<uint8_t>(bg >> 8), static_cast<uint8_t>(bg)};
        int64_t minX = w, minY = h, maxX = -1, maxY = -1;
        for (int64_t y = 0; y < h; y++)
            for (int64_t x = 0; x < w; x++)
                if (std::memcmp(&px[static_cast<size_t>((y * w + x) * 4)], want, 4) != 0) {
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                }
        if (maxX < 0) return Value::null();
        return listOf(vm, {Value::integer(minX), Value::integer(minY), Value::integer(maxX - minX + 1), Value::integer(maxY - minY + 1)});
    });
    // samePixels(a, b): whether two pictures' pixels are the same, byte for byte.
    def("imageSame", 2, 2, [](VM&, int, Value* args) -> Value {
        auto bytesOf = [&](Value v) -> const std::vector<uint8_t>* {
            if (v.isObjType(ObjType::Bytes)) return &asBytes(v)->data;
            if (v.isObjType(ObjType::List) && asList(v)->packed()) return &asList(v)->pack->block;
            return nullptr;
        };
        const std::vector<uint8_t>* a = bytesOf(args[0]);
        const std::vector<uint8_t>* b = bytesOf(args[1]);
        return Value::boolean(a != nullptr && b != nullptr && *a == *b);
    });
    // hash(cells): a hash of a picture's pixels, the same for the same pixels, eight bytes at a
    // time. An Image's hashCode, with its size.
    def("imageHash", 1, 1, [](VM&, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List) || !asList(args[0])->packed()) return Value::integer(0);
        const std::vector<uint8_t>& px = asList(args[0])->pack->block;
        uint64_t h = 0x9E3779B97F4A7C15ull ^ px.size();
        size_t i = 0;
        for (; i + 8 <= px.size(); i += 8) {
            uint64_t word;
            std::memcpy(&word, &px[i], 8);
            h = (h ^ word) * 0xFF51AFD7ED558CCDull;
            h ^= h >> 32;
        }
        for (; i < px.size(); i++) h = (h ^ px[i]) * 0x100000001B3ull;
        h ^= h >> 29;
        return Value::integer(static_cast<int32_t>(static_cast<uint32_t>(h ^ (h >> 32))));
    });
    // setPixels(cells, bytes): a picture's pixels taken from bytes, four to a pixel.
    def("imageSetPixels", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List) || !asList(args[0])->packed() || !args[1].isObjType(ObjType::Bytes))
            throw vm.runtimeError(Thrown::ClassCastException, "expected a picture's cells and bytes");
        std::vector<uint8_t>& to = asList(args[0])->pack->block;
        const std::vector<uint8_t>& from = asBytes(args[1])->data;
        if (to.size() != from.size()) throw vm.runtimeError(Thrown::IllegalArgumentException, "the bytes do not fit the picture");
        std::memcpy(to.data(), from.data(), to.size());
        return Value::null();
    });

    vm.defineGlobal("__text", Value::object(lib));
    // Measuring changes nothing a program can see, so it may be done in a parallel section.
    vm.markNotParallel("__text", {"width", "charWidth", "faceGlyph"});
}

}  // namespace astra
