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

// The script calls in progress on this thread's current Context, one line each.
inline std::string script_backtrace() {
    std::string out;
    script_backtrace(string_out(out));
    return out;
}

}  // namespace baker
