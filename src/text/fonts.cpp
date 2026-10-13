// The fonts there are, and choosing among them: see fonts.h.

#include "fonts.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace astra::text {

std::string lowerCase(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '"' || s[a] == '\'')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '"' || s[b - 1] == '\'')) b--;
    return s.substr(a, b - a);
}

}  // namespace

FontBook& FontBook::installed() {
    static FontBook book;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string error;
        for (const std::string& path : installedFontFiles()) book.addFile(path, error);
    });
    return book;
}

int FontBook::addFile(const std::string& path, std::string& error) {
    auto data = openFontFile(path);
    if (data == nullptr) {
        error = "cannot read " + path;
        return 0;
    }
    const Reader r(data->data, data->size);
    const uint32_t kind = r.u32(0);
    int count = 0;
    if (kind == tag("ttcf")) count = static_cast<int>(std::min<uint32_t>(r.u32(8), 1024));
    else if (kind == 0x00010000 || kind == tag("true") || kind == tag("OTTO")) count = 1;
    if (count == 0) {
        error = path + ": not a TrueType or OpenType font";
        return 0;
    }
    int added = 0;
    std::lock_guard<std::recursive_mutex> hold(mutex_);
    for (int i = 0; i < count; i++) {
        std::string why;
        auto face = Face::fromData(data, i, path, why);
        if (face == nullptr) {
            if (error.empty()) error = why;
            continue;
        }
        addFace(face, false);
        added++;
    }
    return added;
}

void FontBook::addFace(const std::shared_ptr<Face>& face, bool keep) {
    FaceInfo info;
    info.path = face->path();
    info.index = face->index();
    info.family = face->family();
    info.legacyFamily = face->legacyFamily();
    info.style = face->style();
    info.fullName = face->fullName();
    info.weight = face->weight();
    info.stretch = face->stretch();
    info.italic = face->italic();
    info.monospace = face->monospace();
    info.symbol = face->symbol();
    info.cubic = face->cubic();
    info.color = face->hasColor();
    info.variable = face->variable();
    info.minWeight = info.maxWeight = info.weight;
    for (const Axis& axis : face->axes()) {
        if (axis.tag == tag("wght")) {
            info.minWeight = static_cast<int>(axis.minimum);
            info.maxWeight = static_cast<int>(axis.maximum);
        } else if (axis.tag == tag("ital") || axis.tag == tag("slnt")) {
            info.hasSlant = true;
        }
    }

    const int which = static_cast<int>(faces_.size());
    auto note = [&](std::unordered_map<std::string, std::vector<int>>& map, const std::string& name) {
        if (name.empty()) return;
        std::vector<int>& list = map[lowerCase(name)];
        if (std::find(list.begin(), list.end(), which) == list.end()) list.push_back(which);
    };
    note(byFamily_, info.family);
    if (lowerCase(info.legacyFamily) != lowerCase(info.family)) note(byLegacy_, info.legacyFamily);
    note(byLegacy_, info.fullName);

    faces_.push_back(std::move(info));
    loaded_.push_back(keep ? face : nullptr);
    failed_.push_back(false);
    fallbacks_.clear();
    fallbackOrderMade_ = false;
}

std::vector<FaceInfo> FontBook::faces() {
    std::lock_guard<std::recursive_mutex> hold(mutex_);
    return faces_;
}

std::vector<std::string> FontBook::families() {
    std::lock_guard<std::recursive_mutex> hold(mutex_);
    std::set<std::string> seen;
    std::vector<std::string> out;
    for (const FaceInfo& info : faces_)
        if (seen.insert(lowerCase(info.family)).second) out.push_back(info.family);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const std::string la = lowerCase(a), lb = lowerCase(b);
        return la != lb ? la < lb : a < b;
    });
    return out;
}

bool FontBook::hasFamily(const std::string& family) {
    std::lock_guard<std::recursive_mutex> hold(mutex_);
    const std::string key = lowerCase(trim(family));
    return byFamily_.count(key) != 0 || byLegacy_.count(key) != 0;
}

