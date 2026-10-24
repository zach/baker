#pragma once

// Laying out Baker text as Baker's writer lays out a tree, keeping everything the text says
// that a tree does not: comments, directives, macros as written, number and string
// spellings, and the blank lines between forms. For an editor's Format Document, a CI step,
// a pre-commit hook, and any tool that writes a file. Part of baker::tools.

#include <baker/tools/structure.hpp>
#include <baker/write.hpp>

#include <cstdint>

namespace baker::tools {

struct FormatOptions {
    WriteOptions layout{};  // indent and width; a project's (format ...) gives them
    // Only the top-level forms this byte span touches are laid out, the rest of the text
    // unchanged: an editor's selection, or the forms an edit touched. The default is all.
    uint32_t begin = 0;
    uint32_t end = UINT32_MAX;
};

// One gap of the old text, [begin, end), replaced by `newlines` line ends (the text's own,
// Formatted::line_end) and then `spaces` spaces. Only gaps change, so these turn the old text
// into the new.
struct FormatEdit {
    uint32_t begin = 0, end = 0;
    uint32_t newlines = 0, spaces = 0;
};

struct Formatted {
    List<FormatEdit> edits;  // in order, none overlapping; none if it was laid out already
    List<Error> errors;      // the tolerant read's; with any, the text is unchanged
    // The text's line end, "\r\n" if its first one is, else "\n": what an edit's newlines are.
    std::string_view line_end = "\n";
    [[nodiscard]] bool changed() const noexcept { return !edits.empty(); }
};

// Writes `text` laid out to `out`, and returns the edits that make it.
//  - The writer's layout (write_dta): a form goes on one line if it fits the width at its
//    indent; otherwise its first item stays on the opening line and each later item gets a
//    line of its own, one indent step deeper. A form holding a ; comment or a directive is
//    never put on one line.
//  - Only whitespace changes: every token, comment and directive is kept as written, so the
//    text reads as the same tree, with the same comments in the same order.
//  - A directive keeps its arguments on its line: #include path, #ifdef N, #define N (.
//  - A ; comment that followed code stays on that line; one on its own line stays so. A
//    block comment stays inline or on its own line, as it was.
//  - What was written aligned stays aligned: a run of consecutive one-line sibling forms
//    whose second items were at one column is re-padded to the run's widest first item,
//    and a run of trailing comments on consecutive lines at one column is put one space
//    past the run's longest line. Nothing else is aligned.
//  - One blank line where any was written between two forms; none at the start or the end.
//  - A byte order mark at the start stays, and so does the text's line end, CRLF or LF.
//  - Text that does not parse is written unchanged, with the read's errors.
// Formatting its own output changes nothing. Linear in the text; no file is read.
Formatted format_text(std::string_view text, TextSink out, const FormatOptions& options = {});

}  // namespace baker::tools
