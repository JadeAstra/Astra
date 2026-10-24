// `astra doc`: a reference page for Astra source files, the standard library's among them.
//
// Each file is parsed with the compiler's own parser, which finds its declarations and what
// each belongs to. What a declaration says of itself is the comment written right above it,
// with nothing between; a comment followed by a blank line stands on its own, as a heading
// (`// ---- Lists ----`, `// -- string --`) or prose between the declarations. A declaration is
// shown as it is written, up to its body, with `external` left out. Private and internal members,
// and names beginning with two underscores, are the code's own and are left out.
// So is a comment standing on its own whose first line begins `Inside Astra:`, a note for
// whoever works on the code (where the C++ is, how it is reached); and the starting value of a
// variable, but for a `const`'s, which is part of what it is.
//
//   astra doc <file>... [-o page.html] [--template page.html] [--title text]
//   astra doc --missing <file>...     the declarations nothing is written about, file:line name
//
// With --template, the page is that file with the reference put where it says <!-- astra doc -->;
// otherwise it is a page of its own. docs/language.html is made this way from
// docs/language-template.html and src/std/*.astra.

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "lexer.h"
#include "parser.h"

namespace astra {

namespace {

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

bool isComment(const std::string& line) { return trim(line).rfind("//", 0) == 0; }
bool isBlank(const std::string& line) { return trim(line).empty(); }

// A comment line's text: what follows `//` and one space.
std::string commentText(const std::string& line) {
    std::string t = trim(line).substr(2);
    if (!t.empty() && t[0] == ' ') t.erase(0, 1);
    return t;
}

// Prose with `code` in backticks, as HTML.
// The references to Astra's design records a comment carries for its maintainers, `(D35)`,
// `(D99, D102 to D105)`, `(M6.6 part 4)`, `(L14)`, mean nothing to a reader of the reference:
// left out of it.
std::string withoutReferences(const std::string& s) {
    static const std::regex reference(R"(\s*\((?:[DGLMR]\d+(?:\.\d+)*(?: part \d+)?(?:,\s*|\s+to\s+|\s+and\s+|;\s*)?)+\))");
    return std::regex_replace(s, reference, "");
}

std::string inlineText(const std::string& raw) {
    const std::string s = withoutReferences(raw);
    std::string out;
    bool code = false;
    std::string run;
    for (char c : s) {
        if (c == '`') {
            out += code ? "<code>" + escape(run) + "</code>" : escape(run);
            run.clear();
            code = !code;
        } else {
            run += c;
        }
    }
    out += code ? "`" + escape(run) : escape(run);
    return out;
}

// Comment lines as HTML: paragraphs, broken at empty lines; lines indented past the text
// around them (an example) kept as they are.
std::string commentHtml(const std::vector<std::string>& texts) {
    std::string out, paragraph, code;
    auto endParagraph = [&] {
        if (!paragraph.empty()) out += "<p>" + inlineText(paragraph) + "</p>\n";
        paragraph.clear();
    };
    auto endCode = [&] {
        if (!code.empty()) out += "<pre>" + escape(code) + "</pre>\n";
        code.clear();
    };
    for (const std::string& t : texts) {
        if (trim(t).empty()) {
            endParagraph();
            endCode();
        } else if (t.size() >= 2 && t[0] == ' ' && t[1] == ' ') {
            endParagraph();
            code += t.substr(2) + "\n";
        } else {
            endCode();
            if (!paragraph.empty()) paragraph += ' ';
            paragraph += trim(t);
        }
    }
    endParagraph();
    endCode();
    return out;
}

// A declaration as written, from its first line (1-based) to where its body begins, joined
// onto one line.
std::string declarationText(const std::vector<std::string>& lines, int first) {
    std::string text;
    int depth = 0;
    for (size_t at = static_cast<size_t>(first - 1); at < lines.size(); at++) {
        const std::string& line = lines[at];
        bool done = false;
        std::string part;
        char quote = 0;
        for (size_t k = 0; k < line.size(); k++) {
            const char c = line[k];
            if (quote != 0) {
                part += c;
                if (c == '\\' && k + 1 < line.size()) part += line[++k];
                else if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
                part += c;
                continue;
            }
            if (c == '/' && k + 1 < line.size() && line[k + 1] == '/') break;  // a comment after it
            if (c == '(' || c == '[' || c == '<') depth++;
            if ((c == ')' || c == ']' || c == '>') && depth > 0 && !(c == '>' && k > 0 && line[k - 1] == '-')) depth--;
            if (depth == 0 && (c == '{' || (c == '=' && k + 1 < line.size() && line[k + 1] == '>'))) {
                done = true;
                break;
            }
            part += c;
        }
        if (!text.empty()) text += ' ';
        text += trim(part);
        if (done || depth == 0) break;
    }
    // `external` says where it is written, which is nobody's business here.
    for (const char* word : {"external "}) {
        const size_t at = text.find(word);
        if (at != std::string::npos && (at == 0 || text[at - 1] == ' ')) text.erase(at, std::string(word).size());
    }
    // A variable's starting value is how it is made, not what it is, but for a constant's.
    if (text.rfind("const ", 0) != 0 && (text.rfind("val ", 0) == 0 || text.rfind("var ", 0) == 0 || text.find(" val ") != std::string::npos ||
                                          text.find(" var ") != std::string::npos)) {
        int level = 0;
        char quote = 0;
        for (size_t k = 0; k < text.size(); k++) {
            const char c = text[k];
            if (quote != 0) {
                if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') quote = c;
            else if (c == '(' || c == '[' || c == '<') level++;
            else if ((c == ')' || c == ']' || c == '>') && level > 0) level--;
            else if (c == '=' && level == 0 && k > 0 && text[k - 1] == ' ' && k + 1 < text.size() && text[k + 1] == ' ') {
                text.erase(k);
                break;
            }
        }
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '=')) text.pop_back();
    return trim(text);
}

// The name a type is listed under: `List` for `open List<T>`, `string` for `string?`.
std::string baseName(const std::string& written) {
    std::string t = trim(written);
    if (t.rfind("open ", 0) == 0) t = trim(t.substr(5));
    const size_t cut = t.find_first_of("<?");
    if (cut != std::string::npos) t = t.substr(0, cut);
    return t;
}

bool hidden(const std::string& name, uint8_t access) {
    return name.rfind("__", 0) == 0 || access == kAccessWordPrivate || access == kAccessWordInternal;
}

struct Entry {
    int line = 0;           // where it is written
    std::string anchor;     // `List.add`, `Duration`, `print`
    std::string owner;      // the type it belongs to, or empty for a function or value of its own
    std::string signature;
    std::string comment;    // as HTML
    bool missing = false;   // nothing written about it
    int depth = 0;          // 0 at the top of the file, 1 a member, and so on
    std::string kind;       // "class", "fun", "val", "prose", "heading", "chapter"
};

class Documenter {
public:
    std::vector<std::string> lines;
    std::vector<Entry> entries;
    std::map<std::string, int> anchors;  // how often each has been used, for overloads

