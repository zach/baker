#pragma once

// Opt-in interop between baker::tools and the C++ standard library: the std::string forms of
// what the tools write through TextSink, as <baker/std_script.hpp> is for baker::script.

#include <baker/std_interop.hpp>
#include <baker/tools/reference.hpp>

#include <string>

namespace baker::tools {

// The check report as JSON (write_json, docs/check-json.md).
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

}  // namespace baker::tools