std::shared_ptr<Face> FontBook::face(int which) {
    std::lock_guard<std::recursive_mutex> hold(mutex_);
    if (which < 0 || static_cast<size_t>(which) >= faces_.size()) return nullptr;
    const size_t i = static_cast<size_t>(which);
    if (loaded_[i] != nullptr || failed_[i]) return loaded_[i];
    // The other fonts of the same file share its bytes.
    std::shared_ptr<const FontData> data;
    for (size_t k = 0; k < faces_.size() && data == nullptr; k++)
        if (loaded_[k] != nullptr && faces_[k].path == faces_[i].path) data = loaded_[k]->data();
    if (data == nullptr) data = openFontFile(faces_[i].path);
    std::string error;
    if (data != nullptr) loaded_[i] = Face::fromData(data, faces_[i].index, faces_[i].path, error);
    if (loaded_[i] == nullptr) failed_[i] = true;
    return loaded_[i];
}

// Picks among the fonts of one family: first the width, then upright or italic, then the
// weight, each as near to what was asked for as the family has. (The order CSS matches in.)
Match FontBook::choose(const std::vector<int>& candidates, int weight, bool italic, int stretch, bool legacy) {
    Match out;
    if (candidates.empty()) return out;

    // Asked for by its four-style name, "Segoe UI Semibold", a family is its own regular:
    // plain means the lightest it has and bold the heaviest.
    int wanted = weight;
    if (legacy) {
        int lightest = 1000, heaviest = 0;
        for (int c : candidates) {
            lightest = std::min(lightest, faces_[static_cast<size_t>(c)].minWeight);
            heaviest = std::max(heaviest, faces_[static_cast<size_t>(c)].maxWeight);
        }
        wanted = weight >= 600 ? heaviest : lightest;
    }

    // How far a font's weight is from the one wanted, in the order of preference: a weight
    // in the right direction first (lighter for light, heavier for bold), then the other.
    auto weightOf = [&](const FaceInfo& f) { return std::clamp(wanted, f.minWeight, f.maxWeight); };
    auto weightCost = [&](const FaceInfo& f) {
        const int w = weightOf(f);
        if (w == wanted) return 0;
        if (wanted >= 400 && wanted <= 500) {
            if (w >= 400 && w <= 500) return 1;
            return w < wanted ? 1000 + (wanted - w) : 3000 + (w - wanted);
        }
        if (wanted < 400) return w < wanted ? wanted - w : 2000 + (w - wanted);
        return w > wanted ? w - wanted : 2000 + (wanted - w);
    };

    int best = -1;
    long bestCost = 0;
    for (int c : candidates) {
        const FaceInfo& f = faces_[static_cast<size_t>(c)];
        long cost = static_cast<long>(std::abs(f.stretch - stretch)) * 100000;
        if (f.italic != italic && !(italic && f.hasSlant)) cost += italic ? 20000 : 40000;
        cost += weightCost(f);
        if (best < 0 || cost < bestCost) {
            best = c;
            bestCost = cost;
        }
    }
    const FaceInfo& f = faces_[static_cast<size_t>(best)];
    out.face = face(best);
    if (out.face == nullptr) return Match();
    out.which = best;
    out.weight = weightOf(f);
    out.fakeBold = !legacy && weight >= 600 && out.weight <= 500;
    out.fakeItalic = italic && !f.italic && !f.hasSlant;
    return out;
}

Match FontBook::matchOne(const std::string& lowerFamily, int weight, bool italic, int stretch) {
    auto it = byFamily_.find(lowerFamily);
    if (it != byFamily_.end()) return choose(it->second, weight, italic, stretch, false);
    it = byLegacy_.find(lowerFamily);
    if (it != byLegacy_.end()) {
        // Within such a family the width does not choose: it is part of the name.
        int own = stretch;
        if (!it->second.empty()) own = faces_[static_cast<size_t>(it->second.front())].stretch;
        return choose(it->second, weight, italic, own, true);
    }
    return Match();
}

