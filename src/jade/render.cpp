// Drawing with the graphics card: see render.h.

#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace astra::jade {

namespace {

// One program draws everything. The card is sent one record for each shape and makes its
// four corners itself. What the shape is decides how each of its pixels is coloured: a
// plain fill; a rounded rectangle or an oval, whose edge is smoothed by how far the pixel
// is from it; a glyph, whose strength comes from the atlas; or a picture.
const char* const kVertexSource = R"(#version 330 core
layout(location = 0) in vec4 aRect;    // middle, half size
layout(location = 1) in vec4 aShape;   // which way it lies, kind, corner radius
layout(location = 2) in vec4 aUv;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec4 aClip;
layout(location = 5) in vec2 aExtra;   // outline width, room for the smoothed edge
uniform vec2 uTarget;
uniform float uFlip;
out vec2 vLocal;
out vec2 vUv;
out vec4 vColor;
flat out vec2 vHalf;
flat out vec4 vClip;
flat out vec3 vParams;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    vec2 local = corner * (aRect.zw + vec2(aExtra.y));
    vec2 axis = aShape.xy;
    vec2 pos = aRect.xy + axis * local.x + vec2(-axis.y, axis.x) * local.y;
    vLocal = local;
    vHalf = aRect.zw;
    vUv = mix(aUv.xy, aUv.zw, corner * 0.5 + 0.5);
    vColor = aColor;
    vClip = aClip;
    vParams = vec3(aShape.z, aShape.w, aExtra.x);
    gl_Position = vec4(pos.x / uTarget.x * 2.0 - 1.0, (pos.y / uTarget.y * 2.0 - 1.0) * uFlip, 0.0, 1.0);
})";

const char* const kFragmentSource = R"(#version 330 core
in vec2 vLocal;
in vec2 vUv;
in vec4 vColor;
flat in vec2 vHalf;
flat in vec4 vClip;
flat in vec3 vParams;
uniform sampler2D uTex;
out vec4 frag;
float roundedBox(vec2 p, vec2 halfSize, float radius) {
    vec2 q = abs(p) - halfSize + vec2(radius);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}
void main() {
    vec2 at = gl_FragCoord.xy;
    if (at.x < vClip.x || at.y < vClip.y || at.x >= vClip.z || at.y >= vClip.w) discard;
    float kind = vParams.x;
    vec4 color = vec4(vColor.rgb * vColor.a, vColor.a);
    if (kind < 0.5) {
        frag = color;
    } else if (kind < 2.5) {
        float d;
        if (kind < 1.5) {
            d = roundedBox(vLocal, vHalf, vParams.y);
        } else {
            float k = length(vLocal / vHalf);
            d = (k - 1.0) / max(length(vec2(dFdx(k), dFdy(k))), 0.00001);
        }
        if (vParams.z > 0.0) d = abs(d + vParams.z * 0.5) - vParams.z * 0.5;
        frag = color * clamp(0.5 - d, 0.0, 1.0);
    } else if (kind < 3.5) {
        frag = color * texture(uTex, vUv).r;
    } else {
        frag = texture(uTex, vUv) * vColor.a;
    }
})";

const int kCoverageAtlas = 2048, kColorAtlas = 1024;
const int kLargestInAtlas = 400;  // anything larger is sent to the card on its own

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

GLuint makeTexture(bool smooth) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(kClampToEdge));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(kClampToEdge));
    return texture;
}

std::mutex& releasedMutex() {
    static auto* m = new std::mutex();
    return *m;
}
std::vector<int64_t>& released() {
    static auto* ids = new std::vector<int64_t>();
    return *ids;
}

}  // namespace

// ---- Starting ----

Renderer* Renderer::get(std::string& error) {
    static Renderer* renderer = nullptr;
    static std::string failure;
    static bool tried = false;
    if (!tried) {
        tried = true;
        auto* made = new Renderer();  // lives as long as the program
        if (made->start(failure)) renderer = made;
    }
    if (renderer == nullptr) error = failure;
    return renderer;
}

