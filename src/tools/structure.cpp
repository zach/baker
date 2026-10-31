// What a text holds, for tools (<baker/tools/structure.hpp>): the parser's own lexer, read
// tolerantly.

#include <baker/tools/structure.hpp>

#include "data/lexer.hpp"
#include "data/text.hpp"

namespace baker::tools {
namespace {

// A brace's slots as they are read, for the infix rule: the parser's test, slot by
// slot, without the tree.
struct Slots {
    uint32_t n = 0;
    int32_t op = -1;            // slot 1, a bare symbol that starts with an operator byte
    std::string_view op_text;   // its text
    int32_t odd_bad = -1;       // the first odd slot past 1 that is not op, by slot
    bool odd_operator = false;  // and whether it is another operator
    bool head_operand = false;  // slot 0 is a number, variable, property or command
    bool operand = false;       // an even slot is
    bool no_infix = false;      // a quoted symbol at an odd slot, or a macro use
};

bool operand_kind(TextTokenKind k) noexcept {
    return k == TextTokenKind::Int || k == TextTokenKind::Float || k == TextTokenKind::Variable;
}

void add_slot(Slots& s, TextTokenKind kind, char open, std::string_view text, int32_t index) {
    const uint32_t k = s.n++;
    const bool sym = kind == TextTokenKind::Symbol && !open;
    if (kind == TextTokenKind::Macro || ((k & 1) && kind == TextTokenKind::QuotedSymbol)) {
        s.no_infix = true;
        return;
    }
    if (!(k & 1)) {
        const bool operand = operand_kind(kind) || open == '{' || open == '[';
        s.operand = s.operand || operand;
        if (k == 0) s.head_operand = operand;
    } else if (k == 1) {
        if (sym && !text.empty() && detail::kOperatorByte[static_cast<unsigned char>(text[0])]) {
            s.op = index;
            s.op_text = text;
        }
    } else if (s.odd_bad < 0 && !(sym && text == s.op_text)) {
        s.odd_bad = static_cast<int32_t>(k);
        s.odd_operator = sym && detail::is_operator_symbol(text);
    }
}

}  // namespace

TextStructure read_structure(std::string_view text, Symbol file) {
    using detail::Tok;
    Vec<Error> errors;
    Vec<detail::LexComment> comments;
    Vec<TextToken> tokens;
    Vec<TextBracket> brackets;
    Vec<int32_t> open;       // the brackets not yet closed, innermost last
    Vec<Slots> slots;        // each open bracket's, parallel to `open`
    Vec<Symbol> macros;      // the names [name ...] at the top level defines, in order
    int32_t fresh = -1;      // a bracket just opened, whose first token may be its tag
    bool naming = false;     // the next symbol names a macro
    int32_t declaring = -1;  // a declaring directive just read, whose form comes next
    detail::Lexer lexer(text, file);
    lexer.tolerate(errors, comments);
    auto problem = [&](const Str& message, int32_t line, uint32_t at) {
        Error e = make_error(ErrorCode::Syntax, message, file, line);
        e.set_column(lexer.column_at(at));
        errors.push_back(std::move(e));
    };
    // The parser's rule at a brace's close, and its errors for malformed infix.
    auto infix = [&](TextBracket& b, const Slots& s, int32_t) {
        if (s.n < 2 || (!(s.n & 1) && !s.head_operand) || s.no_infix || s.op < 0 ||
            !detail::is_operator_symbol(s.op_text) || !s.operand)
            return;
        const Str op(s.op_text);
        if (s.odd_bad >= 0)
            problem(s.odd_operator ? Str("mixed operators in {...}: nest braces to say which "
                                         "applies first")
                                   : "infix {...} needs '" + op + "' between every two operands",
                    b.line, b.begin);
        else if (!(s.n & 1))
            problem("infix {...} ends with '" + op + "': write an operand after it", b.line,
                    b.begin);
        else
            b.infix = s.op;
    };
    for (;;) {
        detail::Token t;
        Error ignored;
        (void)lexer.next(t, ignored);  // tolerant: it goes on past every problem
        if (t.kind == Tok::End) break;
        TextToken out{.bracket = t.bracket, .line = t.line, .begin = t.begin, .end = t.end};
        const auto index = static_cast<int32_t>(tokens.size());
        switch (t.kind) {
            case Tok::Open: {
                out.kind = TextTokenKind::Open;
                const int32_t parent = open.empty() ? -1 : open.back();
                if (!slots.empty())
                    add_slot(slots.back(), TextTokenKind::Open, t.bracket, {}, index);
                naming = t.bracket == '[' && parent == -1;
                fresh = static_cast<int32_t>(brackets.size());
                brackets.push_back({.open = t.bracket,
                                    .line = t.line,
                                    .begin = t.begin,
                                    .end = static_cast<uint32_t>(text.size()),
                                    .parent = parent,
                                    .declares = t.bracket == '(' && parent == -1 ? declaring : -1});
                declaring = -1;
                open.push_back(fresh);
                slots.emplace_back();
                tokens.push_back(out);
                continue;
            }
            case Tok::Close: {
                out.kind = TextTokenKind::Close;
                fresh = -1;
                if (open.empty()) {
                    problem("unexpected " + Str(detail::bracket_name(t.bracket)), t.line, t.begin);
                    continue;  // a stray close is left out
                }
                TextBracket& b = brackets[open.back()];
                const char want = b.open == '(' ? ')' : b.open == '{' ? '}' : ']';
                if (t.bracket != want)
                    problem(Str(detail::bracket_name(t.bracket)) + " closes " +
                                Str(detail::bracket_name(b.open)) + " opened on line " +
                                detail::decimal(b.line),
                            t.line, t.begin);
                b.end = t.end;
                b.closed = t.bracket == want;
                if (b.open == '{' && b.closed) infix(b, slots.back(), t.line);
                open.pop_back();
                slots.pop_back();
                tokens.push_back(out);
                continue;
            }
            case Tok::Int: out.kind = TextTokenKind::Int; break;
            case Tok::Float: out.kind = TextTokenKind::Float; break;
            case Tok::String: out.kind = TextTokenKind::String; break;
            case Tok::Var: out.kind = TextTokenKind::Variable; break;
            case Tok::Directive:
                out.kind = TextTokenKind::Directive;
                // #pack, #origin, #shape at the top level: the form that follows declares.
                if (open.empty() && (t.text == "pack" || t.text == "origin" || t.text == "shape")) {
                    fresh = -1;
                    naming = false;
                    declaring = index;
                    tokens.push_back(out);
                    continue;
                }
                break;
            case Tok::Symbol: {
                out.kind = t.quoted ? TextTokenKind::QuotedSymbol : TextTokenKind::Symbol;
                if (!t.quoted && fresh >= 0 && brackets[fresh].open == '(' &&
                    brackets[fresh].parent < 0 && t.text.size() > 1 && t.text[0] == '/')
                    out.kind = TextTokenKind::PathKey;
                const Symbol name(t.text);
                if (!t.quoted && naming && fresh >= 0) {
                    macros.push_back(name);
                } else if (!t.quoted &&
                           std::find(macros.begin(), macros.end(), name) != macros.end()) {
                    out.kind = TextTokenKind::Macro;
                }
                if (fresh >= 0) brackets[fresh].tag = index;
                break;
            }
            case Tok::End: break;
        }
        fresh = -1;
        naming = false;
        declaring = -1;
        if (!slots.empty())
            add_slot(slots.back(), out.kind, 0, text.substr(out.begin, out.end - out.begin), index);
        tokens.push_back(out);
    }
    for (const int32_t b : open)
        problem("unclosed " + Str(detail::bracket_name(brackets[b].open)), brackets[b].line,
                brackets[b].begin);
    Vec<TextComment> spans;
    spans.reserve(comments.size());
    for (const detail::LexComment& c : comments) spans.push_back({c.line, c.begin, c.end, c.block});
    return {List<TextToken>({tokens.data(), tokens.size()}),
            List<TextBracket>({brackets.data(), brackets.size()}),
            List<TextComment>({spans.data(), spans.size()}),
            List<Error>({errors.data(), errors.size()})};
}

}  // namespace baker::tools
