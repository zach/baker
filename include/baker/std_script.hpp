#pragma once

// Opt-in interop between baker::script and the C++ standard library: the std::string forms
// of what baker::script writes through TextSink. The baker::script counterpart of
// <baker/std_interop.hpp>, kept apart so baker::data never includes script headers.

#include <baker/script.hpp>
#include <baker/std_interop.hpp>

#include <string>

namespace baker {

// The check report, one line each (baker::check).
inline std::string to_string(const CheckReport& report) {
    std::string out;
    write(report, string_out(out));
    return out;
}

// The check report as JSON (baker::write_json, docs/check-json.md).
inline std::string to_json(const CheckReport& report, const LoadSession* packs = nullptr) {
    std::string out;
    write_json(report, string_out(out), packs);
    return out;
}

// The script reference, as Markdown.
inline std::string reference(const Context& ctx, const LoadSession* packs = nullptr) {
    std::string out;
    reference(ctx, string_out(out), packs);
    return out;
}

// The script calls in progress on this thread's current Context, one line each.
inline std::string script_backtrace() {
    std::string out;
    script_backtrace(string_out(out));
    return out;
}

}  // namespace baker