bool Renderer::start(std::string& error) {
    platform_ = Platform::get(error);
    if (platform_ == nullptr) return false;

    std::string log;
    const GLuint vs = compile(kVertexShader, kVertexSource, log), fs = compile(kFragmentShader, kFragmentSource, log);
    if (!log.empty()) {
        error = "the drawing program does not compile: " + log;
        return false;
    }
    program_ = gl.CreateProgram();
    gl.AttachShader(program_, vs);
    gl.AttachShader(program_, fs);
    gl.LinkProgram(program_);
    GLint linked = 0;
    gl.GetProgramiv(program_, kLinkStatus, &linked);
    if (!linked) {
        error = "the drawing program does not link";
        return false;
    }
    uTarget_ = gl.GetUniformLocation(program_, "uTarget");
    uFlip_ = gl.GetUniformLocation(program_, "uFlip");
    uTex_ = gl.GetUniformLocation(program_, "uTex");

    gl.GenVertexArrays(1, &vao_);
    gl.BindVertexArray(vao_);
    gl.GenBuffers(1, &vbo_);
    gl.BindBuffer(kArrayBuffer, vbo_);
    for (GLuint i = 0; i < 6; i++) {
        gl.EnableVertexAttribArray(i);
        gl.VertexAttribDivisor(i, 1);  // one record for each shape, not for each corner
    }

    GLint largest = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &largest);
    maxTexture_ = std::clamp(static_cast<int>(largest), 1024, 16384);

    coverage_.size = kCoverageAtlas;
    coverage_.color = false;
    coverage_.texture = makeTexture(false);
    coverage_.pixels.assign(static_cast<size_t>(kCoverageAtlas) * kCoverageAtlas, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(kR8), kCoverageAtlas, kCoverageAtlas, 0, kRed, GL_UNSIGNED_BYTE, coverage_.pixels.data());
    colors_.size = kColorAtlas;
    colors_.color = true;
    colors_.texture = makeTexture(false);
    colors_.pixels.assign(static_cast<size_t>(kColorAtlas) * kColorAtlas * 4, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(kRgba8), kColorAtlas, kColorAtlas, 0, GL_RGBA, GL_UNSIGNED_BYTE, colors_.pixels.data());

    // Colours are multiplied by their alpha before they are laid over what is there.
    glEnable(GL_BLEND);
    gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    return true;
}

std::string Renderer::description() const {
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* card = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    return std::string("OpenGL ") + (version != nullptr ? version : "?") + ", " + (card != nullptr ? card : "?");
}

// ---- Surfaces ----

void Renderer::releaseLater(int64_t id) {
    std::lock_guard<std::mutex> hold(releasedMutex());
    released().push_back(id);
}

void Renderer::drainReleased() {
    std::vector<int64_t> gone;
    {
        std::lock_guard<std::mutex> hold(releasedMutex());
        gone.swap(released());
    }
    for (int64_t id : gone) destroySurface(static_cast<int>(id));
}

Surface* Renderer::surface(int id) {
    auto found = surfaces_.find(id);
    return found != surfaces_.end() ? found->second.get() : nullptr;
}

int Renderer::createSurface(int width, int height) {
    drainReleased();
    if (width < 1 || height < 1 || width > maxTexture_ || height > maxTexture_) return 0;
    auto made = std::make_unique<Surface>();
    made->width = width;
    made->height = height;
    made->texture = makeTexture(true);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(kRgba8), width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.GenFramebuffers(1, &made->framebuffer);
    gl.BindFramebuffer(kFramebuffer, made->framebuffer);
    gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, made->texture, 0);
    const bool complete = gl.CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
    if (complete) {
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    gl.BindFramebuffer(kFramebuffer, 0);
    if (!complete) {
        gl.DeleteFramebuffers(1, &made->framebuffer);
        glDeleteTextures(1, &made->texture);
        return 0;
    }
    const int id = nextSurface_++;
    surfaces_[id] = std::move(made);
    return id;
}

void Renderer::destroySurface(int id) {
    Surface* s = surface(id);
    if (s == nullptr) return;
    // What is waiting to be drawn may draw on it or from it.
    if (!batch_.empty()) flush();
    if (target_ == id) target_ = 0;
    gl.DeleteFramebuffers(1, &s->framebuffer);
    glDeleteTextures(1, &s->texture);
    surfaces_.erase(id);
}

