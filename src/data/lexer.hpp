#pragma once

// The parser's lexer, shared by the parser (parse.cpp) and the tools' tolerant read of text
// (read_structure). Private to Baker: not installed.

#include <baker/parse.hpp>

#include "common/containers.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstring>

namespace baker::detail {

// Each file that includes this keeps its own copy, in an anonymous namespace: the lexer
// keeps the internal linkage it had inside parse.cpp, which lets the compiler inline it
// as it did (external linkage cost the parse 1.5% more instructions).
namespace {

// A comment's span, as the tolerant lexer records it.
struct LexComment {
    int32_t line = 0;
    uint32_t begin = 0, end = 0;
    bool block = false;  // /* */, or ; to the line's end
};

enum class Tok : uint8_t { End, Open, Close, Int, Float, String, Symbol, Var, Directive };

struct Token {
    Tok kind = Tok::End;
    char bracket = 0;     // Open, Close
    bool quoted = false;  // Symbol written as 'text'
    int32_t line = 0;
    uint32_t begin = 0, end = 0;  // byte span in the source
    int64_t i = 0;
    double f = 0;
    std::string_view text;  // symbol, var name, directive name, decoded string
};

std::string_view bracket_name(char c) noexcept {
    switch (c) {
        case '(': return "'('";
        case ')': return "')'";
        case '{': return "'{'";
        case '}': return "'}'";
        case '[': return "'['";
        case ']': return "']'";
        default: return "?";
    }
}

class Lexer {
public:
    Lexer(std::string_view src, Symbol file) : src_(src), file_(file) {
        if (src_.size() >= 3 && std::memcmp(src_.data(), "\xEF\xBB\xBF", 3) == 0) pos_ = 3;
    }

    // Produces the next token. On failure fills `err` and returns false.
    bool next(Token& t, Error& err) {
        if (has_peek_) {
            has_peek_ = false;
            t = peeked_;
            return true;
        }
        return lex(t, err);
    }
    bool peek(Token& t, Error& err) {
        if (!has_peek_) {
            if (!lex(peeked_, err)) return false;
            has_peek_ = true;
        }
        t = peeked_;
        return true;
    }

    // Tolerant: each problem goes to `errors` and the lexing goes on (read_structure), and
    // each comment's span to `comments`.
    void tolerate(Vec<Error>& errors, Vec<LexComment>& comments) noexcept {
        soft_ = &errors;
        comments_ = &comments;
    }

private:
    bool fail(Error& err, ErrorCode code, std::string_view msg, int32_t line, size_t at) {
        err = make_error(code, msg, file_, line);
        err.set_column(column_at(at));
        return false;
    }
    // A problem the lexer goes on past when tolerant; false, as fail, when not.
    bool soft(Error& err, ErrorCode code, std::string_view msg, int32_t line, size_t at) {
        if (!soft_) return fail(err, code, msg, line, at);
        Error e;
        (void)fail(e, code, msg, line, at);
        soft_->push_back(std::move(e));
        return true;
    }
    void comment(int32_t line, size_t begin, bool block) {
        if (comments_)
            comments_->push_back(
                {line, static_cast<uint32_t>(begin), static_cast<uint32_t>(pos_), block});
    }

public:
    // The 1-based column of byte `at`, in characters (code points) from its line's start,
    // a byte-order mark aside. On the error path only: nothing tracks it per token.
    [[nodiscard]] int32_t column_at(size_t at) const noexcept {
        at = std::min(at, src_.size());
        const size_t nl = at == 0 ? std::string_view::npos : src_.rfind('\n', at - 1);
        size_t from = nl == std::string_view::npos ? 0 : nl + 1;
        if (from == 0 && src_.size() >= 3 && std::memcmp(src_.data(), "\xEF\xBB\xBF", 3) == 0)
            from = std::min<size_t>(3, at);
        int32_t column = 1;
        for (size_t i = from; i < at; ++i)
            if ((static_cast<unsigned char>(src_[i]) & 0xC0) != 0x80) ++column;
        return column;
    }

private:
    bool lex(Token& t, Error& err) {
        const bool ok = lex_token(t, err);
        t.end = static_cast<uint32_t>(pos_);
        return ok;
    }

    static int32_t count_lines(std::string_view s) noexcept {
        return static_cast<int32_t>(std::count(s.begin(), s.end(), '\n'));
    }

