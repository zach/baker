#pragma once

// Printing trees back to DTA text.
//
// The printer round-trips: parsing what it writes gives a structurally equal tree. It
// preserves nothing else; comments, formatting, and macro structure are gone (edit-mode
// parsing and write-back are how source text is preserved).
//
// A parsed tree always round-trips. A tree built in C++ can hold values DTA text cannot
// spell, which print as near as they can:
//   - a symbol that needs quoting and also contains `'`;
//   - a string holding a backslash followed by `n` or `q`;
//   - a NaN or infinite float (`nan`, `inf`, which read back as symbols);
//   - an object handle (the quoted symbol `'<object 3:17>'`);
//   - a Property node at a root's top level.

#include <baker/array.hpp>
#include <baker/error.hpp>
#include <baker/node.hpp>
#include <baker/text.hpp>

#include <string_view>

namespace baker {

struct WriteOptions {
    int indent = 2;  // spaces per nesting level
    int width = 80;  // arrays that fit within this many columns stay on one line
};

// Writes the compact single-line form of a node: (price 12.75).
void write(const Node& node, TextSink out);
// Writes the compact single-line form of an array, with parentheses.
void write(const Array& array, TextSink out);

// Writes the file form of a root array: each top-level node on its own line,
// pretty-printed, no enclosing parentheses. Reading it back gives a structurally equal
// tree. <baker/std_interop.hpp> has to_string(node) and to_dta(root), returning strings.
void write_dta(const Array& root, TextSink out, const WriteOptions& options = {});

[[nodiscard]] Expected<void> write_file(std::string_view path, const Array& root,
                                        const WriteOptions& options = {});

}  // namespace baker
