// Spells a file's declarations as directives: the forms that declared by position.
//
//   baker_migrate_declarations packs/game/core.dta                    print the rewritten text
//   baker_migrate_declarations --write $(git ls-files '*.dta')        rewrite in place
//   baker_migrate_declarations --report $(git ls-files '*.dta')       what would change
//   baker_migrate_declarations --write --cpp tests/shape/test_shape.cpp   DTA in C++ literals
//   baker_migrate_declarations --write --md docs/language.md              DTA in Markdown
//
// Four rewrites, each at a text's top level, read as the parser lexes it, tolerantly
// (baker::tools::read_structure): only the declaring words and brackets move, so comments,
// macros and the forms' insides stay as written.
//
// 1. A first form (pack NAME ...) becomes #pack (NAME ...).
// 2. (origin "source" ...), first or after the pack, becomes #origin ("source" ...).
// 3. A top-level (shapes (a ...) (b ...)) becomes #shape (a ...) and #shape (b ...), each on a
//    line of its own (in C++ literals, beside each other), its lines moved left with it.
// 4. #define NAME (body), its ( on the name's line, at the top level, becomes [NAME body].
//
// A second run changes nothing: none of the four forms is left.

#include <baker/tools/structure.hpp>

#include "migrate_runs.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace baker::migrate;
using baker::tools::TextBracket;
using baker::tools::TextToken;
using baker::tools::TextTokenKind;

// Replaces [at, at + len) of a text with `text`.
struct Edit {
    size_t at = 0, len = 0;
    std::string text;
};

class Migration {
public:
    Migration(std::string_view text, Mode mode)
        : text_(text), s_(baker::tools::read_structure(text)), mode_(mode) {}

    std::vector<Edit> edits;
    size_t packs = 0, origins = 0, shapes = 0, defines = 0;

    void run() {
        // The top level: each item a bracket (~b) or a token, in order.
        std::vector<int32_t> top;
        std::vector<int32_t> open;
        int32_t next = 0;
        for (size_t i = 0; i < s_.tokens.size(); ++i) {
            const TextTokenKind kind = s_.tokens[i].kind;
            if (kind == TextTokenKind::Open) {
                if (open.empty()) top.push_back(~next);
                open.push_back(next++);
            } else if (kind == TextTokenKind::Close) {
                if (!open.empty()) open.pop_back();
            } else if (open.empty()) {
                top.push_back(static_cast<int32_t>(i));
            }
        }
        size_t brackets = 0;  // top-level brackets seen
        bool pack = false;
        for (size_t k = 0; k < top.size(); ++k) {
            const int32_t item = top[k];
            if (item >= 0) {
                define(top, k);
                continue;
            }
            const TextBracket& b = bracket(~item);
            const std::string_view tag = tag_of(b);
            if (brackets == 0 && tag == "pack" && b.open == '(') {
                declare(b, "#pack ");
                pack = true;
                ++packs;
            } else if ((brackets == 0 || (brackets == 1 && pack)) && tag == "origin" &&
                       b.open == '(' && second_is_string(~item)) {
                declare(b, "#origin ");
                ++origins;
            } else if (tag == "shapes" && b.open == '(') {
                shapes_of(~item);
            }
            ++brackets;
        }
    }

private:
    [[nodiscard]] const TextBracket& bracket(int32_t b) const {
        return s_.brackets[static_cast<size_t>(b)];
    }
    [[nodiscard]] std::string_view span(const TextToken& t) const {
        return text_.substr(t.begin, t.end - t.begin);
    }
    [[nodiscard]] std::string_view tag_of(const TextBracket& b) const {
        if (b.tag < 0) return {};
        const TextToken& t = s_.tokens[static_cast<size_t>(b.tag)];
        return t.kind == TextTokenKind::Symbol || t.kind == TextTokenKind::Macro
                   ? span(t)
                   : std::string_view();
    }
    // The tokens after bracket b's open token, to its close: its children as tokens.
    [[nodiscard]] size_t open_token(int32_t b) const {
        int32_t n = -1;
        for (size_t i = 0; i < s_.tokens.size(); ++i)
            if (s_.tokens[i].kind == TextTokenKind::Open && ++n == b) return i;
        return s_.tokens.size();
    }
    [[nodiscard]] bool second_is_string(int32_t b) const {
        const size_t i = open_token(b) + 2;  // open, tag, then the source
        return i < s_.tokens.size() && s_.tokens[i].kind == TextTokenKind::String;
    }
    // The end of the whitespace from `at`.
    [[nodiscard]] size_t past_space(size_t at) const {
        while (at < text_.size() && (text_[at] == ' ' || text_[at] == '\t')) ++at;
        return at;
    }

    // (word NAME ...) -> #word (NAME ...): the directive before the bracket, and the tag and
    // the blanks after it gone.
    void declare(const TextBracket& b, const char* directive) {
        const TextToken& tag = s_.tokens[static_cast<size_t>(b.tag)];
        edits.push_back({b.begin, 0, directive});
        edits.push_back({tag.begin, past_space(tag.end) - tag.begin, ""});
    }

