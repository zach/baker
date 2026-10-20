// Live (host/live.hpp): a Config, its stack opened from the project file, reloaded when a
// watched file is saved.

#include <baker/host/console.hpp>
#include <baker/host/live.hpp>

#include "common/containers.hpp"

#if BAKER_HOST_SHAPE
#include <baker/shape.hpp>
#endif

namespace baker::host {

struct Live::Impl {
    Impl(Context& c, const ConfigOptions& o) : ctx(c), cfg(o) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void log(const Error& e) const {
        if (on_log) on_log(e);
    }
    void problem(const Error& e) {
        load_errors.push_back(e);
        log(e);
    }

    // Watches the stack's files and the project file, as they are now.
    void rewatch() {
        const std::string_view more[] = {project_file.str()};
        watcher.watch(cfg.stack_files(), project_file.empty()
                                             ? std::span<const std::string_view>()
                                             : std::span<const std::string_view>(more));
        epoch = cfg.stack_files_epoch();
    }

    // Runs every enabled layer's #autorun commands, which also undoes what a removed layer
    // defined, then tells the game.
    void autorun() {
        stack_changed = false;
        load_errors.clear();
        for (const Error& e : ctx.run_autorun(cfg.autorun().items())) problem(e);
        if (changed) changed(cfg);
        seen = cfg.epoch();  // what #autorun and `changed` set is seen
    }

    Context& ctx;
    Config cfg;
    Watcher watcher;
    Own<Console> console;
    Callback<void(const Config&)> changed;
    Callback<void(const Error&)> on_log;
    Symbol project_file;
    Vec<Error> load_errors;
    uint64_t epoch = 0;  // stack_files_epoch() when last watched
    uint64_t seen = 0;   // the Config's epoch() when the game last read it
    bool watching = false;
    bool stack_changed = false;  // set after any change; #autorun runs again
};

Live::Live(Context& ctx, const ConfigOptions& config)
    : impl_(detail::create<Impl>(default_allocator(), ctx, config)) {
    Impl& im = *impl_;
    im.cfg.set_warning_handler([this](const Error& e) { impl_->log(e); });
    im.cfg.set_time_source([](void*) { return file_clock_now_ns(); });
    im.cfg.on_reload_complete([this](const ReloadRecord& r) {
        if (r.outcome == ReloadOutcome::Ok) impl_->stack_changed = true;
    });
}

Live::~Live() {
    detail::destroy(default_allocator(), impl_);
}

Expected<void> Live::open(const LiveOptions& options) {
    Impl& im = *impl_;
    im.changed = options.changed;
    im.on_log = options.log;
    im.load_errors.clear();
    if (options.reader) im.cfg.session().set_reader(options.reader);
    if (options.disk_path) im.watcher.set_disk_path_hook(options.disk_path);
    if (options.stamp) im.watcher.set_stamp_hook(options.stamp);

    // The stack: the nearest project file, or the one the host built.
    Project read;
    const Project* project = options.built;
    if (!options.project.empty()) {
        im.project_file = find_project(options.project, options.reader);
        if (!im.project_file.empty()) {
            auto r = read_project(im.project_file.str(), options.reader);
            if (!r) {
                im.problem(r.error());
                return Unexpected(std::move(r).error());
            }
            read = std::move(*r);
            project = &read;
        }
    }
    if (!project) {
        Error e = make_error(ErrorCode::NotFound, "no " + Str(kProjectFileName) + " at or above '" +
                                                      Str(options.project) +
                                                      "', and no stack built in code");
        im.problem(e);
        return Unexpected(std::move(e));
    }

    im.cfg.add_validator(check_validator(im.ctx, {.packs = &im.cfg.read_session()}));
#if BAKER_HOST_SHAPE
    im.cfg.add_validator(shape::validator(), {.any_thread = true});
#endif
    auto opened = open_project(im.cfg, *project, options.open);
    if (!opened) {
        im.problem(opened.error());
        return Unexpected(std::move(opened).error());
    }
    Vec<Error> problems;  // a layer that did not read, with go_on: kept past autorun's reset
    for (const ProjectProblem& p : *opened) problems.push_back(p.error);

    if (auto r = bind_config_tools(im.ctx, im.cfg, {.allow_write = options.console_write}); !r)
        im.log(r.error());
    im.autorun();
    for (const Error& e : problems) im.problem(e);

    if constexpr (!kShipping) {
        if (options.watch) {
            im.watching = true;
            im.rewatch();
        }
        if (options.console_port) {
            im.console = Own<Console>::make(default_allocator(), im.ctx);
            if (!im.console->listen(options.console_port)) {
                im.log(make_error(ErrorCode::Io, "the console cannot listen on 127.0.0.1:" +
                                                     detail::decimal(options.console_port)));
                im.console = Own<Console>();
            }
        }
    }
    return {};
}

void Live::begin_frame() {
    Impl& im = *impl_;
    if (im.console) im.console->poll();
    if (im.watching && im.watcher.poll()) {
        for (const Symbol f : im.watcher.changed())
            if (f == im.project_file)
                im.log(make_error(ErrorCode::Refused,
                                  "the project file changed; restart to open the stack it "
                                  "describes now",
                                  f, 1));
        auto r = im.cfg.reload({.trigger = "watcher", .origin_ns = im.watcher.newest_write_ns()});
        if (!r) im.log(r.error());
    }
    // The stack read other files (a new #include, a layer pushed or removed): watch those.
    if (im.watching && im.cfg.stack_files_epoch() != im.epoch) im.rewatch();
    if (im.stack_changed) {
        im.autorun();
    } else if (im.cfg.epoch() != im.seen) {  // a live edit
        if (im.changed) im.changed(im.cfg);
        im.seen = im.cfg.epoch();
    }
}

void Live::end_frame() noexcept {
    impl_->cfg.mark_visible();
}

bool Live::reload_now() {
    Impl& im = *impl_;
    auto r = im.cfg.reload({.trigger = "reload"});
    if (!r) {
        im.log(r.error());
        return false;
    }
    if (im.stack_changed) im.autorun();
    return *r;
}

Config& Live::config() noexcept {
    return impl_->cfg;
}

const Config& Live::config() const noexcept {
    return impl_->cfg;
}

Symbol Live::project_file() const noexcept {
    return impl_->project_file;
}

std::span<const Error> Live::load_errors() const noexcept {
    return {impl_->load_errors.data(), impl_->load_errors.size()};
}

uint16_t Live::console_port() const noexcept {
    return impl_->console ? impl_->console->port() : 0;
}

}  // namespace baker::host