    std::string uniqueAnchor(const std::string& name) {
        const int n = ++anchors[name];
        return n == 1 ? name : name + "-" + std::to_string(n);
    }

    // The comment right above line `line`, the lines in order.
    std::vector<std::string> commentAbove(int line) {
        std::vector<std::string> texts;
        for (int at = line - 2; at >= 0 && isComment(lines[static_cast<size_t>(at)]); at--) texts.push_back(commentText(lines[static_cast<size_t>(at)]));
        std::reverse(texts.begin(), texts.end());
        return texts;
    }

    void add(int line, const std::string& name, const std::string& owner, int depth, const std::string& kind) {
        Entry e;
        e.line = line;
        e.anchor = uniqueAnchor(owner.empty() ? name : owner + "." + name);
        e.owner = owner;
        e.signature = declarationText(lines, line);
        std::vector<std::string> texts = commentAbove(line);
        // `// ---- Workers ----` written straight above the declaration, with no blank line:
        // the title still opens a part of the file (or `-- Title --` a group).
        if (!texts.empty()) {
            const std::string first = trim(texts[0]);
            for (const char* mark : {"----", "--"}) {
                const std::string m = mark;
                if (first.size() > 2 * m.size() && first.rfind(m, 0) == 0 && first.compare(first.size() - m.size(), m.size(), m) == 0) {
                    Entry title;
                    title.line = line - static_cast<int>(texts.size());
                    title.kind = m == "----" ? "chapter" : "heading";
                    title.signature = trim(first.substr(m.size(), first.size() - 2 * m.size()));
                    entries.push_back(std::move(title));
                    texts.erase(texts.begin());
                    break;
                }
            }
        }
        e.comment = commentHtml(texts);
        e.missing = texts.empty();
        e.depth = depth;
        e.kind = kind;
        entries.push_back(std::move(e));
    }

