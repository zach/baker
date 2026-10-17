#include <baker/array.hpp>
#include <baker/path.hpp>
#include <baker/write.hpp>

#include "common/containers.hpp"
#include "text.hpp"

#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>

#include <fast_float.h>

// libc++ on Apple ships double std::to_chars only for recent deployment targets, so Apple
// builds use the portable formatter. Define BAKER_FORCE_FORMAT_FALLBACK to use it anywhere.
#if defined(BAKER_FORCE_FORMAT_FALLBACK) || (defined(__APPLE__) && defined(_LIBCPP_VERSION))
#define BAKER_FORMAT_FALLBACK 1
#else
#define BAKER_FORMAT_FALLBACK 0
#endif

namespace baker::detail {
namespace {
bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}
int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Locale-independent parse of an unsigned decimal float. fast_float is used everywhere,
// not just where std::from_chars lacks doubles (libc++ before LLVM 20), so every
// platform reads the same text as the same value.
bool parse_double(std::string_view s, double& out, bool& out_of_range) noexcept {
    const char* end = s.data() + s.size();
    auto [p, ec] = fast_float::from_chars(s.data(), end, out, fast_float::chars_format::general);
    out_of_range = ec == std::errc::result_out_of_range;
    return p == end && (ec == std::errc() || out_of_range);
}
}  // namespace

Number classify_number(std::string_view s) noexcept {
    std::string_view r = s;
    bool neg = false;
    if (!r.empty() && (r[0] == '+' || r[0] == '-')) {
        neg = r[0] == '-';
        r.remove_prefix(1);
    }
    if (r.empty() || (!is_digit(r[0]) && r[0] != '.')) return {};  // every number starts so

    if (r.size() > 2 && r[0] == '0' && (r[1] == 'x' || r[1] == 'X')) {
        uint64_t v = 0;
        bool overflow = false;
        for (char c : r.substr(2)) {
            const int d = hex_value(c);
            if (d < 0) return {};
            if (!overflow) {
                overflow = v > (uint64_t{INT64_MAX} >> 4);
                v = v * 16 + static_cast<uint64_t>(d);
                overflow = overflow || v > uint64_t{INT64_MAX};
            }
        }
        if (overflow) return {NumberKind::IntOutOfRange};
        const auto i = static_cast<int64_t>(v);
        return {NumberKind::Int, neg ? -i : i};
    }

    bool all_digits = true;
    for (char c : r) all_digits = all_digits && is_digit(c);
    if (all_digits) {
        // Accumulate negatively so INT64_MIN is representable.
        int64_t v = 0;
        for (char c : r) {
            const int d = c - '0';
            if (v < (INT64_MIN + d) / 10) return {NumberKind::IntOutOfRange};
            v = v * 10 - d;
        }
        if (!neg) {
            if (v == INT64_MIN) return {NumberKind::IntOutOfRange};
            v = -v;
        }
        return {NumberKind::Int, v};
    }

    if (is_digit(r[0]) || (r[0] == '.' && r.size() > 1 && is_digit(r[1]))) {
        double f = 0;
        bool out_of_range = false;
        if (!parse_double(r, f, out_of_range)) return {};
        if (out_of_range || !std::isfinite(f)) return {NumberKind::FloatOutOfRange};
        return {NumberKind::Float, 0, neg ? -f : f};
    }
    return {};
}

bool is_bare_symbol(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s)
        if (is_atom_end(static_cast<unsigned char>(c))) return false;
    if (s[0] == '\'' || s[0] == '$' || s[0] == '#') return false;
    if (s.size() >= 2 && s[0] == '/' && s[1] == '*') return false;
    return classify_number(s).kind == NumberKind::NotNumber;
}

void append_path_name(Str& out, std::string_view name) {
    const bool bare = is_bare_symbol(name) && name.find_first_of("./") == std::string_view::npos;
    if (!bare) out += '\'';
    out += name;
    if (!bare) out += '\'';
}

namespace {

// `v` printed as "%.*g" at `precision` into `out`, with '.' for the C locale's decimal
// point, which %g follows; its length, or 0 if it did not fit. The two %g searches below
// print through it.
size_t print_g(double v, int precision, char (&out)[64]) noexcept {
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "%.*g", precision, v);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof buf) return 0;
    const char* point = std::localeconv()->decimal_point;
    const std::string_view decimal = point && *point ? point : ".";
    const std::string_view text(buf, static_cast<size_t>(n));
    size_t len = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text.substr(i, decimal.size()) == decimal) {
            out[len++] = '.';
            i += decimal.size() - 1;
        } else {
            out[len++] = text[i];
        }
    }
    return len;
}

}  // namespace

Str format_double_fallback(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    // The shortest %g precision that reads back exactly.
    Str text;
    for (int precision = 1; precision <= 17; ++precision) {
        char buf[64];
        text.assign(std::string_view(buf, print_g(v, precision, buf)));
        std::string_view digits = text;
        const bool neg = !digits.empty() && digits[0] == '-';
        if (neg) digits.remove_prefix(1);
        double back = 0;
        bool out_of_range = false;
        if (parse_double(digits, back, out_of_range) && (neg ? -back : back) == v) break;
    }
    return text;
}

Str format_double(double v) {
#if BAKER_FORMAT_FALLBACK
    return format_double_fallback(v);
#else
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    return Str(buf, r.ptr);
#endif
}