bool Renderer::resizeSurface(int id, int width, int height) {
    Surface* s = surface(id);
    if (s == nullptr || width < 1 || height < 1 || width > maxTexture_ || height > maxTexture_) return false;
    if (s->width == width && s->height == height) return true;
    if (!batch_.empty()) flush();
    // A new picture of the new size, with the old one drawn into its top left corner, takes
    // the old one's place. What the old one showed is kept, as much of it as fits, so that
    // a window shown again before the program has drawn at the new size is not blank.
    const int fresh = createSurface(width, height);
    if (fresh == 0) return false;
    s = surface(id);  // the table may have moved
    const int savedTarget = target_;
    const float savedClip[4] = {clip_[0], clip_[1], clip_[2], clip_[3]};
    setTarget(fresh, 0, 0, width, height);
    const float w = static_cast<float>(std::min(width, s->width)), h = static_cast<float>(std::min(height, s->height));
    image(id, 0, 0, w, h, 0, 0, w, h, 1.0f);
    flush();
    Surface* made = surface(fresh);
    std::swap(s->texture, made->texture);
    std::swap(s->framebuffer, made->framebuffer);
    std::swap(s->width, made->width);
    std::swap(s->height, made->height);
    target_ = savedTarget;
    std::memcpy(clip_, savedClip, sizeof clip_);
    destroySurface(fresh);  // which now holds the old picture
    return true;
}

void Renderer::upload(int id, const uint8_t* rgba) {
    Surface* s = surface(id);
    if (s == nullptr) return;
    if (!batch_.empty()) flush();
    const size_t count = static_cast<size_t>(s->width) * static_cast<size_t>(s->height);
    std::vector<uint8_t> multiplied(count * 4);
    for (size_t i = 0; i < count * 4; i += 4) {
        const uint32_t a = rgba[i + 3];
        multiplied[i] = static_cast<uint8_t>((rgba[i] * a + 127) / 255);
        multiplied[i + 1] = static_cast<uint8_t>((rgba[i + 1] * a + 127) / 255);
        multiplied[i + 2] = static_cast<uint8_t>((rgba[i + 2] * a + 127) / 255);
        multiplied[i + 3] = static_cast<uint8_t>(a);
    }
    glBindTexture(GL_TEXTURE_2D, s->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s->width, s->height, GL_RGBA, GL_UNSIGNED_BYTE, multiplied.data());
}

void Renderer::uploadPart(int id, const uint8_t* rgba, int rgbaWidth, int x, int y, int w, int h) {
    Surface* s = surface(id);
    if (s == nullptr || w <= 0 || h <= 0) return;
    if (!batch_.empty()) flush();
    std::vector<uint8_t> multiplied(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int row = 0; row < h; row++) {
        const uint8_t* from = rgba + (static_cast<size_t>(y + row) * static_cast<size_t>(rgbaWidth) + static_cast<size_t>(x)) * 4;
        uint8_t* to = multiplied.data() + static_cast<size_t>(row) * static_cast<size_t>(w) * 4;
        for (int col = 0; col < w * 4; col += 4) {
            const uint32_t a = from[col + 3];
            to[col] = static_cast<uint8_t>((from[col] * a + 127) / 255);
            to[col + 1] = static_cast<uint8_t>((from[col + 1] * a + 127) / 255);
            to[col + 2] = static_cast<uint8_t>((from[col + 2] * a + 127) / 255);
            to[col + 3] = static_cast<uint8_t>(a);
        }
    }
    glBindTexture(GL_TEXTURE_2D, s->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, multiplied.data());
}

