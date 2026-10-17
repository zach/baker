// A calculator: evaluates each argument, or each line of standard input, as a Baker
// expression and prints the result.
//
//   baker_example_calculator [--functions functions.dta] "{+ 1 {* 2 3}}" "{square 7}"
//
// The arithmetic is the built-in vocabulary; `hypot` below is a native function bound in
// one line; `square`, `average` and `clamp` are script functions from functions.dta. $ans
// holds the previous result.
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/write.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

bool show(baker::Context& ctx, const std::string& line) {
    auto r = ctx.eval(line, "<input>");
    if (!r) {
        std::printf("%s\n", baker::to_string(r.error()).data());
        return false;
    }
    // Baker writes text through a TextSink: here, straight to stdout.
    baker::write(*r, {[](void*, std::string_view t) { std::fwrite(t.data(), 1, t.size(), stdout); },
                      nullptr});
    std::printf("\n");
    ctx.var("ans") = *r;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    baker::Context ctx;
    if (auto f = ctx.func("hypot", [](double a, double b) { return std::hypot(a, b); }))
        f->doc("Length of the hypotenuse.");
    ctx.global("ans", 0, "The previous result.");

    int first = 1;
    if (argc > 2 && std::strcmp(argv[1], "--functions") == 0) {
        auto tree = baker::read_file(argv[2]);
        if (!tree || !ctx.run((*tree)->nodes())) {
            std::printf("cannot load %s\n", argv[2]);
            return 1;
        }
        first = 3;
    }
    bool ok = true;
    if (first < argc) {
        for (int i = first; i < argc; ++i) ok = show(ctx, argv[i]) && ok;
    } else {
        for (std::string line; std::getline(std::cin, line);)
            if (!line.empty()) ok = show(ctx, line) && ok;
    }
    return ok ? 0 : 1;
}
