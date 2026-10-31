#pragma once
// The DTA inside other files, for the migrators in tools/: runs of text, each byte with its
// offset in the file. A .dta file is one run; a C++ file's runs are its string literals, as
// the compiler joins adjacent ones; a Markdown file's are its DTA blocks and code spans.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace baker::migrate {

struct Run {
    std::string text;
    size_t base = 0;
    std::vector<size_t> at;  // by byte of text; empty when the run is the file from `base`

    [[nodiscard]] size_t where(size_t k) const { return at.empty() ? base + k : at[k]; }
};

inline bool ascii_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
inline int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Each run of adjacent C++ string literals in `src`, joined as the compiler joins them.
// Every branch moves on by at least a byte, so it ends whatever the text holds.
inline void cpp_runs(std::string_view src, size_t base, std::vector<Run>& runs) {
    Run run;
    auto push = [&](char c, size_t i) {
        run.text += c;
        run.at.push_back(base + i);
    };
    auto close = [&] {
        if (!run.text.empty()) runs.push_back(std::move(run));
        run = Run();
    };
    const size_t n = src.size();
    size_t i = 0;
    while (i < n) {
        const char c = src[i];
        if (src.substr(i, 2) == "//") {
            i = std::min(src.find('\n', i), n);
        } else if (src.substr(i, 2) == "/*") {
            const size_t end = src.find("*/", i + 2);
            i = end == std::string_view::npos ? n : end + 2;
        } else if (src.substr(i, 2) == "R\"" &&
                   !(i > 0 && (ascii_alnum(src[i - 1]) || src[i - 1] == '_'))) {
            const size_t open = src.find('(', i + 2);
            if (open == std::string_view::npos) {
                ++i;
                continue;
            }
            const std::string delim = ")" + std::string(src.substr(i + 2, open - i - 2)) + "\"";
            const size_t end = std::min(src.find(delim, open + 1), n);
            for (size_t k = open + 1; k < end; ++k) push(src[k], k);
            i = end + delim.size();
        } else if (c == '"') {
            ++i;
            while (i < n && src[i] != '"' && src[i] != '\n') {
                if (src[i] != '\\' || i + 1 == n) {
                    push(src[i], i);
                    ++i;
                    continue;
                }
                const char e = src[i + 1];
                if (e == 'x') {  // \x41: one character, its code
                    size_t j = i + 2;
                    uint32_t code = 0;
                    for (; j < n && hex_digit(src[j]) >= 0; ++j)
                        code = std::min<uint32_t>(
                            code * 16 + static_cast<uint32_t>(hex_digit(src[j])), 0x110000);
                    if (j == i + 2) {
                        push('x', i);
                    } else if (code < 0x80) {
                        push(static_cast<char>(code), i);
                    } else {  // as UTF-8, every byte at the escape
                        const uint32_t cp = code < 0x110000 ? code : 0xFFFD;
                        if (cp < 0x800) {
                            push(static_cast<char>(0xC0 | (cp >> 6)), i);
                        } else if (cp < 0x10000) {
                            push(static_cast<char>(0xE0 | (cp >> 12)), i);
                            push(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), i);
                        } else {
                            push(static_cast<char>(0xF0 | (cp >> 18)), i);
                            push(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)), i);
                            push(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), i);
                        }
                        push(static_cast<char>(0x80 | (cp & 0x3F)), i);
                    }
                    i = j;
                    continue;
                }
                // \n \t \r \0 decode; any other escaped character stands for itself.
                push(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == '0' ? '\0' : e, i);
                i += 2;
            }
            ++i;  // the closing quote, or the newline that ends an unterminated literal
        } else if (c == '\'') {
            if (i > 0 && ascii_alnum(src[i - 1])) {  // a digit separator: 1'000
                ++i;
                continue;
            }
            size_t j = i + 1;
            while (j < n && src[j] != '\'' && src[j] != '\n') j += src[j] == '\\' ? 2U : 1U;
            i = j + 1;
        } else if (static_cast<unsigned char>(c) <= ' ') {
            ++i;
        } else {  // any other token ends a run
            close();
            ++i;
        }
    }
    close();
}

// The length of the line break at `i`, as Python's str.splitlines counts one; 0 if none.
inline size_t line_break(std::string_view s, size_t i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == '\r') return s.substr(i, 2) == "\r\n" ? 2 : 1;
    if (c == '\n' || c == '\v' || c == '\f' || (c >= 0x1C && c <= 0x1E)) return 1;
    if (s.substr(i, 2) == "\xC2\x85") return 2;
    if (s.substr(i, 3) == "\xE2\x80\xA8" || s.substr(i, 3) == "\xE2\x80\xA9") return 3;
    return 0;
}
inline bool space(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u == ' ' || (u >= '\t' && u <= '\r') || (u >= 0x1C && u <= 0x1F);
}
inline std::string_view lstrip(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    return s;
}
inline std::string_view strip(std::string_view s) {
    s = lstrip(s);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

// ```dta and unmarked fenced blocks and inline code spans as DTA; ```cpp blocks as C++.
inline void md_runs(std::string_view src, std::vector<Run>& runs) {
    std::optional<std::string> lang;  // inside a fence: its language
    size_t start = 0;
    for (size_t offset = 0; offset < src.size();) {
        size_t end = offset;
        while (end < src.size() && line_break(src, end) == 0) ++end;
        if (end < src.size()) end += line_break(src, end);
        const std::string_view line = src.substr(offset, end - offset);
        const bool fence = lstrip(line).substr(0, 3) == "```";
        if (!lang && fence) {
            lang = std::string(strip(lstrip(line).substr(3)));
            for (char& c : *lang)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            start = end;
        } else if (lang && fence) {
            const std::string_view body = src.substr(start, offset - start);
            if (lang->empty() || *lang == "dta")
                runs.push_back({std::string(body), start, {}});
            else if (*lang == "cpp" || *lang == "c++")
                cpp_runs(body, start, runs);
            lang.reset();
        } else if (!lang) {  // `code`: a backquote, then no backquote or newline up to one
            for (size_t p = line.find('`'); p != std::string_view::npos;) {
                size_t q = p + 1;
                while (q < line.size() && line[q] != '`' && line[q] != '\n') ++q;
                if (q < line.size() && line[q] == '`' && q > p + 1) {
                    runs.push_back(
                        {std::string(line.substr(p + 1, q - p - 1)), offset + p + 1, {}});
                    p = line.find('`', q + 1);
                } else {
                    p = line.find('`', p + 1);
                }
            }
        }
        offset = end;
    }
}

enum class Mode : uint8_t { Dta, Cpp, Md };

// The runs of DTA in a file read as `mode`.
inline std::vector<Run> runs_of(std::string_view src, Mode mode) {
    std::vector<Run> runs;
    if (mode == Mode::Cpp)
        cpp_runs(src, 0, runs);
    else if (mode == Mode::Md)
        md_runs(src, runs);
    else
        runs.push_back({std::string(src), 0, {}});
    return runs;
}

}  // namespace baker::migrate