bool Renderer::read(int id, std::vector<uint8_t>& rgba) {
    Surface* s = surface(id);
    if (s == nullptr) return false;
    if (!batch_.empty()) flush();
    rgba.resize(static_cast<size_t>(s->width) * static_cast<size_t>(s->height) * 4);
    gl.BindFramebuffer(kFramebuffer, s->framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, s->width, s->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    gl.BindFramebuffer(kFramebuffer, 0);
    // From colours multiplied by their alpha to colours as they are.
    for (size_t i = 0; i < rgba.size(); i += 4) {
        const uint32_t a = rgba[i + 3];
        if (a == 0 || a == 255) continue;
        for (size_t k = 0; k < 3; k++) rgba[i + k] = static_cast<uint8_t>(std::min<uint32_t>(255, (rgba[i + k] * 255 + a / 2) / a));
    }
    return true;
}

// ---- The atlases ----

bool Renderer::Atlas::place(int w, int h, int& x, int& y) {
    if (shelfX + w + 1 > size) {
        shelfX = 1;
        shelfY += shelfHeight + 1;
        shelfHeight = 0;
    }
    if (shelfY + h + 1 > size) return false;
    x = shelfX;
    y = shelfY;
    shelfX += w + 1;
    shelfHeight = std::max(shelfHeight, h);
    if (dirtyBottom <= dirtyTop) {
        dirtyTop = y;
        dirtyBottom = y + h;
    } else {
        dirtyTop = std::min(dirtyTop, y);
        dirtyBottom = std::max(dirtyBottom, y + h);
    }
    return true;
}

void Renderer::Atlas::reset() {
    std::fill(pixels.begin(), pixels.end(), static_cast<uint8_t>(0));
    shelfX = shelfY = 1;
    shelfHeight = 0;
    dirtyTop = 0;
    dirtyBottom = size;
}

void Renderer::resetAtlases() {
    // What is waiting to be drawn was placed by where things are in the atlases now.
    flush();
    coverage_.reset();
    colors_.reset();
    glyphs_.clear();
    shapes_.clear();
}

void Renderer::sendAtlas(Atlas& atlas) {
    if (atlas.dirtyBottom <= atlas.dirtyTop) return;
    const size_t bytes = atlas.color ? 4 : 1;
    glBindTexture(GL_TEXTURE_2D, atlas.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, atlas.dirtyTop, atlas.size, atlas.dirtyBottom - atlas.dirtyTop, atlas.color ? GL_RGBA : kRed, GL_UNSIGNED_BYTE,
                    atlas.pixels.data() + static_cast<size_t>(atlas.dirtyTop) * static_cast<size_t>(atlas.size) * bytes);
    atlas.dirtyTop = atlas.dirtyBottom = 0;
}

Renderer::AtlasEntry Renderer::store(const text::GlyphImage& image) {
    AtlasEntry entry;
    if (image.empty()) return entry;
    Atlas& atlas = image.color ? colors_ : coverage_;
    int x = 0, y = 0;
    if (!atlas.place(image.width, image.height, x, y)) {
        resetAtlases();
        if (!atlas.place(image.width, image.height, x, y)) return entry;
    }
    const size_t bytes = image.color ? 4 : 1;
    for (int row = 0; row < image.height; row++)
        std::memcpy(&atlas.pixels[(static_cast<size_t>(y + row) * static_cast<size_t>(atlas.size) + static_cast<size_t>(x)) * bytes],
                    &image.pixels[static_cast<size_t>(row) * static_cast<size_t>(image.width) * bytes], static_cast<size_t>(image.width) * bytes);
    entry.x = x;
    entry.y = y;
    entry.w = image.width;
    entry.h = image.height;
    entry.left = image.left;
    entry.top = image.top;
    entry.color = image.color;
    return entry;
}

// ---- Drawing ----

void Renderer::setTarget(int id, int clipLeft, int clipTop, int clipRight, int clipBottom) {
    if (id != target_) {
        if (!batch_.empty()) flush();
        target_ = id;
    }
    clip_[0] = static_cast<float>(clipLeft);
    clip_[1] = static_cast<float>(clipTop);
    clip_[2] = static_cast<float>(clipRight);
    clip_[3] = static_cast<float>(clipBottom);
}

Renderer::Instance& Renderer::add(GLuint texture) {
    // A shape with no texture can go with whatever is being drawn; one with a texture
    // starts a new run if the run so far uses another.
    if (runs_.empty()) {
        runs_.push_back(Run{texture, 0, 0});
    } else if (texture != 0 && runs_.back().texture != texture) {
        if (runs_.back().texture == 0) runs_.back().texture = texture;
        else runs_.push_back(Run{texture, batch_.size(), 0});
    }
    runs_.back().count++;
    batch_.emplace_back();
    Instance& made = batch_.back();
    made.ax = 1;
    made.ay = 0;
    made.u0 = made.v0 = made.u1 = made.v1 = 0;
    std::memcpy(made.clip, clip_, sizeof clip_);
    return made;
}

void Renderer::push(float cx, float cy, float hx, float hy, float kind, float radius, float stroke, float margin, uint32_t rgba, GLuint texture) {
    Instance& i = add(texture);
    i.cx = cx;
    i.cy = cy;
    i.hx = hx;
    i.hy = hy;
    i.kind = kind;
    i.radius = radius;
    i.stroke = stroke;
    i.margin = margin;
    i.r = static_cast<uint8_t>(rgba >> 24);
    i.g = static_cast<uint8_t>(rgba >> 16);
    i.b = static_cast<uint8_t>(rgba >> 8);
    i.a = static_cast<uint8_t>(rgba);
}

void Renderer::fillRect(float left, float top, float right, float bottom, uint32_t rgba) {
    if (right <= left || bottom <= top) return;
    push((left + right) / 2, (top + bottom) / 2, (right - left) / 2, (bottom - top) / 2, 0, 0, 0, 0, rgba);
}

void Renderer::roundRect(float left, float top, float right, float bottom, float radius, float stroke, uint32_t rgba) {
    if (right <= left || bottom <= top) return;
    const float hx = (right - left) / 2, hy = (bottom - top) / 2;
    push((left + right) / 2, (top + bottom) / 2, hx, hy, 1, std::clamp(radius, 0.0f, std::min(hx, hy)), stroke, 1, rgba);
}

void Renderer::oval(float left, float top, float right, float bottom, float stroke, uint32_t rgba) {
    if (right <= left || bottom <= top) return;
    push((left + right) / 2, (top + bottom) / 2, (right - left) / 2, (bottom - top) / 2, 2, 0, stroke, 1, rgba);
}

void Renderer::line(float x1, float y1, float x2, float y2, float width, uint32_t rgba) {
    // Coordinates name pixels, and a line runs through their middles, covering both ends.
    x1 += 0.5f;
    y1 += 0.5f;
    x2 += 0.5f;
    y2 += 0.5f;
    const float dx = x2 - x1, dy = y2 - y1, length = std::hypot(dx, dy);
    const float half = std::max(width, 1.0f) / 2;
    push((x1 + x2) / 2, (y1 + y2) / 2, length / 2 + half, half, 1, 0, 0, 1, rgba);
    Instance& made = batch_.back();
    if (length > 0) {
        made.ax = dx / length;
        made.ay = dy / length;
    }
}

void Renderer::polygon(const std::vector<float>& points, uint32_t rgba) {
    if (points.size() < 6) return;
    float minX = points[0], minY = points[1], maxX = minX, maxY = minY;
    for (size_t i = 0; i + 1 < points.size(); i += 2) {
        minX = std::min(minX, points[i]);
        maxX = std::max(maxX, points[i]);
        minY = std::min(minY, points[i + 1]);
        maxY = std::max(maxY, points[i + 1]);
    }
    if (!(maxX - minX < 16000 && maxY - minY < 16000)) return;
    // The shape is drawn once into a small picture of how much of each pixel it covers,
    // with the text engine's rasteriser, and kept by what it looks like: the arrows and
    // marks a toolkit draws are the same few shapes over and over.
    const float originX = std::floor(minX), originY = std::floor(minY);
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](int64_t v) {
        hash ^= static_cast<uint64_t>(v);
        hash *= 1099511628211ull;
    };
    for (size_t i = 0; i + 1 < points.size(); i += 2) {
        mix(std::lround((points[i] - originX) * 16));
        mix(std::lround((points[i + 1] - originY) * 16));
    }
    mix(static_cast<int64_t>(points.size()));

    AtlasEntry entry;
    auto known = shapes_.find(hash);
    const bool small = maxX - minX <= kLargestInAtlas && maxY - minY <= kLargestInAtlas;
    GLuint texture = coverage_.texture;
    float size = static_cast<float>(coverage_.size);
    if (known != shapes_.end()) {
        entry = known->second;
    } else {
        text::Path path;  // the rasteriser has y growing upwards
        path.moveTo(points[0] - originX, -(points[1] - originY));
        for (size_t i = 2; i + 1 < points.size(); i += 2) path.lineTo(points[i] - originX, -(points[i + 1] - originY));
        path.close();
        const text::GlyphImage image = text::rasterize(path);
        if (image.empty()) return;
        if (small) {
            entry = store(image);
            if (entry.w == 0) return;
            shapes_[hash] = entry;
        } else {
            // Too large to keep: sent on its own, for this once.
            texture = makeTexture(false);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(kR8), image.width, image.height, 0, kRed, GL_UNSIGNED_BYTE, image.pixels.data());
            temporaries_.push_back(texture);
            entry.w = image.width;
            entry.h = image.height;
            entry.left = image.left;
            entry.top = image.top;
            size = 0;
        }
    }
    const float left = originX + static_cast<float>(entry.left), top = originY - static_cast<float>(entry.top);
    push(left + entry.w / 2.0f, top + entry.h / 2.0f, entry.w / 2.0f, entry.h / 2.0f, 3, 0, 0, 0, rgba, texture);
    Instance& made = batch_.back();
    if (size > 0) {
        made.u0 = static_cast<float>(entry.x) / size;
        made.v0 = static_cast<float>(entry.y) / size;
        made.u1 = static_cast<float>(entry.x + entry.w) / size;
        made.v1 = static_cast<float>(entry.y + entry.h) / size;
    } else {
        made.u1 = made.v1 = 1;
    }
}