    void cls(const ClassStmt& c, const std::string& owner, int depth) {
        if (hidden(c.name, c.access)) return;
        const std::string path = owner.empty() ? c.name : owner + "." + c.name;
        add(c.line, c.name, owner, depth, "class");
        for (const auto& f : c.fields) {
            if (static_cast<size_t>(&f - &c.fields[0]) < c.headerFieldCount) continue;  // in the class's own line
            if (!hidden(f->name, f->access)) add(f->line, f->name, path, depth + 1, "val");
        }
        for (const auto& f : c.staticFields)
            if (!hidden(f->name, f->access)) add(f->line, f->name, path, depth + 1, "val");
        for (const auto& m : c.methods)
            if (!m->generated && !hidden(m->name, m->access) && m->name != "init") add(m->line, m->name, path, depth + 1, "fun");
        for (const auto& n : c.nested) cls(*n, path, depth + 1);
    }

    void statement(const Stmt& s) {
        if (s.visibility != 0) return;  // internal or private, at the top of the file
        switch (s.kind) {
            case StmtKind::Function: {
                const auto& f = static_cast<const FunctionStmt&>(s);
                if (hidden(f.fn->name, f.fn->access)) return;
                add(f.line, f.fn->name, f.receiver.empty() ? std::string() : baseName(f.receiver), 0, "fun");
                break;
            }
            case StmtKind::Var: {
                const auto& v = static_cast<const VarStmt&>(s);
                if (v.name.empty() || hidden(v.name, v.access)) return;
                add(v.line, v.name, v.receiverType.present() ? baseName(v.receiverType.name) : std::string(), 0, "val");
                break;
            }
            case StmtKind::Class:
                cls(static_cast<const ClassStmt&>(s), "", 0);
                break;
            case StmtKind::TypeAlias: {
                const auto& t = static_cast<const TypeAliasStmt&>(s);
                add(t.line, t.name, "", 0, "class");
                break;
            }
            default:
                break;
        }
    }

