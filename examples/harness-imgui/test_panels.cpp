// The harness's panels, headless: Dear ImGui with no backend, a few frames, and
// assertions on what each panel's requests changed. Runs in a scratch directory of its own.
#include "panels.hpp"

#include <doctest.h>
#include <baker/std_interop.hpp>

#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr const char* kDefault =
    "; shared\n"
    "(ball\n"
    "   (radius 24 (range 4 80) (desc \"Radius\"))\n"
    "   (color 0.9 0.5 0.2 (range 0 1))\n"
    "   (bounce 0.8 (note \"1.0 never settles\")))\n"
    "(shapes (spawner (count int (range 1 50)) (interval float (range 0.1 10))))\n"
    "(spawner (count 3) (interval 2.5))\n";

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// A harness over files in a fresh directory, with its own tweaks.
struct Harness {
    fs::path dir = fs::current_path() / "harness_test";
    baker::TweakRegistry registry;
    baker::Tweak<int> radius{"ball.radius", 10, {}, registry};
    baker::Tweak<float> bounce{"ball.bounce", 0.5f, {}, registry};
    baker::Config cfg{{.tweaks = &registry}};
    baker::Context ctx;
    harness::TweaksState tweaks;
    harness::ShapesState shapes;
    harness::ConsoleState console;
    harness::LayersState layers;
    harness::SnapshotsState snapshots;
    int frames = 0;

    Harness() {
        fs::remove_all(dir);
        fs::create_directories(dir);
        std::ofstream(dir / "default.dta", std::ios::binary) << kDefault;
        REQUIRE(cfg.push_layer("default", (dir / "default.dta").string()));
        // The personal layer, by the convention: optional, and missing until written.
        REQUIRE(
            cfg.push_layer("local", (dir / "local" / "tester.dta").string(), {.optional = true}));
        fs::create_directories(dir / "local");
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 800);
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels;
        int w, h;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);  // no renderer: build the atlas here
    }
    ~Harness() { ImGui::DestroyContext(); }

    void frame() {
        ImGui::NewFrame();
        harness::tweaks_panel(cfg, registry, tweaks);
        harness::shapes_panel(cfg, shapes);
        harness::console_panel(ctx, console);
        harness::loop_panel(cfg);
        harness::journal_panel(cfg);
        harness::layers_panel(cfg, layers);
        harness::snapshots_panel(cfg, snapshots);
        ImGui::Render();
        // The first frame draws nothing: ImGui hides new windows while it sizes them.
        if (frames++ > 0) REQUIRE(ImGui::GetDrawData()->CmdListsCount >= 6);
    }
};

}  // namespace

