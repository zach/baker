// Script: native functions, script functions, typed lookups that evaluate, eval, and
// check.
//
//   baker_example_scripting path/to/examples/scripting/rules.dta
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/std_script.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace {

void show(baker::Context& ctx, const char* text) {
    auto r = ctx.eval(text);
    if (r)
        std::printf("%s => %s\n", text, baker::to_string(*r).c_str());
    else
        std::printf("%s => error:\n%s\n", text, baker::describe(r.error()).data());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    baker::Context ctx;

    // A native function: parameter and result types are deduced from the lambda.
    auto spawn = ctx.func("spawn", [](baker::Symbol kind, int64_t count) {
        std::printf("  spawn %lld %s\n", static_cast<long long>(count), kind.c_str());
        return count;
    });
    if (!spawn) return 1;
    spawn->doc("Spawns `count` enemies of `kind`.");
    ctx.global("difficulty", 2, "Set by the game: 1 to 3.");

    // Read from its own directory, so errors name the file as its author knows it.
    const std::filesystem::path path = argv[1];
    if (path.has_parent_path()) std::filesystem::current_path(path.parent_path());
    auto tree = baker::read_file(path.filename().string());
    if (!tree) return std::printf("%s\n", baker::to_string(tree.error()).data()), 1;

    // check walks the data without running it: a call with the wrong arguments is an error,
    // and a call to a function nobody has written yet is the demand report.
    std::printf("check:\n%s", baker::to_string(baker::check(**tree, ctx)).c_str());

    // Running the file's top level defines `scaled`.
    if (!ctx.run((*tree)->nodes())) return 1;

    // Typed lookups evaluate commands and variables in the current Context.
    const baker::Array& root = **tree;
    std::printf("grunt hp %lld, brute hp %lld, brute damage %lld\n",
                static_cast<long long>(*root.get<int64_t>("enemy.grunt.hp")),
                static_cast<long long>(*root.get<int64_t>("enemy.brute.hp")),
                static_cast<long long>(*root.get<int64_t>("enemy.brute.damage")));
    ctx.var("difficulty") = 3;
    std::printf("at difficulty 3: brute hp %lld, damage %lld\n",
                static_cast<long long>(*root.get<int64_t>("enemy.brute.hp")),
                static_cast<long long>(*root.get<int64_t>("enemy.brute.damage")));

    // A handler is an array of commands, run in order.
    std::printf("on_enter:\n");
    if (const baker::Array* on_enter = root.try_get("level.on_enter"))
        for (const baker::Node& command : on_enter->nodes().subspan(1))
            if (!ctx.eval(command)) return 1;

    // eval runs text, as a console would.
    show(ctx, "{scaled 10}");
    show(ctx, "{func ratio ($a $b) {% $a $b}}");
    show(ctx, "{ratio 7 0}");  // an error names the call stack it unwound through
    return 0;
}
