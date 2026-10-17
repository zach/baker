#include <baker/allocator.hpp>
#include <baker/error.hpp>

#include "common/containers.hpp"

#include <charconv>

namespace baker {

namespace detail {
struct ErrorRecord {
    ErrorCode code = ErrorCode::Syntax;
    Str message;
    Symbol file;
    int32_t line = 0;
    int32_t column = 0;
    Str path;
    Vec<StackEntry> stack;
    // The formatted forms, made on first request and cleared by any change.
    Str text;
    Str described;
};
}  // namespace detail

namespace {
// Records come from the default allocator: an error may outlive whatever made it.
template <class... A>
detail::ErrorRecord* new_record(A&&... a) {
    return detail::create<detail::ErrorRecord>(default_allocator(), std::forward<A>(a)...);
}
void free_record(detail::ErrorRecord* r) noexcept {
    detail::destroy(default_allocator(), r);
}
detail::ErrorRecord* copy_of(const detail::ErrorRecord* r) {
    return r ? new_record(*r) : nullptr;
}
}  // namespace

std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Syntax: return "syntax";
        case ErrorCode::Range: return "range";
        case ErrorCode::Directive: return "directive";
        case ErrorCode::Io: return "io";
        case ErrorCode::Limit: return "limit";
        case ErrorCode::Type: return "type";
        case ErrorCode::NotFound: return "not found";
        case ErrorCode::Refused: return "refused";
    }
    return "unknown";
}

Error::Error(ErrorCode code, std::string_view message, Symbol file, int32_t line)
    : r_(new_record()) {
    r_->code = code;
    r_->message = message;
    r_->file = file;
    r_->line = line;
}

Error::Error(const Error& other) : r_(copy_of(other.r_)) {}

Error& Error::operator=(const Error& other) {
    if (this != &other) {
        detail::ErrorRecord* r = copy_of(other.r_);
        free_record(r_);
        r_ = r;
    }
    return *this;
}

Error& Error::operator=(Error&& other) noexcept {
    if (this != &other) {
        free_record(r_);
        r_ = other.r_;
        other.r_ = nullptr;
    }
    return *this;
}

Error::~Error() {
    free_record(r_);
}

ErrorCode Error::code() const noexcept {
    return r_ ? r_->code : ErrorCode::Syntax;
}
std::string_view Error::message() const noexcept {
    return r_ ? std::string_view(r_->message) : std::string_view();
}
Symbol Error::file() const noexcept {
    return r_ ? r_->file : Symbol();
}
int32_t Error::line() const noexcept {
    return r_ ? r_->line : 0;
}
int32_t Error::column() const noexcept {
    return r_ ? r_->column : 0;
}
std::string_view Error::path() const noexcept {
    return r_ ? std::string_view(r_->path) : std::string_view();
}
std::span<const StackEntry> Error::stack() const noexcept {
    return r_ ? std::span<const StackEntry>(r_->stack) : std::span<const StackEntry>();
}

namespace {
detail::ErrorRecord& edit(detail::ErrorRecord*& r) {
    if (!r) r = new_record();
    r->text.clear();
    r->described.clear();
    return *r;
}
}  // namespace

void Error::set_message(std::string_view message) {
    edit(r_).message = message;
}
void Error::set_location(Symbol file, int32_t line) {
    detail::ErrorRecord& r = edit(r_);
    r.file = file;
    r.line = line;
    r.column = 0;
}
void Error::set_column(int32_t column) {
    edit(r_).column = column;
}
void Error::set_path(std::string_view path) {
    edit(r_).path = path;
}
void Error::push_stack(const StackEntry& entry) {
    edit(r_).stack.push_back(entry);
}

Error make_error(ErrorCode code, std::string_view message, Symbol file, int32_t line) {
    return Error(code, message, file, line);
}

std::string_view to_string(const Error& e) {
    if (!e.r_) return "baker: no error";
    detail::ErrorRecord& r = *e.r_;
    if (!r.text.empty()) return r.text;
    Str& out = r.text;
    out = "baker: ";
    out += r.message;
    const bool has_file = !r.file.empty();
    const bool has_path = !r.path.empty();
    if (has_file || has_path) {
        out += "  (";
        if (has_file) {
            out += r.file.str();
            if (r.line > 0) {
                out += ':';
                out += detail::decimal(r.line);
                if (r.column > 0) {
                    out += ':';
                    out += detail::decimal(r.column);
                }
            }
        }
        if (has_path) {
            if (has_file) out += ", ";
            out += "at ";
            out += r.path;
        }
        out += ')';
    }
    return out;
}

void write(const StackEntry& entry, TextSink out) {
    out("{");
    if (!entry.on.empty()) {
        out(entry.on.str());
        out(" ");
    }
    out(entry.function.empty() ? std::string_view("...") : entry.function.str());
    out("}");
    if (!entry.file.empty()) {
        out("  (");
        out(entry.file.str());
        if (entry.line > 0) {
            char buf[16];
            buf[0] = ':';
            const auto r = std::to_chars(buf + 1, buf + sizeof buf, entry.line);
            out(std::string_view(buf, static_cast<size_t>(r.ptr - buf)));
        }
        out(")");
    }
}

std::string_view describe(const Error& e) {
    if (!e.r_ || e.r_->stack.empty()) return to_string(e);
    detail::ErrorRecord& r = *e.r_;
    if (!r.described.empty()) return r.described;
    Str out(to_string(e));
    for (const StackEntry& s : r.stack) {
        out += "\n  in ";
        write(s, detail::str_out(out));
    }
    r.described = std::move(out);
    return r.described;
}

}  // namespace baker