TEST_CASE("the panels draw, and their requests do what their buttons say") {
    Harness h;
    for (int i = 0; i < 3; ++i) h.frame();
    CHECK(int(h.radius) == 24);
    CHECK(harness::provenance(h.cfg, "ball.radius") == "default.dta:3 (layer default)");
    CHECK(harness::provenance(h.cfg, "ball.bounce") ==
          "default.dta:5 (layer default)\nnote: 1.0 never settles");

    // Console: eval, with results and located errors in the log.
    std::snprintf(h.console.input, sizeof h.console.input, "{+ 1 2}");
    h.console.submit = true;
    h.frame();
    std::snprintf(h.console.input, sizeof h.console.input, "{nope}");
    h.console.submit = true;
    h.frame();
    REQUIRE(h.console.log.size() >= 4);
    CHECK(h.console.log[1] == "=> 3");
    CHECK(h.console.log[3].find("'nope' is not a function  (<console>:1)") != std::string::npos);

    if constexpr (!baker::kShipping) {
        // Tweaks: a slider's edit goes to scratch, and shows as unsaved.
        h.tweaks.request = {harness::TweaksState::Request::Set, "ball.radius", {30}};
        h.frame();
        CHECK(int(h.radius) == 30);
        CHECK(harness::provenance(h.cfg, "ball.radius") == "unsaved (layer scratch)");
        // "Keep for me" writes it into the personal layer, creating local/tester.dta.
        h.tweaks.request = {harness::TweaksState::Request::KeepLocal, "ball.radius", {}};
        h.frame();
        CHECK(read(h.dir / "local" / "tester.dta") == "(ball (radius 30))\n");
        CHECK(harness::provenance(h.cfg, "ball.radius") == "tester.dta:1 (layer local)");
        // "Save" writes back where the value came from: one number changes.
        h.tweaks.request = {harness::TweaksState::Request::Set, "ball.bounce", {0.75}};
        h.frame();
        h.tweaks.request = {harness::TweaksState::Request::Save, "ball.bounce", {}};
        h.frame();
        CHECK(read(h.dir / "default.dta").find("(bounce 0.75 (note") != std::string::npos);
        CHECK(h.tweaks.status == "saved: default.dta:5");

        // Shapes: a field of a shaped array, set and saved.
        h.shapes.request = {harness::ShapesState::Request::Set, "spawner.count", 7};
        h.frame();
        CHECK(h.cfg.get<int>("spawner.count") == 7);
        h.shapes.request = {harness::ShapesState::Request::Save, "spawner.count", 0};
        h.frame();
        CHECK(read(h.dir / "default.dta").find("(spawner (count 7)") != std::string::npos);

        // Snapshots: capture A, change, capture B, flip between them.
        h.snapshots.request = harness::SnapshotsState::CaptureA;
        h.frame();
        REQUIRE(h.cfg.set("ball.radius", baker::Node(50)));
        h.snapshots.request = harness::SnapshotsState::CaptureB;
        h.frame();
        h.snapshots.request = harness::SnapshotsState::Flip;
        h.frame();
        CHECK(h.snapshots.showing == 'A');
        CHECK(int(h.radius) == 30);
        h.snapshots.request = harness::SnapshotsState::Flip;
        h.frame();
        CHECK(h.snapshots.showing == 'B');
        CHECK(int(h.radius) == 50);
        REQUIRE(h.cfg.clear_scratch());

        // Layers: toggling the personal layer off returns the shared value; promoting it
        // writes its effect into default.dta as one change.
        h.layers.toggle = "local";
        h.frame();
        CHECK_FALSE(h.cfg.layer_enabled("local"));
        CHECK(int(h.radius) == 24);
        h.layers.toggle = "local";
        h.frame();
        CHECK(int(h.radius) == 30);
        h.layers.promote = harness::LayersState::Promote{"local", "default"};
        h.frame();
        REQUIRE(!h.layers.report.empty());
        CHECK(h.layers.report[0] == "promoted local into default");
        CHECK(read(h.dir / "default.dta").find("(radius 30 (range 4 80)") != std::string::npos);

        // Journal: every edit above, with its fate.
        std::vector<std::string> lines;
        h.cfg.journal(
            [&](const baker::JournalEntry& e) { lines.push_back(harness::journal_line(e)); });
        REQUIRE(lines.size() >= 4);
        CHECK(lines[0] == "ball/radius  24 -> 30  (default.dta:3)  written tester.dta:1");
        CHECK(lines[1] == "ball/bounce  0.8 -> 0.75  (default.dta:5)  written default.dta:5");

        // Loop statistics: a reload with an origin, then a frame that shows it.
        std::ofstream(h.dir / "default.dta", std::ios::binary | std::ios::app) << "(extra 1)\n";
        REQUIRE(h.cfg.reload({.origin_ns = 1}));
        h.cfg.mark_visible();
        const baker::LoopStats stats = h.cfg.loop_stats();
        REQUIRE(stats.files.size() == 1);
        CHECK(stats.files[0].edits == 1);
        CHECK(stats.layers[0].name == baker::Symbol("default"));
        h.frame();

        // Loop: every change above was recorded, and the panel draws them.
        size_t records = 0;
        h.cfg.each_reload([&](const baker::ReloadRecord&) { ++records; });
        CHECK(records >= 5);
        h.frame();
    }
}