const Renderer::AtlasEntry& Renderer::glyphEntry(text::Font& font, int glyph, uint32_t foreground) {
    // Only a colour font's glyphs can depend on the colour of the text.
    const GlyphKey key{&font, static_cast<uint32_t>(glyph), font.face().hasColor() ? foreground | 0xFF : 0};
    auto known = glyphs_.find(key);
    if (known != glyphs_.end()) return known->second;
    const text::GlyphImage& image = font.image(glyph, foreground | 0xFF);
    AtlasEntry entry;
    if (!image.empty() && image.width <= kLargestInAtlas && image.height <= kLargestInAtlas) entry = store(image);
    return glyphs_[key] = entry;  // after store(), which may have emptied the table
}

int Renderer::text(text::Font& font, std::string_view utf8, int x, int baseline, uint32_t rgba) {
    std::vector<text::PlacedGlyph> placed;
    const int width = font.place(utf8, placed);
    for (const text::PlacedGlyph& g : placed) {
        const AtlasEntry entry = glyphEntry(*g.font, g.glyph, rgba);
        if (entry.w > 0) {
            const Atlas& atlas = entry.color ? colors_ : coverage_;
            const float left = static_cast<float>(x + g.x + entry.left), top = static_cast<float>(baseline - entry.top);
            push(left + entry.w / 2.0f, top + entry.h / 2.0f, entry.w / 2.0f, entry.h / 2.0f, entry.color ? 4.0f : 3.0f, 0, 0, 0, rgba, atlas.texture);
            Instance& made = batch_.back();
            const float size = static_cast<float>(atlas.size);
            made.u0 = static_cast<float>(entry.x) / size;
            made.v0 = static_cast<float>(entry.y) / size;
            made.u1 = static_cast<float>(entry.x + entry.w) / size;
            made.v1 = static_cast<float>(entry.y + entry.h) / size;
            continue;
        }
        // A glyph too large for the atlas (very large text) goes to the card on its own.
        const text::GlyphImage& image = g.font->image(g.glyph, rgba | 0xFF);
        if (image.empty()) continue;
        const GLuint texture = makeTexture(false);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(image.color ? kRgba8 : kR8), image.width, image.height, 0, image.color ? GL_RGBA : kRed,
                     GL_UNSIGNED_BYTE, image.pixels.data());
        temporaries_.push_back(texture);
        const float left = static_cast<float>(x + g.x + image.left), top = static_cast<float>(baseline - image.top);
        push(left + image.width / 2.0f, top + image.height / 2.0f, image.width / 2.0f, image.height / 2.0f, image.color ? 4.0f : 3.0f, 0, 0, 0, rgba, texture);
        batch_.back().u1 = batch_.back().v1 = 1;
    }
    return width;
}

