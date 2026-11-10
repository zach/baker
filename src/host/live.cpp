// Live (host/live.hpp): a Config, its stack opened from the project file, reloaded when a
// watched file is saved.

#include <baker/host/live.hpp>
#include <baker/tools/console.hpp>

#include "common/containers.hpp"

#include <atomic>
#include <thread>

#if BAKER_HOST_SHAPE
#include <baker/shape.hpp>
#include <baker/tools/rules.hpp>
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

    // Reloads on the host's worker. The work holds the slot, not Live, so a job the
    // host runs after Live closed, or never runs, touches nothing freed.
    struct JobSlot {
        enum : int { Queued, Running, Done, Cancelled };
        ReloadJob job;
        std::atomic<int> state{Queued};
    };
    Callback<void(Callback<void()>)> run_async;
    Rc<JobSlot> job;             // the worker's, begun and not yet finished
    bool reload_wanted = false;  // the watcher saw a change not yet in a job
    uint64_t reload_origin = 0;  // the newest write the change was seen with

    // Begins a reload on the worker, or runs one here without one.
    void start_reload() {
        reload_wanted = false;
        const ReloadRequest req{.trigger = Symbol("watcher"), .origin_ns = reload_origin};
        if (!run_async) {
            auto r = cfg.reload(req);
            if (!r) log(r.error());
            return;
        }
        job = Rc<JobSlot>::make(default_allocator(MemoryKind::Tools));
        job->job = cfg.begin_reload(req);
        run_async([slot = job] {
            int queued = JobSlot::Queued;
            if (!slot->state.compare_exchange_strong(queued, JobSlot::Running,
                                                     std::memory_order_acq_rel))
                return;  // cancelled: Live closed
            slot->job.run();
            slot->state.store(JobSlot::Done, std::memory_order_release);
        });
    }
    // Finishes the worker's reload once its run returned; one the stack moved under is
    // begun again.
    void poll_job() {
        if (!job || job->state.load(std::memory_order_acquire) != JobSlot::Done) return;
        auto r = cfg.finish_reload(std::move(job->job));
        job = nullptr;
        if (!r)
            log(r.error());
        else if (*r == ReloadOutcome::Refused)
            reload_wanted = true;
    }
    // Before closing, or a reload now: a job not yet started is cancelled, one running is
    // waited for; either way it is destroyed here, while the Config it read from lives.
    void settle_job(bool cancel) {
        if (!job) return;
        int queued = JobSlot::Queued;
        if (cancel && job->state.compare_exchange_strong(queued, JobSlot::Cancelled,
                                                         std::memory_order_acq_rel)) {
            job->job = ReloadJob();
            job = nullptr;
            return;
        }
        while (job->state.load(std::memory_order_acquire) != JobSlot::Done)
            std::this_thread::yield();
    }
};

Live::Live(Context& ctx, const ConfigOptions& config)
    : impl_(detail::create<Impl>(default_allocator(MemoryKind::Tools), ctx, config)) {
    Impl& im = *impl_;
    im.cfg.set_warning_handler([this](const Error& e) { impl_->log(e); });
    im.cfg.set_time_source([](void*) { return file_clock_now_ns(); });
    im.cfg.on_reload_complete([this](const ReloadRecord& r) {
        if (r.outcome == ReloadOutcome::Ok) impl_->stack_changed = true;
    });
}

Live::~Live() {
    impl_->settle_job(true);  // the worker may still be reading into the job
    detail::destroy(default_allocator(MemoryKind::Tools), impl_);
}

Expected<void> Live::open(const LiveOptions& options) {
    Impl& im = *impl_;
    im.changed = options.changed;
    im.on_log = options.log;
    im.load_errors.clear();
    if (options.reader) im.cfg.session().set_reader(options.reader);
    if (options.lister) im.cfg.session().set_lister(options.lister);
    if (options.disk_path) im.watcher.set_disk_path_hook(options.disk_path);
    if (options.stamp) im.watcher.set_stamp_hook(options.stamp);
    im.run_async = options.run_async;
    if constexpr (!kShipping) {
        // The reload's scan opens only the files whose stamp moved.
        im.cfg.session().set_stamper(
            [hook = options.stamp](std::string_view path) { return file_stamp(path, hook); });
    }

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

    if (options.tables)
        if (auto r = options.tables(im.cfg); !r) {
            im.problem(r.error());
            return Unexpected(std::move(r).error());
        }
    // A shipping build keeps them for the checks an overlay must pass, not after each merge,
    // where they would only warn.
    constexpr bool kChecksOnly = kShipping;
    im.cfg.add_validator(check_validator(im.ctx, {.packs = &im.cfg.read_session()}),
                         {.checks_only = kChecksOnly});
#if BAKER_HOST_SHAPE
    im.cfg.add_validator(shape::validator(), {.any_thread = true, .checks_only = kChecksOnly});
    // two records, one @id
    im.cfg.add_validator(id_validator(), {.any_thread = true, .checks_only = kChecksOnly});
    // the shapes' rules, in script
    im.cfg.add_validator(tools::rules_validator(im.ctx), {.checks_only = kChecksOnly});
#endif
    auto opened = open_project(im.cfg, *project, options.open);
    if (!opened) {
        im.problem(opened.error());
        return Unexpected(std::move(opened).error());
    }
    Vec<Error> problems;  // a layer that did not read, with go_on: kept past autorun's reset
    for (const ProjectProblem& p : *opened) problems.push_back(p.error);

    // Tool code: a shipping game neither binds config.* nor links it.
    if constexpr (!kShipping) {
        if (auto r =
                tools::bind_config_tools(im.ctx, im.cfg, {.allow_write = options.console_write});
            !r)
            im.log(r.error());
    }
    im.autorun();
    for (const Error& e : problems) im.problem(e);

    if constexpr (!kShipping) {
        if (options.watch) {
            im.watching = true;
            im.rewatch();
        }
        if (options.console_port || options.console_any_port) {
            ConsoleOptions console = options.console;
            if (!console.log) console.log = options.log;
            im.console = Own<Console>::make(default_allocator(MemoryKind::Tools), im.ctx, console);
            if (!im.console->listen(options.console_any_port ? 0 : options.console_port)) {
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
        im.reload_wanted = true;
        im.reload_origin = im.watcher.newest_write_ns();
    }
    im.poll_job();
    if (im.reload_wanted && !im.job) im.start_reload();
    // The stack read other files (a new #include, a layer pushed or removed): watch those.
    if (im.watching && im.cfg.stack_files_epoch() != im.epoch) im.rewatch();
    if (im.stack_changed) {
        im.autorun();
    } else if (im.cfg.epoch() != im.seen) {  // a live edit
        im.cfg.apply_edits();                // so the game's reads of records and tables see it
        if (im.changed) im.changed(im.cfg);
        im.seen = im.cfg.epoch();
    }
}

void Live::end_frame() noexcept {
    impl_->cfg.mark_visible();
}

bool Live::reload_now() {
    Impl& im = *impl_;
    im.settle_job(false);  // a worker's reload lands first, the host's worker willing
    im.poll_job();
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