namespace {
bool is_separator(char c) noexcept {
    return c == '/' || c == '\\';
}
bool is_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// The length of "name://", a scheme an engine names a virtual root with ("user://"), or 0.
// A scheme is a letter, then letters, digits, '+', '-' or '.'; at least two characters, so
// a drive ("C://") is a drive.
size_t scheme_length(std::string_view p) noexcept {
    const size_t colon = p.find("://");
    if (colon == std::string_view::npos || colon < 2 || !is_alpha(p[0])) return 0;
    for (size_t i = 1; i < colon; ++i) {
        const char c = p[i];
        if (!is_alpha(c) && !(c >= '0' && c <= '9') && c != '+' && c != '-' && c != '.') return 0;
    }
    return colon + 3;
}

// "user://", "/", "C:/", or "C:" (a drive-relative prefix); 0 if none.
size_t root_length(std::string_view p) noexcept {
    if (const size_t scheme = scheme_length(p)) return scheme;
    if (p.size() >= 2 && is_alpha(p[0]) && p[1] == ':')
        return p.size() >= 3 && is_separator(p[2]) ? 3 : 2;
    return !p.empty() && is_separator(p[0]) ? 1 : 0;
}
}  // namespace

bool is_absolute_path(std::string_view p) noexcept {
    const size_t root = root_length(p);
    return root > 0 && is_separator(p[root - 1]);
}

Str normalize_path(std::string_view p) {
    const size_t root = root_length(p);
    const bool absolute = is_absolute_path(p);
    Vec<std::string_view> parts;
    size_t start = root;
    for (size_t i = root; i <= p.size(); ++i) {
        if (i < p.size() && !is_separator(p[i])) continue;
        const std::string_view part = p.substr(start, i - start);
        start = i + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!absolute)
                parts.push_back(part);
            continue;
        }
        parts.push_back(part);
    }
    Str out(p.substr(0, root));
    for (char& c : out)
        if (c == '\\') c = '/';
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += '/';
        out += parts[i];
    }
    return out.empty() && !p.empty() ? "." : out;
}

std::string_view parent_path(std::string_view p) noexcept {
    const size_t root = root_length(p);
    size_t i = p.size();
    while (i > root && !is_separator(p[i - 1])) --i;
    if (i <= root) return p.substr(0, root);  // the root is its own parent
    return i == 1 ? p.substr(0, 1) : p.substr(0, i - 1);
}

// buf_ is left unset: only the bytes written are read.
CPath::CPath(std::string_view path) {  // NOLINT(cppcoreguidelines-pro-type-member-init)
    if (path.size() < sizeof buf_) {
        std::memcpy(buf_, path.data(), path.size());
        buf_[path.size()] = '\0';
    } else {
        p_ = long_.emplace(path).c_str();
    }
}

Str join_path(std::string_view dir, std::string_view p) {
    if (dir.empty()) return Str(p);
    Str out(dir);
    if (!is_separator(out.back())) out += '/';
    out += p;
    return out;
}

double float_to_decimal_double(float f) noexcept {
    if (!std::isfinite(f) || f == 0) return static_cast<double>(f);  // keeps -0's sign
    // Fast path, for magnitudes where it is exact: round to p significant digits by scaling
    // with an exact power of ten, for p = 1, 2, ...; the first that reads back as `f` is the
    // shortest. n / 10^k is then the double nearest the decimal, as reading its text gives.
    // Checked against the text search below on millions of floats.
    static constexpr double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                        1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                        1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    const double x = f;
    const double ax = std::fabs(x);
    if (ax >= 1e-13 && ax < 1e13) {
        auto pow10 = [](int k) { return k >= 0 ? kPow10[k] : 1 / kPow10[-k]; };
        int e = static_cast<int>(std::floor(std::log10(ax)));
        if (pow10(e) > ax)
            --e;
        else if (pow10(e + 1) <= ax)
            ++e;
        for (int p = 1; p <= 9; ++p) {
            const int k = p - 1 - e;
            if (k > 22 || k < -22) break;
            const double scale = kPow10[k >= 0 ? k : -k];
            const double n = std::nearbyint(k >= 0 ? x * scale : x / scale);
            const double r = k >= 0 ? n / scale : n * scale;
            if (static_cast<float>(r) == f) return r;
        }
    }
    // The shortest %g that reads back as the same float, then that text read as a double.
    for (int precision = 1; precision <= 9; ++precision) {
        char norm[64];
        const size_t len = print_g(static_cast<double>(f), precision, norm);
        if (len == 0) break;
        float back = 0;
        double wide = 0;
        const char* end = norm + len;
        if (fast_float::from_chars(norm, end, back).ptr == end && back == f &&
            fast_float::from_chars(norm, end, wide).ptr == end)
            return wide;
    }
    return static_cast<double>(f);
}

Str quoted(Symbol s) {
    return "'" + Str(s.str()) + "'";
}

Str decimal(int64_t value) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, value);
    return Str(std::string_view(buf, static_cast<size_t>(r.ptr - buf)));
}

Str text_of(const Path& path, size_t count, char separator) {
    Str out;
    path.write(str_out(out), count, separator);
    return out;
}

Str text_of(const Node& node) {
    Str out;
    write(node, str_out(out));
    return out;
}

Str dta_of(const Array& root) {
    Str out;
    write_dta(root, str_out(out));
    return out;
}

void json_string(Str& out, std::string_view s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += ch;
        }
    }
    out += '"';
}

void json_names(Str& out, std::span<const Symbol> names) {
    out += '[';
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += ", ";
        json_string(out, names[i].str());
    }
    out += ']';
}

}  // namespace baker::detail
