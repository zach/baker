// Laying out text as Baker's writer lays out a tree, keeping its comments
// (<baker/tools/format.hpp>). Only the gaps between tokens and comments change: each piece of
// the text gets a new gap before it, a number of newlines and then of spaces.

#include <baker/tools/format.hpp>

#include "common/containers.hpp"

#include <algorithm>

namespace baker::tools {

namespace {

constexpr uint32_t kNone = UINT32_MAX;
constexpr size_t kWide = SIZE_MAX / 4;  // the width of what never goes on one line

enum class Kind : uint8_t { Atom, Open, Close, LineComment, BlockComment, Directive };

// A token or comment of the text, in order.
struct Piece {
    Kind kind;
    char bracket;
    int32_t line;
    uint32_t begin, end;
};

// What goes before a piece: `newlines` then `spaces`; `keep`, the old gap as written.
struct Gap {
    uint32_t newlines = 0, spaces = 0;
    bool keep = false;
};

// A form, an atom, a comment, or a directive with its arguments (as `kids`).
struct Item {
    uint32_t piece = 0;
    uint32_t close = kNone;  // a form's close
    Kind kind = Kind::Atom;
    Vec<uint32_t> kids;
    size_t flat = 0;  // its width on one line; kWide if it cannot be
};

struct Formatter {
    std::string_view text;
    const WriteOptions& layout;
    Vec<Piece> pieces;
    Vec<Gap> gaps;
    Vec<Item> items;
    Vec<uint32_t> line_starts;  // byte offset of each line, from line 1
    Vec<uint32_t> trailing;     // pieces: ; comments kept after code on their line
    size_t step = 0, width = 0;

    [[nodiscard]] std::string_view text_of(const Piece& p) const {
        return text.substr(p.begin, p.end - p.begin);
    }
    [[nodiscard]] uint32_t column(const Piece& p) const {
        return p.begin - line_starts[static_cast<size_t>(p.line) - 1];
    }
    // Newlines in the old gap before piece `i`.
    [[nodiscard]] uint32_t newlines_before(uint32_t i) const {
        const uint32_t from = i ? pieces[i - 1].end : 0;
        return static_cast<uint32_t>(
            std::count(text.begin() + from, text.begin() + pieces[i].begin, '\n'));
    }

    // --- The tree. ---

    [[nodiscard]] std::string_view directive_name(const Piece& p) const {
        const std::string_view t = text_of(p);
        return t.size() > 1 ? t.substr(1) : t;
    }
    uint32_t build(size_t& i) {
        const auto at = static_cast<uint32_t>(items.size());
        items.emplace_back();
        items[at].piece = static_cast<uint32_t>(i);
        items[at].kind = pieces[i].kind;
        const Piece p = pieces[i++];
        if (p.kind == Kind::Open) {
            while (i < pieces.size() && pieces[i].kind != Kind::Close) {
                const uint32_t kid = build(i);
                items[at].kids.push_back(kid);
            }
            items[at].close = static_cast<uint32_t>(i);
            if (i < pieces.size()) ++i;
        } else if (p.kind == Kind::Directive) {
            const std::string_view d = directive_name(p);
            if (d == "remove") {
                items[at].kind = Kind::Atom;  // a value, not a directive
            } else {
                // The arguments that stay on the directive's line.
                size_t args = d == "else" || d == "endif" ? 0 : 1;
                auto argument = [&] {
                    return i < pieces.size() && pieces[i].kind != Kind::Close &&
                           pieces[i].kind != Kind::LineComment &&
                           pieces[i].kind != Kind::BlockComment;
                };
                for (; args > 0 && argument(); --args) {
                    const uint32_t kid = build(i);
                    items[at].kids.push_back(kid);
                }
                // #define N ( ... ): a body opened on the name's line.
                if (d == "define" && i < pieces.size() && pieces[i].kind == Kind::Open &&
                    pieces[i].bracket == '(' && pieces[i].line == pieces[i - 1].line) {
                    const uint32_t kid = build(i);
                    items[at].kids.push_back(kid);
                }
            }
        }
        return at;
    }
    size_t measure(uint32_t at) {
        Item& it = items[at];
        const Piece& p = pieces[it.piece];
        size_t w = 0;
        switch (it.kind) {
            case Kind::Atom:
            case Kind::BlockComment:
                w = text_of(p).find('\n') == std::string_view::npos ? p.end - p.begin : kWide;
                break;
            case Kind::LineComment:
            case Kind::Directive:
                w = kWide;
                for (const uint32_t k : it.kids) (void)measure(k);
                break;
            case Kind::Open: {
                w = 2 + (it.kids.empty() ? 0 : it.kids.size() - 1);
                for (const uint32_t k : it.kids) w = std::min(kWide, w + measure(k));
                if (it.close == kNone) w = kWide;
                break;
            }
            case Kind::Close: break;
        }
        return items[at].flat = w;
    }