void Renderer::image(int source, float srcLeft, float srcTop, float srcRight, float srcBottom, float left, float top, float right, float bottom, float alpha) {
    Surface* from = surface(source);
    if (from == nullptr || source == target_ || right <= left || bottom <= top) return;
    push((left + right) / 2, (top + bottom) / 2, (right - left) / 2, (bottom - top) / 2, 4, 0, 0, 0,
         0xFFFFFF00u | static_cast<uint32_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255)), from->texture);
    Instance& made = batch_.back();
    made.u0 = srcLeft / static_cast<float>(from->width);
    made.v0 = srcTop / static_cast<float>(from->height);
    made.u1 = srcRight / static_cast<float>(from->width);
    made.v1 = srcBottom / static_cast<float>(from->height);
}

void Renderer::clear(int left, int top, int right, int bottom) {
    Surface* s = surface(target_);
    if (s == nullptr) return;
    if (!batch_.empty()) flush();
    left = std::max(left, static_cast<int>(clip_[0]));
    top = std::max(top, static_cast<int>(clip_[1]));
    right = std::min(right, static_cast<int>(clip_[2]));
    bottom = std::min(bottom, static_cast<int>(clip_[3]));
    if (right <= left || bottom <= top) return;
    gl.BindFramebuffer(kFramebuffer, s->framebuffer);
    glEnable(GL_SCISSOR_TEST);
    glScissor(left, top, right - left, bottom - top);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    gl.BindFramebuffer(kFramebuffer, 0);
}

