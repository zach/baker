// The reference harness: Baker's tuning panels in a GLFW and OpenGL 3 window, over a
// small scene of its own.
//
//   baker_harness [data-dir]
//
// It pushes the shared data/default.dta, then data/local/<user>.dta as the user's own
// layer, optional, by the convention in docs/cookbook.md: tuning kept with "Keep for me"
// goes there, and the file is created the first time. Each frame it reloads changed files,
// so editing default.dta in a text editor shows at once.
#include "panels.hpp"

#include <baker/std_interop.hpp>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

static baker::Tweak<float> kRadius{"scene.ball.radius", 24.0f};
static baker::Tweak<std::array<float, 3>> kColor{"scene.ball.color", {1.0f, 0.5f, 0.2f}};
static baker::Tweak<float> kBounce{"scene.ball.bounce", 0.8f};
static baker::Tweak<float> kGravity{"scene.gravity", 900.0f};

namespace {

struct Ball {
    float x = 60, y = 40, vx = 220, vy = 0;
};

// The file system's clock, as the Config's time source: then a file's last write time is
// when its edit was saved, in the same domain, and edit-to-see is measured end to end.
// Offset so that every time is positive (file clocks may count from before 1970).
uint64_t file_clock_ns(std::filesystem::file_time_type t) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch());
    return static_cast<uint64_t>(ns.count()) + (uint64_t{1} << 62);
}

// The latest write among the data files: the origin of whatever the next reload finds.
uint64_t newest_write(const std::string& dir) {
    uint64_t newest = 0;
    std::error_code ec;
    for (const auto& f : std::filesystem::recursive_directory_iterator(dir, ec))
        if (f.is_regular_file(ec)) newest = std::max(newest, file_clock_ns(f.last_write_time(ec)));
    return newest;
}

std::string user_name() {
    for (const char* var : {"BAKER_USER", "USER", "USERNAME"})
        if (const char* v = std::getenv(var); v && *v) return v;
    return "me";
}

void place(float x, float y, float w, float h) {
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
}

void scene(Ball& ball, float dt) {
    place(10, 10, 460, 360);
    if (ImGui::Begin("Scene")) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        const float r = kRadius;
        ball.vy += kGravity * dt;
        ball.x += ball.vx * dt;
        ball.y += ball.vy * dt;
        if (ball.x < r || ball.x > size.x - r) ball.vx = -ball.vx;
        ball.x = std::clamp(ball.x, r, std::max(r, size.x - r));
        if (ball.y > size.y - r) {
            ball.y = size.y - r;
            ball.vy = -ball.vy * kBounce;
        }
        const std::array<float, 3>& c = kColor.get();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRect(at, ImVec2(at.x + size.x, at.y + size.y), IM_COL32(90, 90, 90, 255));
        draw->AddCircleFilled(ImVec2(at.x + ball.x, at.y + ball.y), r,
                              ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], 1)));
        if (ImGui::InvisibleButton("##scene", size)) ball = Ball{};  // click: drop it again
    }
    ImGui::End();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : BAKER_HARNESS_DATA;

    baker::Context ctx;
    harness::ConsoleState console;
    baker::Config cfg;
    cfg.set_warning_handler(
        [&](const baker::Error& e) { console.log.push_back(std::string(baker::to_string(e))); });
    cfg.add_validator(baker::shape::validator(), {.any_thread = true});
    cfg.add_validator(baker::check_validator(ctx));  // script calls, at every merge too
    cfg.set_time_source(
        [](void*) { return file_clock_ns(std::filesystem::file_time_type::clock::now()); });
    if (auto r = cfg.push_layer("default", dir + "/default.dta"); !r) {
        std::fprintf(stderr, "%s\n", baker::to_string(r.error()).data());
        return 1;
    }
    if (auto r =
            cfg.push_layer("local", dir + "/local/" + user_name() + ".dta", {.optional = true});
        !r) {
        std::fprintf(stderr, "%s\n", baker::to_string(r.error()).data());
        return 1;
    }
    Ball ball;
    if (!ctx.func("drop", [&ball] { ball = Ball{}; }) ||
        !ctx.func("tweak", [&cfg](std::string_view path, double v) {
            return bool(cfg.set(path, baker::Node(v)));
        }))
        return 1;

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1400, 900, "Baker harness", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    harness::TweaksState tweaks;
    harness::ShapesState shapes;
    harness::LayersState layers;
    harness::SnapshotsState snapshots;
    auto last_reload = std::chrono::steady_clock::now();
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        // Pick up files edited outside the harness, four times a second.
        if (const auto now = std::chrono::steady_clock::now();
            now - last_reload > std::chrono::milliseconds(250)) {
            last_reload = now;
            if (auto r = cfg.reload({.trigger = "watcher", .origin_ns = newest_write(dir)}); !r)
                console.log.push_back(std::string(baker::to_string(r.error())));
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        scene(ball, std::min(ImGui::GetIO().DeltaTime, 1.0f / 30));
        place(480, 10, 440, 360);
        harness::tweaks_panel(cfg, baker::TweakRegistry::global(), tweaks);
        place(930, 10, 460, 360);
        harness::shapes_panel(cfg, shapes);
        place(10, 380, 460, 510);
        harness::console_panel(ctx, console);
        place(480, 380, 440, 300);
        harness::loop_panel(cfg);
        place(930, 770, 460, 120);
        harness::journal_panel(cfg);
        place(930, 380, 460, 380);
        harness::layers_panel(cfg, layers);
        place(480, 690, 440, 110);
        harness::snapshots_panel(cfg, snapshots);
        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.12f, 0.12f, 0.14f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        cfg.mark_visible();  // this frame shows whatever the last reload changed
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