Match FontBook::matchExactly(const std::string& family, int weight, bool italic, int stretch) {
    std::lock_guard<std::recursive_mutex> hold(mutex_);
    size_t start = 0;
    while (start <= family.size()) {
        size_t comma = family.find(',', start);
        if (comma == std::string::npos) comma = family.size();
        const std::string name = lowerCase(trim(family.substr(start, comma - start)));
        start = comma + 1;
        if (name.empty()) continue;

        std::string logical;
        if (name == "sansserif" || name == "sans-serif" || name == "dialog" || name == "dialoginput" || name == "system-ui" || name == "default")
            logical = name == "dialoginput" ? "monospaced" : "sansserif";
        else if (name == "serif") logical = "serif";
        else if (name == "monospaced" || name == "monospace") logical = "monospaced";
        if (!logical.empty()) {
            for (const std::string& candidate : logicalFamily(logical))
                if (Match m = matchOne(lowerCase(candidate), weight, italic, stretch)) return m;
            continue;
        }
        if (Match m = matchOne(name, weight, italic, stretch)) return m;
    }
    return Match();
}

Match FontBook::match(const std::string& family, int weight, bool italic, int stretch) {
    std::lock_guard<std::recursive_mutex> hold(mutex_);
    if (Match m = matchExactly(family, weight, italic, stretch)) return m;
    if (Match m = matchExactly("sansserif", weight, italic, stretch)) return m;
    // No font of the system's usual ones: any at all that has letters.
    for (size_t i = 0; i < faces_.size(); i++) {
        if (faces_[i].symbol) continue;
        auto f = face(static_cast<int>(i));
        if (f == nullptr || !f->has('a')) continue;
        if (Match m = matchOne(lowerCase(faces_[i].family), weight, italic, stretch)) return m;
    }
    return Match();
}

Match FontBook::fallback(uint32_t codepoint, int weight, bool italic) {
    std::lock_guard<std::recursive_mutex> hold(mutex_);
    const bool bold = weight >= 600;
    const uint64_t key = (static_cast<uint64_t>(codepoint) << 2) | (bold ? 2u : 0u) | (italic ? 1u : 0u);
    // The font found, in the style asked for if its family has that style with the character in it.
    auto styled = [&](int candidate) {
        const FaceInfo& info = faces_[static_cast<size_t>(candidate)];
        Match m = matchOne(lowerCase(info.family), weight, italic, info.stretch);
        if (m && m.face->has(codepoint)) return m;
        m.face = face(candidate);
        m.which = candidate;
        m.weight = std::clamp(weight, info.minWeight, info.maxWeight);
        m.fakeBold = bold && m.weight <= 500;
        m.fakeItalic = italic && !info.italic && !info.hasSlant;
        return m;
    };
    auto known = fallbacks_.find(key);
    if (known != fallbacks_.end()) return known->second < 0 ? Match() : styled(known->second);

    if (!fallbackOrderMade_) {
        // The system's own fonts for other scripts first, then every other family, plain
        // text fonts before the symbol ones.
        fallbackOrder_.clear();
        std::set<std::string> seen;
        auto addFamily = [&](const std::string& lowerFamily) {
            if (!seen.insert(lowerFamily).second) return;
            auto it = byFamily_.find(lowerFamily);
            if (it == byFamily_.end()) return;
            // One font stands for its family here: the most ordinary one it has.
            int best = -1, bestCost = 0;
            for (int c : it->second) {
                const FaceInfo& f = faces_[static_cast<size_t>(c)];
                const int cost = std::abs(f.stretch - 5) * 1000 + (f.italic ? 500 : 0) + std::abs(std::clamp(400, f.minWeight, f.maxWeight) - 400);
                if (best < 0 || cost < bestCost) {
                    best = c;
                    bestCost = cost;
                }
            }
            if (best >= 0) fallbackOrder_.push_back(best);
        };
        for (const std::string& family : fallbackFamilies()) addFamily(lowerCase(family));
        std::vector<std::string> rest;
        for (const FaceInfo& f : faces_)
            if (!f.symbol) rest.push_back(lowerCase(f.family));
        std::sort(rest.begin(), rest.end());
        for (const std::string& family : rest) addFamily(family);
        for (const FaceInfo& f : faces_)
            if (f.symbol) addFamily(lowerCase(f.family));
        fallbackOrderMade_ = true;
    }

    for (int candidate : fallbackOrder_) {
        auto f = face(candidate);
        if (f == nullptr || !f->has(codepoint)) continue;
        fallbacks_[key] = candidate;
        return styled(candidate);
    }
    fallbacks_[key] = -1;
    return Match();
}

}  // namespace astra::text