    // Comments that stand on their own at the top level: those not right above a declaration.
    void freeComments() {
        for (size_t at = 0; at < lines.size();) {
            if (!isComment(lines[at]) || (!lines[at].empty() && (lines[at][0] == ' ' || lines[at][0] == '\t'))) {
                at++;
                continue;
            }
            const size_t from = at;
            std::vector<std::string> texts;
            while (at < lines.size() && isComment(lines[at])) texts.push_back(commentText(lines[at++]));
            if (at < lines.size() && !isBlank(lines[at])) continue;  // a declaration's own
            if (trim(texts[0]).rfind("Inside Astra:", 0) == 0) continue;  // a note for the code's workers
            // `---- Title ----` opens a part of the file, `-- Title --` a group in it; what
            // follows the title is prose.
            std::string first = trim(texts[0]);
            auto title = [&](const std::string& mark) {
                if (first.size() > 2 * mark.size() && first.rfind(mark, 0) == 0 && first.compare(first.size() - mark.size(), mark.size(), mark) == 0)
                    return trim(first.substr(mark.size(), first.size() - 2 * mark.size()));
                return std::string();
            };
            Entry e;
            e.line = static_cast<int>(from + 1);
            e.kind = "prose";
            if (std::string t = title("----"); !t.empty()) {
                e.kind = "chapter";
                e.signature = t;
                texts.erase(texts.begin());
            } else if (std::string t2 = title("--"); !t2.empty()) {
                e.kind = "heading";
                e.signature = t2;
                texts.erase(texts.begin());
            }
            e.comment = commentHtml(texts);
            entries.push_back(std::move(e));
        }
    }
};

struct FileDoc {
    std::string path, name;
    std::vector<Entry> entries;
};

std::string slug(const std::string& s) {
    std::string out;
    for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '-');
    return out;
}

const char* const kStyle = R"(<style>
.api-ref { --api-muted: var(--muted, #5b6270); --api-line: var(--line, #d8dbe1); --api-code: var(--code-bg, #f3f4f6); --api-missing: var(--warn, #b45309); }
.api-ref .file { margin-top: 3rem; }
.api-ref .entry { border-top: 1px solid var(--api-line); padding: .55rem 0 .3rem; }
.api-ref .entry.member { margin-left: 1.5rem; }
.api-ref .entry.member2 { margin-left: 3rem; }
.api-ref .sig { font-family: ui-monospace, Consolas, monospace; font-size: .92rem; background: var(--api-code); padding: .2rem .45rem; border-radius: 4px; display: inline-block; overflow-wrap: anywhere; }
.api-ref .entry p { margin: .35rem 0 .2rem; }
.api-ref .missing { color: var(--api-missing); font-style: italic; }
.api-ref .anchor { color: var(--api-muted); text-decoration: none; margin-left: .4rem; font-size: .85rem; }
.api-ref .types { columns: 14rem; }
.api-ref .types li { break-inside: avoid; }
</style>
)";

std::string render(const std::vector<FileDoc>& files, int& missing, int& total) {
    std::ostringstream out;
    out << kStyle << "<div class=\"api-ref\">\n";
    // Every type's members, wherever each is declared.
    std::map<std::string, std::vector<const Entry*>> byType;
    for (const FileDoc& f : files)
        for (const Entry& e : f.entries) {
            if (e.kind == "class") byType[e.owner.empty() ? e.anchor : e.anchor].push_back(&e);
            else if (!e.owner.empty()) byType[e.owner].push_back(&e);
        }
    out << "<h2 id=\"api-files\">The standard library, file by file</h2>\n<ul>\n";
    for (const FileDoc& f : files) out << "<li><a href=\"#file-" << slug(f.name) << "\">" << escape(f.name) << "</a></li>\n";
    out << "</ul>\n<h2 id=\"api-types\">Types and what they have</h2>\n<ul class=\"types\">\n";
    for (const auto& [type, list] : byType) {
        bool own = false;
        for (const Entry* e : list) own = own || (e->kind == "class" && e->anchor == type);
        out << "<li><a href=\"#" << escape(own ? type : list.front()->anchor) << "\"><code>" << escape(type) << "</code></a> (" << list.size() << ")</li>\n";
    }
    out << "</ul>\n";
    for (const FileDoc& f : files) {
        out << "<section class=\"file\" id=\"file-" << slug(f.name) << "\">\n<h2>" << escape(f.name) << "</h2>\n";
        for (const Entry& e : f.entries) {
            if (e.kind == "chapter") {
                out << "<h3>" << inlineText(e.signature) << "</h3>\n" << e.comment;
                continue;
            }
            if (e.kind == "heading") {
                out << "<h4>" << inlineText(e.signature) << "</h4>\n" << e.comment;
                continue;
            }
            if (e.kind == "prose") {
                out << e.comment;
                continue;
            }
            total++;
            const char* depth = e.depth == 0 ? "" : e.depth == 1 ? " member" : " member member2";
            out << "<div class=\"entry" << depth << "\" id=\"" << escape(e.anchor) << "\"><code class=\"sig\">" << escape(e.signature)
                << "</code><a class=\"anchor\" href=\"#" << escape(e.anchor) << "\">#</a>\n";
            if (e.missing) {
                missing++;
                out << "<p class=\"missing\">Not described yet.</p>\n";
            } else {
                out << e.comment;
            }
            out << "</div>\n";
        }
        out << "</section>\n";
    }
    out << "</div>\n";
    return out.str();
}

}  // namespace

int docCommand(const std::vector<std::string>& args) {
    std::vector<std::string> paths;
    std::string output, templatePath, title = "Astra reference";
    bool listMissing = false;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "-o" && i + 1 < args.size()) output = args[++i];
        else if (args[i] == "--template" && i + 1 < args.size()) templatePath = args[++i];
        else if (args[i] == "--title" && i + 1 < args.size()) title = args[++i];
        else if (args[i] == "--missing") listMissing = true;
        else if (!args[i].empty() && args[i][0] == '-') {
            std::cerr << "astra: usage: astra doc <file>... [-o page.html] [--template page.html] [--title text]\n";
            return 64;
        } else {
            paths.push_back(args[i]);
        }
    }
    if (paths.empty()) {
        std::cerr << "astra: usage: astra doc <file>... [-o page.html] [--template page.html] [--title text]\n";
        return 64;
    }
    std::vector<FileDoc> files;
    for (const std::string& path : paths) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::cerr << "astra: cannot read " << path << "\n";
            return 66;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string source = buffer.str();
        Documenter d;
        std::istringstream split(source);
        for (std::string line; std::getline(split, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            d.lines.push_back(line);
        }
        std::vector<StmtPtr> statements;
        try {
            Lexer lexer(source, path);
            std::vector<Token> tokens = lexer.scan();
            Parser parser(tokens, path);
            statements = parser.parseProgram();
        } catch (const CompileError& e) {
            std::cerr << e.file << ":" << e.line << ":" << e.col << ": error: " << e.what() << "\n";
            return 65;
        }
        for (const StmtPtr& s : statements) d.statement(*s);
        d.freeComments();
        std::stable_sort(d.entries.begin(), d.entries.end(), [](const Entry& a, const Entry& b) { return a.line < b.line; });
        FileDoc f;
        f.path = path;
        const size_t slash = path.find_last_of("/\\");
        f.name = slash == std::string::npos ? path : path.substr(slash + 1);
        f.entries = std::move(d.entries);
        files.push_back(std::move(f));
    }
    if (listMissing) {
        int count = 0;
        for (const FileDoc& f : files)
            for (const Entry& e : f.entries)
                if (e.missing && e.kind != "prose" && e.kind != "heading" && e.kind != "chapter") {
                    std::cout << f.path << ":" << e.line << " " << e.anchor << "\n";
                    count++;
                }
        std::cerr << count << " not described\n";
        return 0;
    }
    int missing = 0, total = 0;
    const std::string reference = render(files, missing, total);
    std::string page;
    if (!templatePath.empty()) {
        std::ifstream in(templatePath, std::ios::binary);
        if (!in) {
            std::cerr << "astra: cannot read " << templatePath << "\n";
            return 66;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        page = buffer.str();
        const std::string marker = "<!-- astra doc -->";
        const size_t at = page.find(marker);
        if (at == std::string::npos) {
            std::cerr << "astra: " << templatePath << " has no " << marker << " to put the reference at\n";
            return 65;
        }
        page.replace(at, marker.size(), reference);
    } else {
        page = "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" +
               escape(title) + "</title>\n<style>body{font-family:system-ui,sans-serif;max-width:60rem;margin:2rem auto;padding:0 1rem;line-height:1.5}</style></head><body>\n<h1>" +
               escape(title) + "</h1>\n" + reference + "</body></html>\n";
    }
    if (output.empty()) {
        std::cout << page;
    } else {
        std::ofstream out(output, std::ios::binary);
        out << page;
        if (!out) {
            std::cerr << "astra: cannot write " << output << "\n";
            return 73;
        }
    }
    std::cerr << total << " declarations, " << (total - missing) << " described, " << missing << " not yet\n";
    return 0;
}

}  // namespace astra
