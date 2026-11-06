// The language's migrations as findings with edits (<baker/tools/migrate.hpp>). The text is
// read by the parser's own lexer, tolerantly (read_structure), as tokens and brackets by byte
// span: nothing is parsed, expanded or reformatted, so comments, layout and macros stay as
// written. Both migrations read the same structure in one call.

#include <baker/tools/migrate.hpp>

#include "common/containers.hpp"

#include <algorithm>
#include <array>
#include <cassert>

namespace baker::tools {

const char* migration_name(Migration migration) noexcept {
    switch (migration) {
        case Migration::Metadata: return "metadata";
        case Migration::Declarations: return "declarations";
    }
    return "";
}

namespace {

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
// In prose, a top-level form tagged with one of these is a finding for a person.
constexpr Words<14> kProse = {"range",    "step", "desc",  "note", "kind", "group",  "one_of",
                              "optional", "at",   "keyed", "rule", "warn", "status", "doc"};

// `word` as it is spelled in `words` (static, so a finding can keep it), or empty.
template <size_t N>
std::string_view find(const Words<N>& words, std::string_view word) {
    for (const std::string_view w : words)
        if (w == word) return w;
    return {};
}

// A finding while it is built: its edits are [first, first + count) of the call's pool.
struct Found {
    Migration migration;
    uint32_t begin, end;
    int32_t line;
    Str message;
    uint32_t first, count;
};

// The text's brackets, each with its direct children: a child >= 0 is a token's index, ~b
// the bracket b. A close of the wrong kind closes the innermost and an unclosed bracket runs
// to the end, as read_structure reads them. `top` holds the top level's items the same way.
struct Tree {
    std::string_view text;
    const TextStructure& s;
    Vec<uint32_t> first;  // by bracket, into `kids`; one past the last bracket at the end
    Vec<int32_t> kids;
    Vec<int32_t> top;

