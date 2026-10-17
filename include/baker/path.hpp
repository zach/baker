#pragma once

// Path: a parsed route through a tree, for Array::get and (M3) Config::get.
//
//   renderer.clear_color.0          name, name, index
//   menu.'bacon and eggs'.price     a quoted segment may hold any character but '
//   menu/'bacon and eggs'/price     '/' separates too, so a path copied from an error works
//
// A name segment selects the child array tagged with that symbol, and continues with the
// values after the tag. An all-digit segment selects the value at that index (0 is the
// first value after the tag); if that value is an array, the path continues inside it.
// The empty path is the array itself. A key whose name is all digits, or contains '.' or
// '/', must be quoted.
//
// Parsing interns each name. Build a Path once (a static or a member) for lookups in a
// hot loop; the string forms of get() parse on every call. A Path holds its segments
// inline, so building one never allocates; a path deeper than kMaxSegments is invalid.

#include <baker/symbol.hpp>
#include <baker/text.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace baker {

class Path {
public:
    static constexpr size_t kMaxSegments = 16;

    struct Segment {
        Symbol name;         // when !is_index
        uint32_t index = 0;  // when is_index
        bool is_index = false;
        friend bool operator==(const Segment&, const Segment&) noexcept = default;
    };

    Path() = default;
    Path(std::string_view text);                              // NOLINT(google-explicit-constructor)
    Path(const char* text) : Path(std::string_view(text)) {}  // NOLINT

    // False for malformed text (an empty segment, an unterminated quote, an index beyond
    // 32 bits, more than kMaxSegments segments). Lookups along an invalid path fail with
    // ErrorCode::Syntax.
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] const Segment& operator[](size_t i) const noexcept { return segments_[i]; }

    // Writes the first `count` segments, '/'-separated and quoted where needed, as in
    // errors: menu/'bacon and eggs'. An invalid path writes (the start of) the text it was
    // parsed from. <baker/std_interop.hpp> has to_string(path).
    void write(TextSink out, size_t count = SIZE_MAX, char separator = '/') const;

    // Whether `text` parses to this path; never allocates or interns.
    [[nodiscard]] bool matches(std::string_view text) const noexcept;
    friend bool operator==(const Path& a, const Path& b) noexcept {
        if (a.valid_ != b.valid_ || a.size_ != b.size_) return false;
        for (size_t i = 0; i < a.size_; ++i)
            if (!(a.segments_[i] == b.segments_[i])) return false;
        return true;
    }

private:
    static constexpr size_t kBadText = 64;  // how much of an invalid path's text is kept
    bool parse(std::string_view text);

    Segment segments_[kMaxSegments];
    uint32_t size_ = 0;
    uint32_t bad_size_ = 0;
    char bad_text_[kBadText] = {};  // only for an invalid path
    bool valid_ = true;
};

}  // namespace baker
