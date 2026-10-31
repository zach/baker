// Spells metadata with the sigil: (range 0 10) after a value becomes (@range 0 10).
//
//   baker_migrate_metadata data/default.dta                     print the rewritten text
//   baker_migrate_metadata --write $(git ls-files '*.dta')      rewrite in place
//   baker_migrate_metadata --report $(git ls-files '*.dta')     what would change, for review
//   baker_migrate_metadata --write --cpp tests/data/test_config.cpp   DTA in C++ literals
//   baker_migrate_metadata --write --md docs/language.md              DTA in Markdown
//
// The at-metadata rule: a tagged child whose tag begins
// with '@' is metadata, never a value and never a field. The text is read by the parser's
// own lexer, tolerantly (baker::tools::read_structure), as tokens and brackets by byte span:
// nothing is parsed, expanded or reformatted, and the only change is an '@' before a tag, so
// comments, layout and macros stay as written. Three rewrites:
//
// 1. In every tagged array with at least one untagged value, the old rule names the metadata:
//    the trailing run of children tagged range, step, desc, note, kind, group or one_of (a
//    tweak's without_metadata). Each gets the sigil: (hit_pause 6 (@range 0 30)).
// 2. Inside a top-level (shapes ...), the words shape.cpp reads as metadata: a shape's own
//    (at ...), (keyed) and (rule "message" {expr} (warn)), whose (warn) gets it too, and its
//    (desc "...") and (note "..."), told from a field named desc or note by the string where
//    a field's type goes; a field's children after its type tagged range, step, kind, desc,
//    note, optional or one_of. (list T), (key_of TABLE) and * are types and a wildcard.
// 3. In {func name (params) ...} and {override ...}: the (status ...) and (doc ...) forms
//    between the parameters and the body.
//
// What it will not decide, it reports: a tagged array with no untagged value whose last
// child is tagged with a metadata word, (limits (min 0) (step 0.5)), is the old rule's
// ambiguity; it reads as all value today and under the rule, so nothing is renamed and a
// person decides.
// In --md, a top-level form tagged with a metadata word, `(range 0 10)` in prose, is
// reported too: it names metadata without a value, so no rule can tell.
//
// --cpp reads each run of adjacent C++ string literals, as the compiler joins them, as one
// DTA text: escapes decoded, raw strings as written, comments and character literals
// skipped. --md reads ```dta and unmarked fenced blocks and inline code spans as DTA, ```cpp
// blocks as C++, and skips blocks in other languages. Every byte keeps its offset in the
// file, so a rename lands in the right literal; read the diff all the same. A second run
// changes nothing: a tag that has its sigil is no metadata word.

#include <baker/tools/structure.hpp>

#include "migrate_runs.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace baker::migrate;
using baker::tools::TextBracket;
using baker::tools::TextToken;
using baker::tools::TextTokenKind;

template <size_t N>
using Words = std::array<std::string_view, N>;

// The old rule: the trailing run a tweak left out of its value (tweak.cpp, without_metadata).
constexpr Words<7> kMeta = {"range", "step", "desc", "note", "kind", "group", "one_of"};
// What shape.cpp's read_field reads after a field's type.
constexpr Words<7> kField = {"range", "step", "kind", "desc", "note", "optional", "one_of"};
// A shape's own words (shape.cpp's Set::add), and a rule's (warn).
constexpr Words<3> kShape = {"at", "keyed", "rule"};
constexpr Words<2> kShapeText = {"desc", "note"};  // (desc "..."): the shape's own
constexpr Words<1> kWarn = {"warn"};
constexpr Words<2> kFunc = {"status", "doc"};
// In Markdown, a top-level form tagged with one of these is listed for a person.
constexpr Words<14> kProse = {"range",    "step", "desc",  "note", "kind", "group",  "one_of",
                              "optional", "at",   "keyed", "rule", "warn", "status", "doc"};

// `word` as it is spelled in `words` (static, so a rename can keep it), or empty.
template <size_t N>
std::string_view find(const Words<N>& words, std::string_view word) {
    for (const std::string_view w : words)
        if (w == word) return w;
    return {};
}

// A text's brackets, each with its direct children: a child >= 0 is a token's index, ~b the
// bracket b. A close of the wrong kind closes the innermost and an unclosed bracket runs to
// the end, as read_structure reads them.
struct Tree {
    std::string_view text;
    baker::tools::TextStructure s;
    std::vector<std::vector<int32_t>> kids;  // by bracket

