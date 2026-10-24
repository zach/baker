// What a text holds, for tools (<baker/tools/structure.hpp>): the parser's own lexer, read
// tolerantly.

#include <baker/tools/structure.hpp>

#include "data/lexer.hpp"

namespace baker::tools {

TextStructure read_structure(std::string_view text, Symbol file) {
    using detail::Tok;
    Vec<Error> errors;
    Vec<detail::LexComment> comments;
    Vec<TextToken> tokens;
    Vec<TextBracket> brackets;
    Vec<int32_t> open;    // the brackets not yet closed, innermost last
    Vec<Symbol> macros;   // the names [name ...] at the top level defines, in order
    int32_t fresh = -1;   // a bracket just opened, whose first token may be its tag
    bool naming = false;  // the next symbol names a macro
    detail::Lexer lexer(text, file);
    lexer.tolerate(errors, comments);
    auto problem = [&](const Str& message, int32_t line, uint32_t at) {
        Error e = make_error(ErrorCode::Syntax, message, file, line);
        e.set_column(lexer.column_at(at));
        errors.push_back(std::move(e));
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
                naming = t.bracket == '[' && parent == -1;
                fresh = static_cast<int32_t>(brackets.size());
                brackets.push_back({.open = t.bracket,
                                    .line = t.line,
                                    .begin = t.begin,
                                    .end = static_cast<uint32_t>(text.size()),
                                    .parent = parent});
                open.push_back(fresh);
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
                open.pop_back();
                tokens.push_back(out);
                continue;
            }
            case Tok::Int: out.kind = TextTokenKind::Int; break;
            case Tok::Float: out.kind = TextTokenKind::Float; break;
            case Tok::String: out.kind = TextTokenKind::String; break;
            case Tok::Var: out.kind = TextTokenKind::Variable; break;
            case Tok::Directive: out.kind = TextTokenKind::Directive; break;
            case Tok::Symbol: {
                out.kind = t.quoted ? TextTokenKind::QuotedSymbol : TextTokenKind::Symbol;
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
