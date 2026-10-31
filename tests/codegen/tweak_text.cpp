// Compiled to assembly by check_tweak_text.cmake: a shipping build carries none of a tweak's
// tool text.
#include <baker/script.hpp>
#include <baker/tweak.hpp>

baker::Tweak<float> kBoxWidth{"box.width",
                              2.0f,
                              {.range = {0, 10},
                               .step = 0.5,
                               .desc = "TOOL_TEXT_DESC",
                               .kind = "TOOL_TEXT_KIND",
                               .group = "TOOL_TEXT_GROUP"}};

float read_box_width() {
    return kBoxWidth;
}

// A function's and a global's doc text, which a shipping build drops too.
void bind_all(baker::Context& ctx) {
    if (auto f = ctx.func("twice", [](int64_t x) { return 2 * x; })) f->doc("TOOL_TEXT_FUNC");
    ctx.global("level", baker::Node(int64_t{1}), "TOOL_TEXT_GLOBAL");
}