    // --- The layout. ---

    void set(uint32_t piece, uint32_t newlines, uint32_t spaces) {
        gaps[piece] = Gap{newlines, spaces, false};
    }
    // Every gap of `at` as on one line.
    void flat(uint32_t at) {
        const Item& it = items[at];
        for (size_t k = 0; k < it.kids.size(); ++k) {
            set(items[it.kids[k]].piece, 0, k == 0 && it.kind == Kind::Open ? 0 : 1);
            flat(it.kids[k]);
        }
        if (it.close != kNone) set(it.close, 0, 0);
    }
    // Lays out `at`, whose first piece's gap its parent sets, at line indent `indent`.
    void place(uint32_t at, size_t indent) {
        const Item& it = items[at];
        if (it.kind == Kind::Directive) {
            for (const uint32_t k : it.kids) {
                set(items[k].piece, 0, 1);
                place(k, indent);
            }
            return;
        }
        if (it.kind != Kind::Open) return;
        if (it.flat <= (width > indent ? width - indent : 0)) {
            flat(at);
            return;
        }
        const size_t inner = indent + step;
        for (size_t k = 0; k < it.kids.size(); ++k) {
            const uint32_t kid = it.kids[k];
            const uint32_t piece = items[kid].piece;
            const uint32_t lines = newlines_before(piece);
            const bool comment =
                items[kid].kind == Kind::LineComment || items[kid].kind == Kind::BlockComment;
            if (comment && lines == 0 && piece > 0) {
                set(piece, 0, 1);  // stays after what it followed
                if (items[kid].kind == Kind::LineComment) trailing.push_back(piece);
            } else if (k == 0) {
                set(piece, 0, 0);
            } else {
                set(piece, lines > 1 ? 2 : 1, static_cast<uint32_t>(inner));
            }
            place(kid, inner);
        }
        align(it, inner);
        if (it.close != kNone) {
            const bool after_comment =
                it.close > 0 && pieces[it.close - 1].kind == Kind::LineComment;
            if (after_comment)
                set(it.close, 1, static_cast<uint32_t>(indent));
            else
                set(it.close, 0, 0);
        }
    }
    // Blocks of one-line sibling forms (consecutive, no blank line or comment between) that
    // were written aligned, kept aligned: if two members' second items were at one column,
    // one of them padded, every member is padded to the block's widest first item. So a
    // longer key added to an aligned table re-aligns it, and an unaligned block stays so.
    void align(const Item& parent, size_t inner) {
        const size_t budget = width > inner ? width - inner : 0;
        auto member = [&](uint32_t kid) {  // a one-line form with a first and a second item
            const Item& c = items[kid];
            return c.kind == Kind::Open && c.kids.size() >= 2 && c.flat <= budget &&
                   c.close != kNone && pieces[c.piece].line == pieces[c.close].line;
        };
        auto second = [&](uint32_t kid) { return items[items[kid].kids[1]].piece; };
        size_t k = 1;
        while (k < parent.kids.size()) {
            if (!member(parent.kids[k])) {
                ++k;
                continue;
            }
            size_t end = k + 1;
            while (end < parent.kids.size() && member(parent.kids[end]) &&
                   newlines_before(items[parent.kids[end]].piece) == 1)
                ++end;
            // Written aligned: two second items at one column, at least one padded.
            bool aligned = false;
            for (size_t a = k; a < end && !aligned; ++a)
                for (size_t b = a + 1; b < end && !aligned; ++b) {
                    const uint32_t x = second(parent.kids[a]), y = second(parent.kids[b]);
                    const bool padded = pieces[x].begin - pieces[x - 1].end > 1 ||
                                        pieces[y].begin - pieces[y - 1].end > 1;
                    aligned = padded && column(pieces[x]) == column(pieces[y]);
                }
            if (aligned) {
                size_t widest = 0;
                for (size_t j = k; j < end; ++j)
                    widest = std::max(widest, items[items[parent.kids[j]].kids[0]].flat);
                bool fits = true;
                for (size_t j = k; j < end; ++j) {
                    const Item& c = items[parent.kids[j]];
                    fits = fits && c.flat + (widest - items[c.kids[0]].flat) <= budget;
                }
                if (fits)
                    for (size_t j = k; j < end; ++j) {
                        const Item& c = items[parent.kids[j]];
                        set(items[c.kids[1]].piece, 0,
                            static_cast<uint32_t>(widest - items[c.kids[0]].flat + 1));
                    }
            }
            k = end;
        }
    }

