// The one-line constant replacement.
//
// Before, the box width was a constant:
//
//     const float kBoxWidth = 2.0f;
//
// After, it is a tweak: same name, same default, one line. Its value now comes from
// config.dta if the file sets it, reloads when the file changes, and can be tuned live and
// saved back to the file.
//
//   baker_example_one_line_tweak [config.dta] [--watch]
//
// With --watch, edit config.dta while it runs and the new width prints.
#include <baker/config.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

static baker::Tweak<float> kBoxWidth{
    "box.width", 2.0f, {.range = {0, 10}, .step = 1, .desc = "Box width"}};

namespace {

void show() {
    std::printf("box width %g", static_cast<double>(kBoxWidth));
    if (auto s = kBoxWidth.source())
        std::printf("  (layer %s, %s:%d)", s->layer.c_str(), s->file.c_str(), s->line);
    if (!kBoxWidth.note().empty())
        std::printf("  note: %.*s", int(kBoxWidth.note().size()), kBoxWidth.note().data());
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 && std::strcmp(argv[1], "--watch") != 0 ? argv[1] : "config.dta";
    const bool watch = std::strcmp(argv[argc - 1], "--watch") == 0;

    std::printf("before loading: ");
    show();  // the default: no config yet

    baker::Config cfg;
    if (auto r = cfg.load(path); !r) {
        std::fprintf(stderr, "%s\n", baker::to_string(r.error()).data());
        return 1;
    }
    std::printf("after loading:  ");
    show();

    kBoxWidth.on_change([] {
        std::printf("reloaded:       ");
        show();
    });
    while (watch) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (auto r = cfg.reload(); !r)
            std::fprintf(stderr, "%s\n", baker::to_string(r.error()).data());
    }
    return 0;
}
