#pragma once

// SourceMap: the side table of an edit-mode parse.
//
// A gameplay parse keeps only the tree. An edit-mode parse (set LoadSession::source_map)
// also records the source text of every file it reads and, for every array, where it and
// each of its nodes are written: file, byte span, and whether the node was spliced in from
// a macro. Node and Array are unchanged; the table lives beside the tree.
//
// This is what write-back needs. Comments, whitespace, and formatting are the text between
// spans, so replacing one token's bytes keeps everything else exactly as it was, and the
// result is a minimal diff. Values that have no single literal token as their source (a
// macro expansion, an array rebuilt by #merge) are marked or absent, so write-back can
// refuse instead of guessing.

#include <baker/array.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace baker {

struct SourceSpan {
    Symbol file;         // empty: not written in any file (built in C++, or unknown)
    uint32_t begin = 0;  // byte offset of the first character (a quote or bracket included)
    uint32_t end = 0;    // one past the last
    Symbol macro;        // non-empty: spliced in from this macro, so not written here

    [[nodiscard]] bool literal() const noexcept { return !file.empty() && macro.empty(); }
};

class SourceMap {
public:
    SourceMap();
    // Its tables from `alloc`, which must outlive it (a Config passes its own).
    explicit SourceMap(const Allocator& alloc);
    SourceMap(const SourceMap&) = delete;
    SourceMap& operator=(const SourceMap&) = delete;
    ~SourceMap();

    // Where `a`'s nodes are written, or nothing if `a` was not read through this map (or
    // was rebuilt by #merge from arrays that were not).
    [[nodiscard]] std::optional<std::span<const SourceSpan>> spans(const Array& a) const;
    [[nodiscard]] const SourceSpan* span(const Array& a, size_t i) const;
    // Where `a` itself is written, brackets included; for a file's root, the whole file.
    [[nodiscard]] const SourceSpan* extent(const Array& a) const;
    // The text of `file` as it was read, or nothing.
    [[nodiscard]] std::optional<std::string_view> text(Symbol file) const;
    [[nodiscard]] bool empty() const noexcept;

    // Used by the parser and #merge to fill the table.
    void record(const Ref<Array>& a, const SourceSpan& extent, std::span<const SourceSpan> spans);
    void record_text(Symbol file, std::string_view text);

private:
    struct Tables;  // defined in source_map.cpp
    Tables* t_;
};

}  // namespace baker
