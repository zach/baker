// The host kit (baker::host): a game's tooling from its own main. With a command, the tool
// commands (check, reference, eval, diff, promote); with `play`, the edit-to-see loop and the
// remote console on 127.0.0.1. Run from this directory, or pass --baker-project PATH.
#include <baker/host/commands.hpp>
#include <baker/host/live.hpp>
#include <baker/std_interop.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

static baker::Tweak<float> player_speed{"player.speed", 1.0f};
static int64_t spawned = 0;

// The game's script functions, bound wherever its data is read, so check knows them.
static void bind(baker::Context& ctx) {
    (void)ctx.func("spawn", [](baker::Symbol, int64_t count) { return spawned += count; });
}

int main(int argc, char** argv) {
    auto line = baker::host::parse_command_line(argc, argv);
    if (!line) return baker::host::kCommandError;
    if (line->project.empty()) line->project = ".";
    if (line->command != "play") {
        const int status = baker::host::run_command(*line, {.bind = bind});
        if (status >= 0) return status;
        std::printf("usage: host play [--frames N] [--console-port N]\n%.*s",
                    int(baker::host::command_help().size()), baker::host::command_help().data());
        return baker::host::kCommandError;
    }
    // The game's own flags, which the kit leaves in `rest`.
    long frames = 0, port = 4555;
    for (size_t i = 0; i + 1 < line->rest.size(); ++i) {
        const std::string value(line->rest[i + 1]);
        if (line->rest[i] == "--frames") frames = std::strtol(value.c_str(), nullptr, 10);
        if (line->rest[i] == "--console-port") port = std::strtol(value.c_str(), nullptr, 10);
    }

    baker::Context ctx;
    bind(ctx);
    baker::host::Live live(ctx);
    auto opened =
        live.open({.project = line->project,
                   .console_port = static_cast<uint16_t>(port),
                   .changed =
                       [](const baker::Config&) {
                           std::printf("player speed %g, %lld spawned\n", double(player_speed),
                                       static_cast<long long>(spawned));
                       },
                   .log =
                       [](const baker::Error& e) {
                           std::fprintf(stderr, "%s\n", std::string(baker::to_string(e)).c_str());
                       }});
    if (!opened) return 1;
    if (live.console_port()) std::printf("console: 127.0.0.1:%u\n", live.console_port());
    for (long frame = 0; frames == 0 || frame < frames; ++frame) {
        live.begin_frame();  // reloads what was saved; answers the console
        // ...simulate and draw with player_speed...
        live.end_frame();  // the frame that shows a reload: closes edit-to-see
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
