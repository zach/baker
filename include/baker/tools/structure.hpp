#pragma once

// What a text holds, for tools, even when it does not parse: an editor while a person types,
// a formatter or a highlighter. Part of baker::tools.

#include <baker/parse.hpp>

namespace baker::tools {

enum class TextTokenKind : uint8_t {
    Open,   // ( { [
    Close,  // ) } ]
    Int,
    Float,
    String,  // "..."
    Symbol,
    QuotedSymbol,  // '...'
    Variable,      // $name
    Directive,     // #name
    Macro,         // a symbol this text defines as a macro, [name ...] at the top level
};
struct TextToken {
    TextTokenKind kind{};
    char bracket = 0;  // Open, Close
    int32_t line = 0;
    uint32_t begin = 0, end = 0;  // bytes
};
struct TextBracket {
    char open = 0;
    int32_t line = 0;
    uint32_t begin = 0;
    uint32_t end = 0;     // past its close; the text's end when it is not closed
    int32_t parent = -1;  // the bracket it is in, as an index; -1 at the top level
    int32_t tag = -1;     // its first token, as an index, when that is a symbol; else -1
    bool closed = false;
};
struct TextComment {
    int32_t line = 0;
    uint32_t begin = 0, end = 0;
    bool block = false;  // /* */, or ; to the line's end
};
struct TextStructure {
    List<TextToken> tokens;
    List<TextBracket> brackets;  // in the order they open
    List<TextComment> comments;
    List<Error> errors;  // located as the parser locates them; the read goes on past each
};

// Reads `text` as the parser lexes it and gives back what is written there, for an editor
// while a person types, a formatter or a highlighter: every token, bracket and comment,
// with byte spans. Nothing is refused: an unclosed bracket runs to the end of the text, an
// unterminated string or comment ends there, a stray close is left out, and each is an
// error beside the rest. No macro is expanded and no include followed. Linear in the
// text; the lists are all it allocates. `file` names the errors' file.
[[nodiscard]] TextStructure read_structure(std::string_view text, Symbol file = {});

}  // namespace baker::tools
