// The text engine and pictures as Astra programs see them: the built-in __text, which
// src/std/fonts.astra is written on.
//
// Fonts are known to a program by number. A font file once read stays read, and a font at a
// size keeps the pictures of its glyphs, for as long as the program runs: there are only as
// many as the program asks for. A picture's pixels, which is where the memory goes, are an
// ordinary `bytes` value that the collector frees.

#include <algorithm>
#include <cmath>
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
    if (number < 0 || static_cast<size_t>(number) >= h.faces.size()) throw vm.runtimeError("not a typeface");
    return h.faces[static_cast<size_t>(number)];
}

std::shared_ptr<Font> fontOf(VM& vm, Value v) {
    const int64_t number = vm.toInt(v, "font");
    Handles& h = handles();
    std::lock_guard<std::mutex> hold(h.mutex);
    if (number < 0 || static_cast<size_t>(number) >= h.fonts.size()) throw vm.runtimeError("not a font");
    return h.fonts[static_cast<size_t>(number)];
}

// A character given as a char, a one-character string or its number.
uint32_t characterOf(VM& vm, Value v) {
    if (v.isChar()) return static_cast<uint32_t>(v.as.i);
    if (v.isInt()) {
        const int64_t cp = v.as.i;
        if (cp < 0 || cp > 0x10FFFF) throw vm.runtimeError("not a character: " + std::to_string(cp));
        return static_cast<uint32_t>(cp);
    }
    const std::string& s = vm.toStr(v, "character");
    if (s.empty()) throw vm.runtimeError("a character is wanted, and the string is empty");
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
    if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError("a picture's pixels are bytes");
    const int64_t w = vm.toInt(args[1], "width"), h = vm.toInt(args[2], "height");
    std::vector<uint8_t>& data = asBytes(args[0])->data;
    if (w < 0 || h < 0 || w > 32768 || h > 32768 || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4 != data.size())
        throw vm.runtimeError("the picture's pixels do not fit its size");
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
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError("a font is read from bytes, not " + vm.stringify(args[0]));
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
        if (!(request.size > 0)) throw vm.runtimeError("a font's size is more than 0");
        if (!(request.scale > 0)) throw vm.runtimeError("a scale is more than 0");
        match.weight = clampInt(vm.toInt(args[3], "weight"));
        match.fakeBold = args[4].truthy();
        match.fakeItalic = args[5].truthy();
        request.weight = match.weight;
        request.italic = match.face->italic() || match.fakeItalic;
        request.hinting = args[6].truthy();
        auto font = Font::of(match, request);
        if (font == nullptr) throw vm.runtimeError("the font cannot be made");
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
        if (width == 0 || height == 0) throw vm.runtimeError("a picture with no pixels cannot be written as PNG");
        ObjBytes* made = vm.alloc<ObjBytes>();
        made->data = image::encodePng(pixels.data(), width, height);
        vm.account(made->data.size());
        return Value::object(made);
    });

    // decodePng(bytes): [width, height, pixels].
    def("decodePng", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::Bytes)) throw vm.runtimeError("a PNG picture is read from bytes, not " + vm.stringify(args[0]));
        const std::vector<uint8_t>& data = asBytes(args[0])->data;
        ObjBytes* made = vm.alloc<ObjBytes>();
        TempRoot keep(vm, Value::object(made));
        int width = 0, height = 0;
        std::string error;
        if (!image::decodePng(data.data(), data.size(), made->data, width, height, error)) throw vm.errorOf("IOException", error);
        vm.account(made->data.size());
        return listOf(vm, {Value::integer(width), Value::integer(height), Value::object(made)});
    });

    vm.defineGlobal("__text", Value::object(lib));
    // Measuring changes nothing a program can see, so it may be done in a parallel section.
    vm.markNotParallel("__text", {"width", "charWidth", "faceGlyph"});
}

}  // namespace astra
