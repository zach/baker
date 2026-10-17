#pragma once

// TextSink: where Baker writes text it produces (a printed tree, the script reference, a
// check report, a file read through a custom reader). A function pointer and a context,
// so producing text never requires a std::string: the host decides where it goes.
// <baker/std_interop.hpp> adapts a std::string.

#include <string_view>

namespace baker {

struct TextSink {
    void (*write)(void* user, std::string_view text) = nullptr;
    void* user = nullptr;

    void operator()(std::string_view text) const {
        if (write) write(user, text);
    }
};

}  // namespace baker