    bool lex_token(Token& t, Error& err) {
        t = Token{};
        const size_t n = src_.size();
        for (;;) {
            if (pos_ >= n) {
                t.kind = Tok::End;
                t.line = line_;
                t.begin = static_cast<uint32_t>(pos_);
                return true;
            }
            const auto c = static_cast<unsigned char>(src_[pos_]);
            if (c <= 0x20) {
                if (c == '\n') ++line_;
                ++pos_;
            } else if (c == ';') {
                const size_t opened = pos_;
                pos_ = std::min(src_.find('\n', pos_), n);  // to the newline, as memchr
                comment(line_, opened, false);
            } else if (c == '/' && pos_ + 1 < n && src_[pos_ + 1] == '*') {
                const int32_t start = line_;
                const size_t opened = pos_;
                pos_ += 2;
                for (;;) {
                    if (pos_ + 1 >= n) {
                        if (!soft(err, ErrorCode::Syntax, "unterminated /* comment", start, opened))
                            return false;
                        pos_ = n;  // the comment runs to the end
                        comment(start, opened, true);
                        break;
                    }
                    if (src_[pos_] == '*' && src_[pos_ + 1] == '/') {
                        pos_ += 2;
                        comment(start, opened, true);
                        break;
                    }
                    if (src_[pos_] == '\n') ++line_;
                    ++pos_;
                }
            } else {
                break;
            }
        }

        t.line = line_;
        t.begin = static_cast<uint32_t>(pos_);
        const char c = src_[pos_];
        switch (c) {
            case '(':
            case '{':
            case '[':
                t.kind = Tok::Open;
                t.bracket = c;
                ++pos_;
                return true;
            case ')':
            case '}':
            case ']':
                t.kind = Tok::Close;
                t.bracket = c;
                ++pos_;
                return true;
            case '"': return lex_string(t, err);
            case '\'': return lex_quoted_symbol(t, err);
            default: return lex_atom(t, err);
        }
    }

    // "..." with two escapes: \n (newline) and \q (double quote). Any other backslash is
    // literal, so Windows paths need no doubling.
    bool lex_string(Token& t, Error& err) {
        const int32_t start_line = line_;
        const size_t begin = ++pos_;
        // Neither escape contains a quote, so the string ends at the first one.
        size_t i = src_.find('"', begin);
        if (i == std::string_view::npos) {
            if (!soft(err, ErrorCode::Syntax, "unterminated string", start_line, begin - 1))
                return false;
            i = src_.size();  // the string runs to the end
        }
        const std::string_view raw = src_.substr(begin, i - begin);
        line_ += count_lines(raw);
        pos_ = std::min(i + 1, src_.size());
        t.kind = Tok::String;
        if (raw.find('\\') == std::string_view::npos) {
            t.text = raw;
            return true;
        }
        buf_.clear();
        for (size_t k = 0; k < raw.size(); ++k) {
            if (raw[k] == '\\' && k + 1 < raw.size() && (raw[k + 1] == 'n' || raw[k + 1] == 'q')) {
                buf_ += raw[k + 1] == 'n' ? '\n' : '"';
                ++k;
            } else {
                buf_ += raw[k];
            }
        }
        t.text = buf_;
        return true;
    }

    bool lex_quoted_symbol(Token& t, Error& err) {
        const int32_t start_line = line_;
        const size_t begin = ++pos_;
        size_t end = src_.find('\'', begin);
        if (end == std::string_view::npos) {
            if (!soft(err, ErrorCode::Syntax, "unterminated 'quoted symbol'", start_line,
                      begin - 1))
                return false;
            end = src_.size();  // runs to the end
        }
        t.kind = Tok::Symbol;
        t.quoted = true;
        t.text = src_.substr(begin, end - begin);
        line_ += count_lines(t.text);
        pos_ = std::min(end + 1, src_.size());
        return true;
    }

    bool lex_atom(Token& t, Error& err) {
        const size_t begin = pos_;
        while (pos_ < src_.size() && !is_atom_end(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        const std::string_view atom = src_.substr(begin, pos_ - begin);
        if (atom[0] == '#' && atom.size() > 1) {  // a lone # is the symbol '#'
            t.kind = Tok::Directive;
            t.text = atom.substr(1);
            return true;
        }
        if (atom[0] == '$') {
            if (atom.size() == 1 &&
                !soft(err, ErrorCode::Syntax, "'$' without a variable name", t.line, begin))
                return false;
            t.kind = Tok::Var;
            t.text = atom.substr(1);
            return true;
        }
        const Number num = classify_number(atom);
        switch (num.kind) {
            case NumberKind::Int:
                t.kind = Tok::Int;
                t.i = num.i;
                return true;
            case NumberKind::Float:
                t.kind = Tok::Float;
                t.f = num.f;
                return true;
            case NumberKind::IntOutOfRange:
                t.kind = Tok::Int;
                return soft(err, ErrorCode::Range,
                            "integer " + Str(atom) + " is out of 64-bit range", t.line, begin);
            case NumberKind::FloatOutOfRange:
                t.kind = Tok::Float;
                return soft(err, ErrorCode::Range, "float " + Str(atom) + " is out of double range",
                            t.line, begin);
            case NumberKind::NotNumber: break;
        }
        t.kind = Tok::Symbol;
        t.text = atom;
        return true;
    }

    std::string_view src_;
    Symbol file_;
    size_t pos_ = 0;
    int32_t line_ = 1;
    Str buf_;  // decoded string with escapes; valid until the next string token
    bool has_peek_ = false;
    Token peeked_;
    Vec<Error>* soft_ = nullptr;           // tolerant: the problems gone past
    Vec<LexComment>* comments_ = nullptr;  // tolerant: the comments' spans
};

}  // namespace
}  // namespace baker::detail