// ---- Sending it to the card ----

void Renderer::drawRuns(int width, int height, float flip) {
    glViewport(0, 0, width, height);
    gl.UseProgram(program_);
    gl.Uniform2f(uTarget_, static_cast<float>(width), static_cast<float>(height));
    gl.Uniform1f(uFlip_, flip);
    gl.Uniform1i(uTex_, 0);
    gl.ActiveTexture(kTexture0);
    gl.BindVertexArray(vao_);
    gl.BindBuffer(kArrayBuffer, vbo_);
    gl.BufferData(kArrayBuffer, static_cast<ptrdiff_t>(batch_.size() * sizeof(Instance)), batch_.data(), kStreamDraw);
    const GLsizei stride = sizeof(Instance);
    for (const Run& run : runs_) {
        if (run.count == 0) continue;
        const size_t base = run.first * sizeof(Instance);
        auto at = [&](size_t offset) { return reinterpret_cast<const void*>(base + offset); };
        gl.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, at(offsetof(Instance, cx)));
        gl.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, at(offsetof(Instance, ax)));
        gl.VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, at(offsetof(Instance, u0)));
        gl.VertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, at(offsetof(Instance, r)));
        gl.VertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, at(offsetof(Instance, clip)));
        gl.VertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, stride, at(offsetof(Instance, stroke)));
        glBindTexture(GL_TEXTURE_2D, run.texture != 0 ? run.texture : coverage_.texture);
        gl.DrawArraysInstanced(kTriangleStrip, 0, 4, static_cast<GLsizei>(run.count));
        batchesDrawn_++;
    }
    shapesDrawn_ += static_cast<int64_t>(batch_.size());
}

void Renderer::flush() {
    drainReleased();
    Surface* s = surface(target_);
    if (s != nullptr && !batch_.empty()) {
        sendAtlas(coverage_);
        sendAtlas(colors_);
        gl.BindFramebuffer(kFramebuffer, s->framebuffer);
        drawRuns(s->width, s->height, 1.0f);
        gl.BindFramebuffer(kFramebuffer, 0);
    }
    batch_.clear();
    runs_.clear();
    if (!temporaries_.empty()) {
        glDeleteTextures(static_cast<GLsizei>(temporaries_.size()), temporaries_.data());
        temporaries_.clear();
    }
}

bool Renderer::present(int id, PlatformWindow& window) {
    flush();
    Surface* s = surface(id);
    if (s == nullptr || !window.makeCurrent()) return false;
    const int width = window.width(), height = window.height();
    if (width < 1 || height < 1) return false;
    gl.BindFramebuffer(kFramebuffer, 0);
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // The surface as it is, from the top left corner: on the screen y grows upwards, so it
    // is turned over on the way.
    const float savedClip[4] = {clip_[0], clip_[1], clip_[2], clip_[3]};
    clip_[0] = clip_[1] = -1e9f;
    clip_[2] = clip_[3] = 1e9f;
    push(s->width / 2.0f, s->height / 2.0f, s->width / 2.0f, s->height / 2.0f, 4, 0, 0, 0, 0xFFFFFFFFu, s->texture);
    batch_.back().u1 = batch_.back().v1 = 1;
    std::memcpy(clip_, savedClip, sizeof clip_);
    glDisable(GL_BLEND);
    drawRuns(width, height, -1.0f);
    glEnable(GL_BLEND);
    batch_.clear();
    runs_.clear();
    return true;
}

}  // namespace astra::jade