    // --- Rendering. ---

    // Each piece's column and line in the new text, by its gaps.
    void columns(Vec<uint32_t>& out, Vec<uint32_t>& lines) const {
        out.assign(pieces.size(), 0);
        lines.assign(pieces.size(), 0);
        uint32_t col = 0, line = 0;
        for (size_t i = 0; i < pieces.size(); ++i) {
            const Gap& g = gaps[i];
            if (g.keep) {
                const uint32_t from = i ? pieces[i - 1].end : 0;
                const std::string_view old = text.substr(from, pieces[i].begin - from);
                line += static_cast<uint32_t>(std::count(old.begin(), old.end(), '\n'));
                const size_t nl = old.rfind('\n');
                col = nl == std::string_view::npos ? col + static_cast<uint32_t>(old.size())
                                                   : static_cast<uint32_t>(old.size() - nl - 1);
            } else {
                col = g.newlines ? g.spaces : col + g.spaces;
                line += g.newlines;
            }
            out[i] = col;
            lines[i] = line;
            const std::string_view t = text_of(pieces[i]);
            line += static_cast<uint32_t>(std::count(t.begin(), t.end(), '\n'));
            const size_t nl = t.rfind('\n');
            col = nl == std::string_view::npos ? col + static_cast<uint32_t>(t.size())
                                               : static_cast<uint32_t>(t.size() - nl - 1);
        }
    }
    // Trailing comments written at one column on consecutive lines, kept aligned one space
    // past the run's longest line.
    void align_comments() {
        if (trailing.size() < 2) return;
        std::sort(trailing.begin(), trailing.end());
        Vec<uint32_t> col, out_line;
        columns(col, out_line);
        size_t k = 0;
        while (k < trailing.size()) {
            size_t end = k + 1;
            auto next = [&](size_t a, size_t b) {
                const Piece& x = pieces[trailing[a]];
                const Piece& y = pieces[trailing[b]];
                // Consecutive and at one column as written, and still consecutive.
                return y.line == x.line + 1 && column(y) == column(x) &&
                       out_line[trailing[b]] == out_line[trailing[a]] + 1;
            };
            while (end < trailing.size() && next(end - 1, end)) ++end;
            if (end - k >= 2) {
                uint32_t target = 0;
                for (size_t j = k; j < end; ++j)
                    target = std::max(target, col[trailing[j]] - gaps[trailing[j]].spaces + 1);
                for (size_t j = k; j < end; ++j) {
                    Gap& g = gaps[trailing[j]];
                    g.spaces = target - (col[trailing[j]] - g.spaces);
                }
            }
            k = end;
        }
    }
};

}  // namespace

Formatted format_text(std::string_view text, TextSink out, const FormatOptions& options) {
    TextStructure s = read_structure(text);
    if (!s.errors.empty()) {
        out(text);
        return {{}, s.errors};
    }
    Formatter f{text, options.layout, {}, {}, {}, {}, {}, 0, 0};
    f.step = options.layout.indent > 0 ? static_cast<size_t>(options.layout.indent) : 0;
    f.width = options.layout.width > 0 ? static_cast<size_t>(options.layout.width) : 0;
    // The pieces: tokens and comments, merged in order.
    size_t t = 0, c = 0;
    while (t < s.tokens.size() || c < s.comments.size()) {
        const bool token = c == s.comments.size() ||
                           (t < s.tokens.size() && s.tokens[t].begin < s.comments[c].begin);
        if (token) {
            const TextToken& k = s.tokens[t++];
            const Kind kind = k.kind == TextTokenKind::Open        ? Kind::Open
                              : k.kind == TextTokenKind::Close     ? Kind::Close
                              : k.kind == TextTokenKind::Directive ? Kind::Directive
                                                                   : Kind::Atom;
            f.pieces.push_back({kind, k.bracket, k.line, k.begin, k.end});
        } else {
            const TextComment& m = s.comments[c++];
            // A ; comment ends before a CRLF's \r: the line end is the gap's to write.
            const uint32_t end =
                !m.block && m.end > m.begin && text[m.end - 1] == '\r' ? m.end - 1 : m.end;
            f.pieces.push_back(
                {m.block ? Kind::BlockComment : Kind::LineComment, 0, m.line, m.begin, end});
        }
    }
    f.line_starts.push_back(0);
    for (uint32_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n') f.line_starts.push_back(i + 1);
    f.gaps.assign(f.pieces.size(), Gap{});

    // The top level: each item on its own line at column 0, a blank line kept where written.
    Vec<uint32_t> top;
    for (size_t i = 0; i < f.pieces.size();) top.push_back(f.build(i));
    for (const uint32_t at : top) (void)f.measure(at);
    for (size_t k = 0; k < top.size(); ++k) {
        const Item& it = f.items[top[k]];
        const uint32_t first = it.piece;
        const uint32_t last = it.close != kNone ? it.close
                              : it.kids.empty() ? it.piece
                              : f.items[it.kids.back()].close != kNone
                                  ? f.items[it.kids.back()].close
                                  : f.items[it.kids.back()].piece;
        const bool touched =
            f.pieces[last].end >= options.begin && f.pieces[first].begin <= options.end;
        if (!touched) {
            for (uint32_t p = first; p <= last; ++p) f.gaps[p].keep = true;
            continue;
        }
        const uint32_t lines = f.newlines_before(first);
        const bool comment = it.kind == Kind::LineComment || it.kind == Kind::BlockComment;
        if (k == 0)
            f.set(first, 0, 0);
        else if (comment && lines == 0) {
            f.set(first, 0, 1);
            if (it.kind == Kind::LineComment) f.trailing.push_back(first);
        } else {
            f.set(first, lines > 1 ? 2 : 1, 0);
        }
        f.place(top[k], 0);
    }
    f.align_comments();

    // The new text, and an edit for each gap that differs from the old. A byte order mark
    // stays, and the text's own line end is the one written.
    const size_t first_newline = text.find('\n');
    const bool crlf = first_newline != std::string_view::npos && first_newline > 0 &&
                      text[first_newline - 1] == '\r';
    const std::string_view line_end = crlf ? "\r\n" : "\n";
    auto newlines = [&](Str& to, uint32_t n) {
        for (uint32_t k = 0; k < n; ++k) to += line_end;
    };
    Str result;
    result.reserve(text.size() + text.size() / 8);
    Vec<FormatEdit> edits;
    uint32_t from = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
    result.append(text.substr(0, from));
    for (size_t i = 0; i < f.pieces.size(); ++i) {
        const Piece& p = f.pieces[i];
        const std::string_view old = text.substr(from, p.begin - from);
        const Gap& g = f.gaps[i];
        if (g.keep) {
            result += old;
        } else {
            const size_t mark = result.size();
            newlines(result, g.newlines);
            result.append(g.spaces, ' ');
            if (std::string_view(result).substr(mark) != old)
                edits.push_back({from, p.begin, g.newlines, g.spaces});
        }
        result += f.text_of(p);
        from = p.end;
    }
    // The end: one newline after the last piece, or as it was when that is left alone.
    const std::string_view tail = text.substr(from);
    const bool kept_tail = !f.pieces.empty() && f.gaps.back().keep;
    if (kept_tail) {
        result += tail;
    } else {
        const uint32_t ends = f.pieces.empty() ? 0 : 1;
        const size_t mark = result.size();
        newlines(result, ends);
        if (tail != std::string_view(result).substr(mark))
            edits.push_back({from, static_cast<uint32_t>(text.size()), ends, 0});
    }
    out(result);
    return {List<FormatEdit>({edits.data(), edits.size()}), {}, line_end};
}

}  // namespace baker::tools