    // (shapes (a ...) (b ...)) -> #shape (a ...) / #shape (b ...).
    void shapes_of(int32_t b) {
        const TextBracket& outer = bracket(b);
        std::vector<int32_t> kids;
        for (size_t i = static_cast<size_t>(b) + 1; i < s_.brackets.size(); ++i)
            if (bracket(static_cast<int32_t>(i)).parent == b)
                kids.push_back(static_cast<int32_t>(i));
        if (kids.empty()) return;
        size_t from = outer.begin;  // what is left of the outer bracket, before the next shape
        for (size_t k = 0; k < kids.size(); ++k) {
            const TextBracket& c = bracket(kids[k]);
            const std::string_view gap = text_.substr(from, c.begin - from);
            const bool blank = k == 0
                                   ? text_.substr(s_.tokens[open_token(b) + 1].end,
                                                  c.begin - s_.tokens[open_token(b) + 1].end)
                                             .find_first_not_of(" \t\r\n") == std::string_view::npos
                                   : gap.find_first_not_of(" \t\r\n") == std::string_view::npos;
            const std::string sep = k == 0 ? "" : mode_ == Mode::Cpp ? " " : "\n";
            if (blank) {
                edits.push_back({from, c.begin - from, sep + "#shape "});
            } else {  // a comment between: kept, with the opening words gone
                if (k == 0) {
                    const TextToken& tag = s_.tokens[open_token(b) + 1];
                    edits.push_back({outer.begin, tag.end - outer.begin, ""});
                }
                edits.push_back({c.begin, 0, "#shape "});
            }
            dedent(c, from);
            from = c.end;
            ++shapes;
        }
        // The outer close, and the blanks before it.
        if (outer.closed) {
            const size_t close = outer.end - 1;
            size_t at = close;
            while (at > from && (text_[at - 1] == ' ' || text_[at - 1] == '\t')) --at;
            edits.push_back({at, outer.end - at, ""});
        }
    }
    // Moves the lines of shape `c` after its first left by its column, so that its fields keep
    // their indent under it. Not in C++ literals, whose lines are the source's.
    void dedent(const TextBracket& c, size_t from) {
        if (mode_ == Mode::Cpp) return;
        const size_t line = text_.rfind('\n', c.begin == 0 ? 0 : c.begin - 1);
        const size_t start = line == std::string_view::npos ? 0 : line + 1;
        if (start < from) return;  // on the outer bracket's line: nothing to move
        const size_t column = c.begin - start;
        for (size_t at = text_.find('\n', c.begin); at != std::string_view::npos && at < c.end;
             at = text_.find('\n', at + 1)) {
            size_t n = 0;
            while (n < column && at + 1 + n < text_.size() && text_[at + 1 + n] == ' ') ++n;
            if (n > 0) edits.push_back({at + 1, n, ""});
        }
    }

    // #define NAME (body) at the top level -> [NAME body].
    void define(const std::vector<int32_t>& top, size_t k) {
        const TextToken& d = s_.tokens[static_cast<size_t>(top[k])];
        if (d.kind != TextTokenKind::Directive || span(d) != "#define" || k + 2 >= top.size())
            return;
        if (top[k + 1] < 0 || top[k + 2] >= 0) return;
        const TextToken& name = s_.tokens[static_cast<size_t>(top[k + 1])];
        const TextBracket& body = bracket(~top[k + 2]);
        if (body.open != '(' || body.line != name.line || !body.closed) return;
        edits.push_back({d.begin, name.begin - d.begin, "["});
        edits.push_back({name.end, past_space(body.begin + 1) - name.end, " "});
        edits.push_back({body.end - 1, 1, "]"});
        ++defines;
    }

    std::string_view text_;
    baker::tools::TextStructure s_;
    Mode mode_;
};

constexpr const char* kUsage = "usage: %s [--write | --report] [--cpp | --md] FILE...\n";

}  // namespace

int main(int argc, char** argv) {
    bool write = false, report = false;
    Mode mode = Mode::Dta;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--write") {
            write = true;
        } else if (arg == "--report") {
            report = true;
        } else if (arg == "--cpp") {
            mode = Mode::Cpp;
        } else if (arg == "--md") {
            mode = mode == Mode::Cpp ? Mode::Cpp : Mode::Md;
        } else if (arg.substr(0, 1) == "-" && arg != "-") {
            std::fprintf(stderr, kUsage, argv[0]);
            return 2;
        } else {
            files.emplace_back(arg);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, kUsage, argv[0]);
        return 2;
    }
    size_t changed = 0, packs = 0, origins = 0, shapes = 0, defines = 0;
    for (const std::string& path : files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return 1;
        }
        const std::string src((std::istreambuf_iterator<char>(in)), {});
        std::vector<Edit> edits;
        size_t p = 0, o = 0, sh = 0, d = 0;
        for (const Run& run : runs_of(src, mode)) {
            Migration m(run.text, mode);
            m.run();
            for (const Edit& e : m.edits) {
                const size_t begin = run.where(e.at);
                const size_t end = e.len == 0 ? begin : run.where(e.at + e.len - 1) + 1;
                edits.push_back({begin, end - begin, e.text});
            }
            p += m.packs;
            o += m.origins;
            sh += m.shapes;
            d += m.defines;
        }
        // Applied from the end, so that each offset holds; an insertion before a deletion at
        // one place.
        std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) {
            return a.at != b.at ? a.at > b.at : a.len > b.len;
        });
        std::string out = src;
        for (const Edit& e : edits) out.replace(e.at, e.len, e.text);
        changed += out != src ? 1U : 0U;
        packs += p;
        origins += o;
        shapes += sh;
        defines += d;
        if (report) {
            if (out != src)
                std::printf("%s: %zu packs, %zu origins, %zu shapes, %zu macros\n", path.c_str(), p,
                            o, sh, d);
        } else if (write) {
            if (out != src) {
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                f.write(out.data(), static_cast<std::streamsize>(out.size()));
                if (!f) {
                    std::fprintf(stderr, "cannot write %s\n", path.c_str());
                    return 1;
                }
            }
        } else {
            std::fwrite(out.data(), 1, out.size(), stdout);
        }
    }
    if (report)
        std::printf("total: %zu packs, %zu origins, %zu shapes, %zu macros in %zu of %zu files\n",
                    packs, origins, shapes, defines, changed, files.size());
    return 0;
}