    explicit Tree(std::string_view t)
        : text(t), s(baker::tools::read_structure(t)), kids(s.brackets.size()) {
        std::vector<int32_t> open;
        int32_t next = 0;
        for (size_t i = 0; i < s.tokens.size(); ++i) {
            const TextTokenKind kind = s.tokens[i].kind;
            if (kind == TextTokenKind::Open) {
                if (!open.empty()) kids[static_cast<size_t>(open.back())].push_back(~next);
                open.push_back(next++);
            } else if (kind == TextTokenKind::Close) {
                if (!open.empty()) open.pop_back();
            } else if (!open.empty()) {
                kids[static_cast<size_t>(open.back())].push_back(static_cast<int32_t>(i));
            }
        }
    }
    [[nodiscard]] const TextBracket& bracket(int32_t b) const {
        return s.brackets[static_cast<size_t>(b)];
    }
    [[nodiscard]] const std::vector<int32_t>& children(int32_t b) const {
        return kids[static_cast<size_t>(b)];
    }
    [[nodiscard]] std::string_view span(const TextToken& t) const {
        return text.substr(t.begin, t.end - t.begin);
    }
    // The name a `(` array is tagged with, its first child when that is a symbol; none for
    // an untagged array or a child that is a token.
    [[nodiscard]] std::optional<std::string_view> tag(int32_t child) const {
        if (child >= 0) return std::nullopt;
        const TextBracket& b = bracket(~child);
        if (b.open != '(' || b.tag < 0) return std::nullopt;
        const TextToken& t = s.tokens[static_cast<size_t>(b.tag)];
        std::string_view w = span(t);
        if (t.kind == TextTokenKind::QuotedSymbol) {
            w.remove_prefix(1);
            if (!w.empty() && w.back() == '\'') w.remove_suffix(1);  // unterminated: to the end
        }
        return w;
    }
    // The word `child` is tagged with, as spelled in `words`, if it is one of them.
    template <size_t N>
    [[nodiscard]] std::string_view tagged(int32_t child, const Words<N>& words) const {
        const auto t = tag(child);
        return t ? find(words, *t) : std::string_view();
    }
};

struct Rename {
    size_t at;              // where the '@' goes
    std::string_view word;  // the metadata word, from the lists above
};
struct Review {
    size_t at;
    std::string text;
};

// The renames one DTA text needs, by offset in that text, and its review list.
class Migration {
public:
    Migration(std::string_view text, bool prose) : tree_(text), prose_(prose) {}

    void run() {
        const auto n = static_cast<int32_t>(tree_.s.brackets.size());
        done_.assign(static_cast<size_t>(n), false);
        uint32_t shapes_end = 0;  // brackets open in the order written: a shapes' own are next
        for (int32_t b = 0; b < n; ++b) {
            const TextBracket& br = tree_.bracket(b);
            if (br.begin < shapes_end) continue;
            const auto tag = tree_.tag(~b);
            const bool top = br.parent == -1;
            const std::vector<int32_t>& kids = tree_.children(b);
            if (tag && *tag == "shapes" && top) {
                for (size_t i = 1; i < kids.size(); ++i) shape(kids[i]);
                shapes_end = br.end;
                continue;
            }
            if (prose_ && top && !tree_.tagged(~b, kProse).empty()) note(b);
            if (br.open == '{' && kids.size() >= 3 && kids[0] >= 0) {
                const TextToken& head = tree_.s.tokens[static_cast<size_t>(kids[0])];
                const std::string_view word = tree_.span(head);
                if ((head.kind == TextTokenKind::Symbol || head.kind == TextTokenKind::Macro) &&
                    (word == "func" || word == "override")) {
                    for (size_t i = 3; i < kids.size(); ++i) {
                        const std::string_view key = tree_.tagged(kids[i], kFunc);
                        if (key.empty()) break;
                        rename(kids[i], key);
                    }
                }
            }
            if (tag && !done_[static_cast<size_t>(b)]) d218(b);
        }
    }

    std::vector<Rename> renames;
    std::vector<Review> review;

private:
    void rename(int32_t child, std::string_view word) {
        const TextBracket& b = tree_.bracket(~child);
        const TextToken& t = tree_.s.tokens[static_cast<size_t>(b.tag)];
        renames.push_back({t.begin + (t.kind == TextTokenKind::QuotedSymbol ? 1U : 0U), word});
        done_[static_cast<size_t>(~child)] = true;
    }

    // The first 72 characters of bracket `b`, on one line.
    void note(int32_t b) {
        const TextBracket& br = tree_.bracket(b);
        const std::string_view all = tree_.text.substr(br.begin, br.end - br.begin);
        size_t cut = 0, chars = 0;
        while (cut < all.size() && chars < 72) {
            ++cut;
            while (cut < all.size() && (static_cast<unsigned char>(all[cut]) & 0xC0) == 0x80) ++cut;
            ++chars;
        }
        std::string text(all.substr(0, cut));
        std::replace(text.begin(), text.end(), '\n', ' ');
        if (cut < all.size()) text += "...";
        review.push_back({br.begin, std::move(text)});
    }

    static bool is_value(const Tree& tree, int32_t child) { return !tree.tag(child); }

    void d218(int32_t b) {
        const std::vector<int32_t>& kids = tree_.children(b);
        bool has_value = false;
        for (size_t i = 1; i < kids.size(); ++i) has_value = has_value || is_value(tree_, kids[i]);
        size_t first = kids.size();
        while (first > 1 && !tree_.tagged(kids[first - 1], kMeta).empty()) --first;
        if (first == kids.size()) return;
        if (!has_value) {
            note(b);
            return;
        }
        for (size_t i = kids.size(); i-- > first;) rename(kids[i], tree_.tagged(kids[i], kMeta));
    }