    Tree(std::string_view t, const TextStructure& st) : text(t), s(st) {
        const size_t n = s.brackets.size();
        Vec<uint32_t> count(n + 1, 0);
        Vec<int32_t> open;
        auto each = [&](auto&& add) {
            open.clear();
            int32_t next = 0;
            for (size_t i = 0; i < s.tokens.size(); ++i) {
                const TextTokenKind kind = s.tokens[i].kind;
                if (kind == TextTokenKind::Open) {
                    add(open.empty() ? -1 : open.back(), ~next);
                    open.push_back(next++);
                } else if (kind == TextTokenKind::Close) {
                    if (!open.empty()) open.pop_back();
                } else {
                    add(open.empty() ? -1 : open.back(), static_cast<int32_t>(i));
                }
            }
        };
        each([&](int32_t parent, int32_t) {
            if (parent >= 0) ++count[static_cast<size_t>(parent)];
        });
        first.assign(n + 1, 0);
        for (size_t b = 0; b < n; ++b) first[b + 1] = first[b] + count[b];
        kids.assign(first[n], 0);
        Vec<uint32_t> at(first.begin(), first.end());
        each([&](int32_t parent, int32_t child) {
            if (parent < 0)
                top.push_back(child);
            else
                kids[at[static_cast<size_t>(parent)]++] = child;
        });
    }
    [[nodiscard]] const TextBracket& bracket(int32_t b) const {
        return s.brackets[static_cast<size_t>(b)];
    }
    [[nodiscard]] const TextToken& token(int32_t t) const {
        return s.tokens[static_cast<size_t>(t)];
    }
    [[nodiscard]] std::span<const int32_t> children(int32_t b) const {
        const auto i = static_cast<size_t>(b);
        return {kids.data() + first[i], first[i + 1] - first[i]};
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
        const TextToken& t = token(b.tag);
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

class Migrator {
public:
    Migrator(const Tree& tree, const MigrateOptions& options) : tree_(tree), options_(options) {
        const size_t nl = tree.text.find('\n');
        crlf_ = nl != std::string_view::npos && nl > 0 && tree.text[nl - 1] == '\r';
    }

    Vec<Found> found;
    Vec<TextEdit> edits;

    void metadata() {
        const auto n = static_cast<int32_t>(tree_.s.brackets.size());
        done_.assign(static_cast<size_t>(n), 0);
        uint32_t shapes_end = 0;  // brackets open in the order written: a shapes' own are next
        for (int32_t b = 0; b < n; ++b) {
            const TextBracket& br = tree_.bracket(b);
            if (br.begin < shapes_end) continue;
            const auto tag = tree_.tag(~b);
            const bool top = br.parent == -1;
            const std::span<const int32_t> kids = tree_.children(b);
            if (tag && *tag == "shapes" && top) {
                for (size_t i = 1; i < kids.size(); ++i) shape(kids[i]);
                shapes_end = br.end;
                continue;
            }
            if (top && br.declares >= 0 && tree_.span(tree_.token(br.declares)) == "#shape") {
                shape(~b);  // #shape (NAME ...), as the declarations migration writes it
                shapes_end = br.end;
                continue;
            }
            if (options_.prose && top) {
                if (const std::string_view w = tree_.tagged(~b, kProse); !w.empty())
                    review(b, Str("(") + w + " ...) names metadata without a value; write (@" + w +
                                  " ...) if it is metadata");
            }
            if (br.open == '{' && kids.size() >= 3 && kids[0] >= 0) {
                const TextToken& head = tree_.token(kids[0]);
                const std::string_view word = tree_.span(head);
                if ((head.kind == TextTokenKind::Symbol || head.kind == TextTokenKind::Macro) &&
                    (word == "func" || word == "override")) {
                    for (size_t i = 3; i < kids.size(); ++i) {
                        const std::string_view key = tree_.tagged(kids[i], kFunc);
                        if (key.empty()) break;
                        rename(kids[i], key, "in a {func}");
                    }
                }
            }
            if (tag && !done_[static_cast<size_t>(b)]) trailing(b);
        }
    }

    void declarations() {
        size_t brackets = 0;  // top-level brackets seen
        bool pack = false;
        const Vec<int32_t>& top = tree_.top;
        for (size_t k = 0; k < top.size(); ++k) {
            const int32_t item = top[k];
            if (item >= 0) {
                define(k);
                continue;
            }
            const TextBracket& b = tree_.bracket(~item);
            const std::string_view tag = tag_of(b);
            if (brackets == 0 && tag == "pack" && b.open == '(') {
                declare(b, "#pack ",
                        "(pack ...) is data; a file declares its pack with #pack (NAME ...)");
                pack = true;
            } else if ((brackets == 0 || (brackets == 1 && pack)) && tag == "origin" &&
                       b.open == '(' && second_is_string(~item)) {
                declare(b, "#origin ",
                        "(origin \"...\") is data; a file declares its origin with #origin "
                        "(\"source\" ...)");
            } else if (tag == "shapes" && b.open == '(') {
                shapes_of(~item);
            }
            ++brackets;
        }
    }

private:
    void start(Migration m, uint32_t begin, uint32_t end, int32_t line, Str message) {
        found.push_back(
            {m, begin, end, line, std::move(message), static_cast<uint32_t>(edits.size()), 0});
    }
    void edit(uint32_t begin, uint32_t end, std::string_view text) {
        edits.push_back({begin, end, text});
        ++found.back().count;
    }

    // --- Metadata ---------------------------------------------------------------------------

    void rename(int32_t child, std::string_view word, const char* where) {
        const TextBracket& b = tree_.bracket(~child);
        const TextToken& t = tree_.token(b.tag);
        const uint32_t at = t.begin + (t.kind == TextTokenKind::QuotedSymbol ? 1U : 0U);
        start(Migration::Metadata, t.begin, t.end, t.line,
              Str("(") + word + " ...) " + where + " is metadata; write (@" + word + " ...)");
        edit(at, at, "@");
        const int32_t bracket = ~child;
        done_[static_cast<size_t>(bracket)] = 1;
    }
    // A finding without edits: bracket `b`, for a person to decide.
    void review(int32_t b, Str message) {
        const TextBracket& br = tree_.bracket(b);
        start(Migration::Metadata, br.begin, br.end, br.line, std::move(message));
    }

    // The old rule's trailing run of metadata words after a value.
    void trailing(int32_t b) {
        const std::span<const int32_t> kids = tree_.children(b);
        bool has_value = false;
        for (size_t i = 1; i < kids.size(); ++i) has_value = has_value || !tree_.tag(kids[i]);
        size_t first = kids.size();
        while (first > 1 && !tree_.tagged(kids[first - 1], kMeta).empty()) --first;
        if (first == kids.size()) return;
        if (!has_value) {
            const std::string_view w = tree_.tagged(kids.back(), kMeta);
            review(b, Str("(") + *tree_.tag(~b) + " ...) has no value, so its (" + w +
                          " ...) is a value under either rule; write (@" + w +
                          " ...) if it is metadata");
            return;
        }
        for (size_t i = first; i < kids.size(); ++i)
            rename(kids[i], tree_.tagged(kids[i], kMeta), "after a value");
    }

    // A shape's own words, and each field's after its type.
    void shape(int32_t decl) {
        if (!tree_.tag(decl)) return;
        const std::span<const int32_t> children = tree_.children(~decl);
        for (size_t c = 1; c < children.size(); ++c) {
            const int32_t child = children[c];
            if (child >= 0) continue;  // a token: neither a field nor the shape's own word
            const std::span<const int32_t> kids = tree_.children(~child);
            if (const std::string_view key = tree_.tagged(child, kShape); !key.empty()) {
                rename(child, key, "in a shape");
                if (key == "rule")  // (rule "message" {expr} (warn)): only (warn) may follow
                    for (size_t i = 3; i < kids.size(); ++i)
                        if (const auto w = tree_.tagged(kids[i], kWarn); !w.empty())
                            rename(kids[i], w, "in a rule");
                continue;
            }
            if (const std::string_view key = tree_.tagged(child, kShapeText);
                !key.empty() && kids.size() >= 2 && kids[1] >= 0 &&
                tree_.token(kids[1]).kind == TextTokenKind::String) {
                rename(child, key, "in a shape");  // a string where a field's type goes
                continue;
            }
            if (!tree_.tag(child)) continue;
            for (size_t i = 2; i < kids.size(); ++i)  // a field: (name type metadata...)
                if (const auto key = tree_.tagged(kids[i], kField); !key.empty())
                    rename(kids[i], key, "after a field's type");
        }
    }

    // --- Declarations -----------------------------------------------------------------------

    [[nodiscard]] std::string_view tag_of(const TextBracket& b) const {
        if (b.tag < 0) return {};
        const TextToken& t = tree_.token(b.tag);
        return t.kind == TextTokenKind::Symbol || t.kind == TextTokenKind::Macro
                   ? tree_.span(t)
                   : std::string_view();
    }
    [[nodiscard]] bool second_is_string(int32_t b) const {
        const std::span<const int32_t> kids = tree_.children(b);
        return kids.size() >= 2 && kids[1] >= 0 &&
               tree_.token(kids[1]).kind == TextTokenKind::String;
    }
    // The end of the blanks from `at`.
    [[nodiscard]] uint32_t past_space(uint32_t at) const {
        const std::string_view text = tree_.text;
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
        return at;
    }

    // (word NAME ...) -> #word (NAME ...): the directive before the bracket, and the tag and
    // the blanks after it gone.
    void declare(const TextBracket& b, std::string_view directive, const char* message) {
        const TextToken& tag = tree_.token(b.tag);
        start(Migration::Declarations, b.begin, b.end, b.line, Str(message));
        edit(b.begin, b.begin, directive);
        edit(tag.begin, past_space(tag.end), "");
    }

    // (shapes (a ...) (b ...)) -> #shape (a ...) / #shape (b ...).
    void shapes_of(int32_t b) {
        const TextBracket& outer = tree_.bracket(b);
        const TextToken& tag = tree_.token(outer.tag);
        const std::string_view text = tree_.text;
        const std::span<const int32_t> all = tree_.children(b);
        if (std::none_of(all.begin(), all.end(), [](int32_t c) { return c < 0; })) return;
        start(Migration::Declarations, outer.begin, outer.end, outer.line,
              Str("(shapes ...) is data; a file declares each shape with #shape (NAME ...)"));
        const std::string_view next = options_.fixed_lines ? " #shape "
                                      : crlf_              ? "\r\n#shape "
                                                           : "\n#shape ";
        auto blank = [](std::string_view s) {
            return s.find_first_not_of(" \t\r\n") == std::string_view::npos;
        };
        uint32_t from = outer.begin;  // what is left of the outer bracket, before the next shape
        bool first = true;
        for (const int32_t kid : all) {
            if (kid >= 0) continue;
            const TextBracket& c = tree_.bracket(~kid);
            const bool clear = first ? blank(text.substr(tag.end, c.begin - tag.end))
                                     : blank(text.substr(from, c.begin - from));
            if (clear) {
                edit(from, c.begin, first ? "#shape " : next);
            } else {  // a comment between: kept, with the opening words gone
                if (first) edit(outer.begin, tag.end, "");
                edit(c.begin, c.begin, "#shape ");
            }
            dedent(c, from);
            from = c.end;
            first = false;
        }
        // The outer close, and the blanks before it.
        if (outer.closed) {
            uint32_t at = outer.end - 1;
            while (at > from && (text[at - 1] == ' ' || text[at - 1] == '\t')) --at;
            edit(at, outer.end, "");
        }
    }
    // Moves the lines of shape `c` after its first left by its column, so that its fields keep
    // their indent under it. Not in text whose lines are not its own.
    void dedent(const TextBracket& c, uint32_t from) {
        if (options_.fixed_lines) return;
        const std::string_view text = tree_.text;
        const size_t line = text.rfind('\n', c.begin == 0 ? 0 : c.begin - 1);
        const size_t start = line == std::string_view::npos ? 0 : line + 1;
        if (start < from) return;  // on the outer bracket's line: nothing to move
        const size_t column = c.begin - start;
        for (size_t at = text.find('\n', c.begin); at != std::string_view::npos && at < c.end;
             at = text.find('\n', at + 1)) {
            size_t n = 0;
            while (n < column && at + 1 + n < text.size() && text[at + 1 + n] == ' ') ++n;
            if (n > 0) edit(static_cast<uint32_t>(at + 1), static_cast<uint32_t>(at + 1 + n), "");
        }
    }

    // #define NAME (body) at the top level -> [NAME body].
    void define(size_t k) {
        const Vec<int32_t>& top = tree_.top;
        const TextToken& d = tree_.token(top[k]);
        if (d.kind != TextTokenKind::Directive || tree_.span(d) != "#define" || k + 2 >= top.size())
            return;
        if (top[k + 1] < 0 || top[k + 2] >= 0) return;
        const TextToken& name = tree_.token(top[k + 1]);
        const TextBracket& body = tree_.bracket(~top[k + 2]);
        if (body.open != '(' || body.line != name.line || !body.closed) return;
        start(Migration::Declarations, d.begin, body.end, d.line,
              Str("#define ") + tree_.span(name) + " (...) is the older macro form; write [" +
                  tree_.span(name) + " ...]");
        edit(d.begin, name.begin, "[");
        edit(name.end, past_space(body.begin + 1), " ");
        edit(body.end - 1, body.end, "]");
    }

    const Tree& tree_;
    const MigrateOptions& options_;
    bool crlf_ = false;
    Vec<uint8_t> done_;  // arrays renamed already, which the old rule does not read again
};

bool before(const TextEdit& a, const TextEdit& b) {
    return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
}

void write_edits(std::string_view text, Vec<TextEdit>& edits, TextSink out) {
    std::stable_sort(edits.begin(), edits.end(), before);
    uint32_t from = 0;
    for (const TextEdit& e : edits) {
        assert(e.begin >= from && "edits overlap");
        out(text.substr(from, e.begin - from));
        out(e.text);
        from = e.end;
    }
    out(text.substr(from));
}

}  // namespace

Migrated migrate_text(std::string_view text, TextSink out, const MigrateOptions& options) {
    const TextStructure s = read_structure(text, options.file);
    const Tree tree(text, s);
    Migrator m(tree, options);
    auto wants = [&](Migration g) { return (options.migrations >> static_cast<int>(g)) & 1U; };
    if (wants(Migration::Metadata)) m.metadata();
    if (wants(Migration::Declarations)) m.declarations();

    Vec<uint32_t> order(m.found.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](uint32_t a, uint32_t b) { return m.found[a].begin < m.found[b].begin; });
    Vec<MigrationFinding> findings;
    findings.reserve(order.size());
    for (const uint32_t i : order) {
        const Found& f = m.found[i];
        TextEdit* e = m.edits.data() + f.first;
        std::stable_sort(e, e + f.count, before);
        findings.push_back({f.migration, f.begin, f.end,
                            make_error(ErrorCode::Syntax, f.message.view(), options.file, f.line),
                            List<TextEdit>({e, f.count})});
    }
    write_edits(text, m.edits, out);
    return {List<MigrationFinding>({findings.data(), findings.size()}), s.errors};
}

void apply_findings(std::string_view text, std::span<const MigrationFinding> findings,
                    TextSink out) {
    Vec<TextEdit> edits;
    for (const MigrationFinding& f : findings)
        for (const TextEdit& e : f.edits) edits.push_back(e);
    write_edits(text, edits, out);
}

}  // namespace baker::tools
