// The host kit's live loop: the stack from its project file, reloaded on a save.
#include <doctest.h>
#include <baker/host/live.hpp>
#include <baker/std_interop.hpp>

#include "alloc_counter.hpp"
#include "host/sockets.hpp"
#include "temp_dir.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace baker;
using baker::host::Live;
using baker::test::TempDir;
namespace net = baker::host::net;

namespace {

const char* const kProject =
    "(project\n"
    "   (include_dirs \"data\")\n"
    "   (layers\n"
    "      (default \"data/default.dta\")\n"
    "      (local \"data/local.dta\" optional)))\n";

// A stack on the disk: a project file, a layer that defines a script function in #autorun.
struct Stack {
    TempDir dir;
    Stack() {
        dir.write("baker-project.dta", kProject);
        dir.write("data/default.dta",
                  "#autorun {func speed_twice () {* {config.get speed} 2}}\n(speed 3)\n");
    }
};

// Runs frames until `done`, or two seconds pass.
template <class F>
bool frames_until(Live& live, F done) {
    for (int i = 0; i < 200; ++i) {
        live.begin_frame();
        live.end_frame();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int64_t eval_int(Context& ctx, const char* command) {
    auto r = ctx.eval(command);
    REQUIRE(r);
    return r->try_int().value_or(-1);
}

// A free port on 127.0.0.1, for the console. Winsock is started for the call, as nothing
// else has started it yet.
uint16_t free_port() {
    (void)net::start();
    net::socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof addr;
    (void)bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    (void)getsockname(s, reinterpret_cast<sockaddr*>(&addr), &size);
    const uint16_t port = ntohs(addr.sin_port);
    net::close_socket(s);
    net::stop();
    return port;
}

// One request to the console, while the game runs its frames; the reply's line.
std::string request(Live& live, const std::string& line) {
    net::socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(live.console_port());
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return "<no console>";
    (void)net::set_nonblocking(s);
    const std::string text = line + "\n";
    (void)send(s, text.data(), static_cast<int>(text.size()), 0);
    std::string got;
    frames_until(live, [&] {
        char buffer[4096];
        const auto n = recv(s, buffer, static_cast<int>(sizeof buffer), 0);
        if (n > 0) got.append(buffer, static_cast<size_t>(n));
        return got.find("\n\n") != std::string::npos;
    });
    net::close_socket(s);
    return got.substr(0, got.find('\n'));
}

}  // namespace

TEST_CASE("Live opens the stack its project file describes, and runs #autorun") {
    Stack st;
    Context ctx;
    Live live(ctx);
    int changed = 0;
    REQUIRE(live.open({.project = st.dir.path("data"),  // found from below the root
                       .watch = false,
                       .changed = [&](const Config&) { ++changed; }}));
    CHECK(live.project_file() == Symbol(st.dir.path("baker-project.dta")));
    CHECK(live.config().get<int>("speed") == 3);
    CHECK(eval_int(ctx, "{speed_twice}") == 6);  // #autorun ran, with the Config's tools bound
    CHECK(changed == 1);
    CHECK(live.load_errors().empty());
    CHECK(live.config().layers().size() == 3);  // default, the missing local, scratch
}

TEST_CASE("Live takes a stack built in code when there is no project file") {
    TempDir dir;
    const std::string file = dir.write("default.dta", "(speed 5)\n");
    Project built;
    const ProjectLayer layers[] = {{.name = "default", .path = Symbol(file)}};
    built.layers = List<ProjectLayer>(layers);

    Context ctx;
    Live live(ctx);
    REQUIRE(live.open({.project = dir.path("nowhere"), .built = &built, .watch = false}));
    CHECK(live.project_file().empty());
    CHECK(live.config().get<int>("speed") == 5);

    Context ctx2;
    Live none(ctx2);
    std::vector<std::string> logged;
    auto r = none.open({.project = dir.path("nowhere"), .watch = false, .log = [&](const Error& e) {
                            logged.emplace_back(to_string(e));
                        }});
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::NotFound);
    CHECK(logged.size() == 1);
    CHECK(none.load_errors().size() == 1);
}

TEST_CASE("Live leaves out what it is told, and reports every broken layer with go_on") {
    Stack st;
    st.dir.write("data/local.dta", "(speed 9\n");  // does not parse
    Context ctx;
    Live stops(ctx);
    CHECK_FALSE(stops.open({.project = st.dir.path("data"), .watch = false}));

    Context ctx2;
    Live tool(ctx2);
    ProjectOptions go_on;
    go_on.go_on = true;
    REQUIRE(tool.open({.project = st.dir.path("data"), .open = go_on, .watch = false}));
    REQUIRE(tool.load_errors().size() == 1);
    CHECK(std::string(tool.load_errors()[0].file().str()).find("local.dta") != std::string::npos);
    CHECK(tool.config().get<int>("speed") == 3);

    Context ctx3;
    Live sim(ctx3);
    REQUIRE(
        sim.open({.project = st.dir.path("data"), .open = {.skip = {"local"}}, .watch = false}));
    CHECK(sim.config().layers().size() == 2);
}

TEST_CASE("Live tells the game of a live edit, the fast path's too") {
    if constexpr (!kShipping) {
        Stack st;
        Context ctx;
        Live live(ctx);
        int changed = 0;
        REQUIRE(live.open({.project = st.dir.path("data"),
                           .watch = false,
                           .changed = [&](const Config&) { ++changed; }}));
        REQUIRE(changed == 1);
        live.begin_frame();
        CHECK(changed == 1);  // nothing happened
        REQUIRE(live.config().set("speed", Node(5)));
        REQUIRE(live.config().set("speed", Node(6)));  // the fast path
        live.begin_frame();
        CHECK(changed == 2);  // once a frame, however many edits
        REQUIRE(live.config().set("speed", Node(7)));
        live.begin_frame();
        CHECK(changed == 3);
        REQUIRE(eval_int(ctx, "{do {config.set speed 8} 1}") == 1);  // as the console does
        live.begin_frame();
        CHECK(changed == 4);
        REQUIRE(live.config().clear_scratch());
        live.begin_frame();
        live.begin_frame();
        CHECK(changed == 5);
        CHECK(eval_int(ctx, "{speed_twice}") == 6);
    }
}

TEST_CASE("Live reloads a saved file, with the save as its origin, and runs #autorun again") {
    if constexpr (!kShipping) {
        Stack st;
        Context ctx;
        Live live(ctx);
        int changed = 0;
        REQUIRE(live.open(
            {.project = st.dir.path("data"), .changed = [&](const Config&) { ++changed; }}));
        st.dir.write("data/default.dta",
                     "#autorun {func speed_twice () {* {config.get speed} 20}}\n(speed 4)\n");
        REQUIRE(frames_until(live, [&] { return live.config().get<int>("speed") == 4; }));
        CHECK(eval_int(ctx, "{speed_twice}") == 80);  // redefined by the new #autorun
        CHECK(changed == 2);
        const auto rec = live.config().last_reload();
        REQUIRE(rec);
        CHECK(rec->trigger == Symbol("watcher"));
        CHECK(rec->origin_ns > 0);
        CHECK(rec->origin_ns <= rec->start_ns);  // the save came before the reload

        // A new include in another directory is watched from the next frame on.
        st.dir.write("shared/more.dta", "(lives 3)\n");
        st.dir.write("data/default.dta", "#include ../shared/more.dta\n(speed 4)\n");
        REQUIRE(frames_until(live, [&] { return live.config().get<int>("lives") == 3; }));
        st.dir.write("shared/more.dta", "(lives 7)\n");
        CHECK(frames_until(live, [&] { return live.config().get<int>("lives") == 7; }));

        // The project file changed: said, not acted on.
        std::vector<std::string> logged;
        Context ctx2;
        Live other(ctx2);
        REQUIRE(other.open({.project = st.dir.path("data"),
                            .log = [&](const Error& e) { logged.emplace_back(to_string(e)); }}));
        st.dir.write("baker-project.dta", std::string(kProject) + "; edited\n");
        CHECK(frames_until(other, [&] { return !logged.empty(); }));
        CHECK(logged.front().find("restart") != std::string::npos);
    }
}

TEST_CASE("Live notices a file added to a directory of layers, and one removed") {
    if constexpr (!kShipping) {
        TempDir dir;
        dir.write("baker-project.dta",
                  "(project (layers (default \"data/default.dta\") (mods (dir \"mods\"))))\n");
        dir.write("data/default.dta", "(speed 1)\n");
        dir.write("mods/order.dta", "(order fast)\n");
        Context ctx;
        Live live(ctx);
        REQUIRE(live.open({.project = dir.path("")}));
        REQUIRE(live.config().get<int>("speed") == 1);
        dir.write("mods/fast.dta", "(speed 5)\n");
        REQUIRE(frames_until(live, [&] { return live.config().get<int>("speed") == 5; }));
        dir.write("mods/pack/layer.dta", "(lives 3)\n");  // a new folder with its layer.dta
        REQUIRE(frames_until(live, [&] { return live.config().get<int>("lives") == 3; }));
        std::filesystem::remove(dir.path("mods/fast.dta"));
        CHECK(frames_until(live, [&] { return live.config().get<int>("speed") == 1; }));
    }
}

TEST_CASE("Live notices a record's file added to an #include_dir directory") {
    if constexpr (!kShipping) {
        TempDir dir;
        dir.write("baker-project.dta", "(project (layers (default \"data/default.dta\")))\n");
        dir.write("data/default.dta", "(cards #include_dir \"cards\")\n");
        dir.write("data/cards/fireball.dta", "(cost 3)\n");
        Context ctx;
        Live live(ctx);
        REQUIRE(live.open({.project = dir.path("")}));
        REQUIRE(live.config().get<int>("cards.fireball.cost") == 3);
        dir.write("data/cards/frost.dta", "(cost 2)\n");
        CHECK(frames_until(live, [&] { return live.config().get<int>("cards.frost.cost") == 2; }));
    }
}

TEST_CASE("Live watches a host's virtual paths as the files they are") {
    if constexpr (!kShipping) {
        Stack st;
        const std::string root = st.dir.path("");
        const auto on_disk = [root](std::string_view path) {
            return path.substr(0, 6) == "res://" ? root + std::string(path.substr(6))
                                                 : std::string();
        };
        Project built;
        const ProjectLayer layers[] = {{.name = "default", .path = "res://data/default.dta"}};
        built.layers = List<ProjectLayer>(layers);
        Context ctx;
        Live live(ctx);
        REQUIRE(live.open({.built = &built,
                           .reader =
                               [&](std::string_view path, TextSink out) {
                                   const std::string file = on_disk(path);
                                   if (file.empty() || !std::filesystem::exists(file)) return false;
                                   out(st.dir.read(file.substr(root.size())));
                                   return true;
                               },
                           .disk_path =
                               [&](std::string_view path, TextSink out) {
                                   const std::string file = on_disk(path);
                                   if (file.empty()) return false;
                                   out(file);
                                   return true;
                               }}));
        REQUIRE(live.config().get<int>("speed") == 3);
        st.dir.write("data/default.dta", "(speed 4)\n");
        REQUIRE(frames_until(live, [&] { return live.config().get<int>("speed") == 4; }));
        CHECK(live.config().source("speed")->file == Symbol("res://data/default.dta"));
    }
}

TEST_CASE("Live serves the console, writing files only when asked") {
    if constexpr (!kShipping) {
        Stack st;
        Context ctx;
        Live live(ctx);
        REQUIRE(live.open({.project = st.dir.path("data"), .console_port = free_port()}));
        REQUIRE(live.console_port() != 0);
        CHECK(request(live, "(1 {config.set speed 6})").find(R"("ok":true)") != std::string::npos);
        CHECK(live.config().get<int>("speed") == 6);
        CHECK(request(live, "(2 {config.write_back speed})").find(R"("ok":false)") !=
              std::string::npos);  // not bound

        Context ctx2;
        Live writes(ctx2);
        REQUIRE(writes.open(
            {.project = st.dir.path("data"), .console_port = free_port(), .console_write = true}));
        CHECK(request(writes, "(1 {config.set speed 8})").find(R"("ok":true)") !=
              std::string::npos);
        CHECK(request(writes, "(2 {config.write_back speed})").find(R"("ok":true)") !=
              std::string::npos);
        CHECK(st.dir.read("data/default.dta").find("(speed 8)") != std::string::npos);
    }
}

TEST_CASE("the console on a free port, which console_port() reports") {
    if constexpr (!kShipping) {
        Stack st;
        Context ctx;
        Live live(ctx);
        REQUIRE(live.open({.project = st.dir.path("data"), .console_any_port = true}));
        CHECK(live.console_port() != 0);
        Context ctx2;
        Live other(ctx2);  // a second instance takes another free port
        REQUIRE(other.open({.project = st.dir.path("data"), .console_any_port = true}));
        CHECK(other.console_port() != 0);
        CHECK(other.console_port() != live.console_port());
    }
}

TEST_CASE("after a live edit, the game's read of the whole tree sees it") {
    if constexpr (!kShipping) {
        Stack st;
        Context ctx;
        Live live(ctx);
        int64_t seen = 0;
        REQUIRE(live.open(
            {.project = st.dir.path("data"), .watch = false, .changed = [&seen](const Config& c) {
                 seen = c.root()->get<int64_t>("speed").value_or(-1);
             }}));
        CHECK(seen == 3);
        REQUIRE(live.config().set("speed", 7));
        live.begin_frame();  // applies the edit before `changed`
        live.end_frame();
        CHECK(seen == 7);
    }
}

TEST_CASE("a quiet frame allocates nothing") {
    if constexpr (!kShipping) {
        Stack st;
        Context ctx;
        Live live(ctx);
        REQUIRE(live.open({.project = st.dir.path("data"), .console_port = free_port()}));
        for (int i = 0; i < 3; ++i) {
            live.begin_frame();
            live.end_frame();
        }
        const test::AllocationScope scope;
        for (int i = 0; i < 10; ++i) {
            live.begin_frame();
            live.end_frame();
        }
        CHECK(scope.count() == 0);
    }
}

TEST_CASE("under BAKER_SHIPPING the stack opens, with no watcher and no console") {
    if constexpr (kShipping) {
        Stack st;
        Context ctx;
        Live live(ctx);
        REQUIRE(live.open({.project = st.dir.path("data"), .console_port = 4999}));
        CHECK(live.config().get<int>("speed") == 3);
        CHECK(live.console_port() == 0);
    }
}