    void shape(int32_t decl) {
        if (!tree_.tag(decl)) return;
        const std::vector<int32_t>& children = tree_.children(~decl);
        for (size_t c = 1; c < children.size(); ++c) {
            const int32_t child = children[c];
            if (child >= 0) continue;  // a token: neither a field nor the shape's own word
            const std::vector<int32_t>& kids = tree_.children(~child);
            if (const std::string_view key = tree_.tagged(child, kShape); !key.empty()) {
                rename(child, key);
                if (key == "rule")  // (rule "message" {expr} (warn)): only (warn) may follow
                    for (size_t i = 3; i < kids.size(); ++i)
                        if (const auto w = tree_.tagged(kids[i], kWarn); !w.empty())
                            rename(kids[i], w);
                continue;
            }
            if (const std::string_view key = tree_.tagged(child, kShapeText);
                !key.empty() && kids.size() >= 2 && kids[1] >= 0 &&
                tree_.s.tokens[static_cast<size_t>(kids[1])].kind == TextTokenKind::String) {
                rename(child, key);  // a string where a field's type goes: the shape's own
                continue;
            }
            if (!tree_.tag(child)) continue;
            for (size_t i = 2; i < kids.size(); ++i)  // a field: (name type metadata...)
                if (const auto key = tree_.tagged(kids[i], kField); !key.empty())
                    rename(kids[i], key);
        }
    }

    Tree tree_;
    bool prose_;
    std::vector<bool> done_;  // arrays renamed already, which the old rule does not read again
};

// The renames a whole file needs, by offset in the file, over its runs of DTA.
struct FileMigration {
    std::vector<Rename> renames;
    std::vector<std::pair<size_t, std::string>> review;  // line, text

    FileMigration(std::string_view src, Mode mode) {
        for (const Run& run : runs_of(src, mode)) {
            Migration m(run.text, mode == Mode::Md);
            m.run();
            for (const Rename& r : m.renames) renames.push_back({run.where(r.at), r.word});
            for (Review& r : m.review) {
                const size_t at = run.where(r.at);
                const auto line =
                    static_cast<size_t>(std::count(src.begin(), src.begin() + at, '\n')) + 1;
                review.emplace_back(line, std::move(r.text));
            }
        }
    }

    [[nodiscard]] std::string apply(std::string_view src) const {
        std::vector<size_t> at;
        at.reserve(renames.size());
        for (const Rename& r : renames) at.push_back(r.at);
        std::sort(at.begin(), at.end());
        at.erase(std::unique(at.begin(), at.end()), at.end());
        std::string out;
        out.reserve(src.size() + at.size());
        size_t from = 0;
        for (const size_t a : at) {
            out.append(src.substr(from, a - from));
            out += '@';
            from = a;
        }
        out.append(src.substr(from));
        return out;
    }
};

std::string keys_text(const std::map<std::string_view, size_t>& by_key) {
    std::string out;
    for (const auto& [key, n] : by_key) {
        if (!out.empty()) out += ", ";
        out += std::string(key) + " " + std::to_string(n);
    }
    return out;
}

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
            mode = mode == Mode::Cpp ? Mode::Cpp : Mode::Md;  // --cpp wins, as the script's did
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
    std::map<std::string_view, size_t> total;
    size_t review_count = 0, changed = 0;
    for (const std::string& path : files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return 1;
        }
        const std::string src((std::istreambuf_iterator<char>(in)), {});
        const FileMigration m(src, mode);
        const std::string out = m.apply(src);
        changed += out != src ? 1U : 0U;
        std::map<std::string_view, size_t> by_key;
        for (const Rename& r : m.renames) {
            ++by_key[r.word];
            ++total[r.word];
        }
        review_count += m.review.size();
        if (report) {
            if (!m.renames.empty() || !m.review.empty())
                std::printf(
                    "%s: %zu renames (%s); %+lld bytes\n", path.c_str(), m.renames.size(),
                    keys_text(by_key).c_str(),
                    static_cast<long long>(out.size()) - static_cast<long long>(src.size()));
            for (const auto& [line, text] : m.review)
                std::printf("  review %s:%zu: %s\n", path.c_str(), line, text.c_str());
        } else {
            if (write && out != src) {
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                f.write(out.data(), static_cast<std::streamsize>(out.size()));
                if (!f) {
                    std::fprintf(stderr, "cannot write %s\n", path.c_str());
                    return 1;
                }
            }
            if (!write) std::fwrite(out.data(), 1, out.size(), stdout);
            for (const auto& [line, text] : m.review)
                std::fprintf(stderr, "review %s:%zu: %s\n", path.c_str(), line, text.c_str());
        }
    }
    if (report) {
        size_t n = 0;
        for (const auto& [key, count] : total) n += count;
        std::printf("total: %zu renames in %zu of %zu files (%s); %zu to review\n", n, changed,
                    files.size(), keys_text(total).c_str(), review_count);
    }
    return 0;
}
