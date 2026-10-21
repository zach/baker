// A game binary for the host kit's commands: two script functions, the kit's
// commands, and one command of its own, as a game's main would have them.
#include <baker/host/commands.hpp>
#include <baker/parse.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    auto line = baker::host::parse_command_line(argc, argv);
    if (!line) {
        std::fprintf(stderr, "%.*s\n", int(baker::to_string(line.error()).size()),
                     baker::to_string(line.error()).data());
        return baker::host::kCommandError;
    }
    // The game's own flag: --extra FILE, checked beside its data.
    std::string_view scenario;
    for (size_t i = 0; i + 1 < line->rest.size(); ++i)
        if (line->rest[i] == "--extra") scenario = line->rest[i + 1];

    baker::host::CommandHooks hooks;
    hooks.bind = [](baker::Context& ctx) {
        (void)ctx.func("spawn", [](baker::Symbol, int64_t n) { return n; });
        ctx.global("score", baker::Node(int64_t{0}), "The score.");
        ctx.global("ticks", baker::Node(int64_t{0}), "The steps taken.");
    };
    hooks.reset = [](baker::Context& ctx, uint64_t seed) {
        ctx.var("score") = baker::Node(static_cast<int64_t>(seed * 10));
        ctx.var("ticks") = baker::Node(int64_t{0});
    };
    // The game's fixed step, for sim: it counts.
    hooks.step = [](baker::Context& ctx, double) {
        auto ticks = ctx.var("ticks");
        ticks = baker::Node(ticks->try_int().value_or(0) + 1);
    };
    hooks.extra_trees =
        [scenario](const baker::Config&) -> baker::Expected<baker::List<baker::Ref<baker::Array>>> {
        if (scenario.empty()) return baker::List<baker::Ref<baker::Array>>();
        auto tree = baker::read_file(scenario);
        if (!tree) return baker::Unexpected(std::move(tree).error());
        const baker::Ref<baker::Array> trees[] = {*tree};
        return baker::List<baker::Ref<baker::Array>>(trees);
    };
    const baker::ProjectOptions skip_local{.skip = {"local"}};
    hooks.open = skip_local;

    // --root DIR: the stack is read through the host's reader, as game://..., which names
    // nothing on the disk.
    std::string root;
    for (size_t i = 0; i + 1 < line->rest.size(); ++i)
        if (line->rest[i] == "--root") root = std::string(line->rest[i + 1]) + "/";
    if (!root.empty()) {
        line->project = "game://data";
        hooks.reader = [root](std::string_view path, baker::TextSink out) {
            if (path.substr(0, 7) != "game://") return false;
            std::FILE* f = std::fopen((root + std::string(path.substr(7))).c_str(), "rb");
            if (!f) return false;
            char buf[4096];
            for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out({buf, n});
            std::fclose(f);
            return true;
        };
    }
    // --to-stderr: the host routes the output (an engine that owns stdout).
    for (const std::string_view a : line->rest)
        if (a == "--to-stderr")
            hooks.output = [](std::string_view text) {
                std::fprintf(stderr, "%.*s", int(text.size()), text.data());
            };

    const int status = baker::host::run_command(*line, hooks);
    if (status >= 0) return status;
    if (line->command == "hello") {  // a command of the game's own
        std::printf("hello\n");
        return 0;
    }
    std::fprintf(stderr,
                 "unknown command\ncommands:\n%.*severy command also takes --project PATH\n",
                 int(baker::host::command_help().size()), baker::host::command_help().data());
    return baker::host::kCommandError;
}
