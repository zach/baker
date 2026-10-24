// The tool commands (host/commands.hpp), run from a game's own binary.

#include <baker/host/commands.hpp>
#include <baker/host/live.hpp>
#include <baker/tools/format.hpp>
#include <baker/tools/layers.hpp>
#include <baker/tools/reference.hpp>
#include <baker/tweak.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"
#include "host/scenarios.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <tuple>

#if BAKER_HOST_SHAPE
#include <baker/shape.hpp>
#include <baker/tools/rules.hpp>
#endif

namespace fs = std::filesystem;

namespace baker::host {
namespace {

// A command's output, gathered while it runs, then sent where the host asked (run_command).
thread_local Str* t_output = nullptr;

void out(std::string_view text) {
    if (t_output) t_output->append(text);
}
void err(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
}
TextSink stdout_sink() {
    return {[](void*, std::string_view text) { out(text); }, nullptr};
}

// The game's data, as the commands see it: a fresh Context with the game's functions, and
// the stack opened without watching.
struct Loaded {
    explicit Loaded(const CommandHooks& h) : hooks(h), live(ctx) {}
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
    ~Loaded() {
        if (bound && hooks.unbind) hooks.unbind(ctx);
    }
    const CommandHooks& hooks;
    Context ctx;
    Live live;
    bool bound = false;
};

bool load(Loaded& l, const CommandLine& line) {
    if (l.hooks.bind) l.hooks.bind(l.ctx);
    l.bound = true;
    LiveOptions o;
    o.project = line.project;
    o.built = l.hooks.built;
    o.reader = l.hooks.reader;
    // A directory the host's lister does not have is the disk's, as for scenarios.
    if (l.hooks.list)
        o.lister = [&l](std::string_view dir, const Callback<void(std::string_view)>& entry) {
            return l.hooks.list(dir, entry) || LoadSession().lister()(dir, entry);
        };
    o.open = l.hooks.open;
    o.watch = false;
    o.changed = l.hooks.changed;
    o.log = [](const Error& e) {
        err(describe(e));
        err("\n");
    };
    return static_cast<bool>(l.live.open(o));
}

// A whole number written in decimal digits.
std::optional<uint64_t> whole(std::string_view s) {
    if (s.empty()) return std::nullopt;
    uint64_t n = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        n = n * 10 + static_cast<uint64_t>(c - '0');
    }
    return n;
}

// The scenarios and objectives the project names (its file's, or the stack built in code).
Expected<void> read_book(const Loaded& l, sim::Book& book) {
    Project read;
    const Project* project = l.hooks.built;
    if (!l.live.project_file().empty()) {
        auto p = read_project(l.live.project_file().str(), l.hooks.reader);
        if (!p) return Unexpected(std::move(p).error());
        read = std::move(*p);
        project = &read;
    }
    if (!project) return {};
    book.step_name = project->step_name;
    for (const List<ProjectEntry>* list : {&project->objectives, &project->scenarios})
        for (const ProjectEntry& e : *list)
            if (auto r = book.read(e.value.str(), l.hooks.reader, l.hooks.list); !r) return r;
    return {};
}

int report_check(CheckReport& report, const Loaded& l, const LoadSession* packs) {
    report.load = List<Error>(l.live.load_errors());
    tools::write_json(report, stdout_sink(), packs);
    out("\n");
    return report.ok() ? 0 : kCommandFailed;
}

int check(const CommandLine& line, const CommandHooks& hooks) {
    Loaded l(hooks);
    CheckReport report;
    // A call to a function nobody defined is demand to Baker: a word the data asks for. In
    // a tool's or an agent's edit it is nearly always a typo, so it fails the check unless
    // declared a stub.
    report.unresolved_fails = true;
    // Data that does not load is still a report: the JSON says why, and the exit status
    // stays "could not run".
    if (!load(l, line)) {
        report_check(report, l, nullptr);
        return kCommandError;
    }
    const Config& cfg = l.live.config();
    auto unreadable = [&](const Error& e) {
        report.load = List<Error>(std::span<const Error>(&e, 1));
        tools::write_json(report, stdout_sink(), &cfg.read_session());
        out("\n");
        err(describe(e));
        err("\n");
        return kCommandError;
    };
    // The project's scenarios and objectives, which an agent writes as it writes data, and
    // the game's own extra files.
    sim::Book book;
    if (auto r = read_book(l, book); !r) return unreadable(r.error());
    List<Ref<Array>> trees;
    if (hooks.extra_trees) {
        auto t = hooks.extra_trees(cfg);
        if (!t) return unreadable(t.error());
        trees = std::move(*t);
    }
    Vec<const Array*> extra;
    for (const Ref<Array>& t : book.trees) extra.push_back(t.get());
    for (const Ref<Array>& t : trees) extra.push_back(t.get());
    check(cfg, l.ctx, report, {.unresolved_fails = true},
          std::span<const Array* const>(extra.data(), extra.size()));
    for (const CheckReport::Demand& d : report.demand)
        if (!d.stub) {
            const CheckReport::Site* site = d.sites.empty() ? nullptr : d.sites.begin();
            err("check: '" + Str(d.name.str()) + "' is not defined (" +
                Str(site ? site->file.str() : "?") + ":" + detail::decimal(site ? site->line : 0) +
                ")\n");
        }
#if BAKER_HOST_SHAPE
    // The shapes, over the merged data: a value out of range or of the wrong type fails the
    // check; a field no shape declares is reported, as demand for a word, and passes.
    shape::Set shapes;
    shape::Report shaped;
    shapes.add(*cfg.root(), shaped);
    shape::check(*cfg.root(), shapes, shaped);
    tools::check_rules(*cfg.root(), shapes, l.ctx, shaped);  // a broken rule fails the check
    for (const Error& e : shaped.errors) err("shape: " + Str(to_string(e)) + "\n");
    for (const Error& e : shaped.warnings) err("shape warning: " + Str(to_string(e)) + "\n");
    report.shape_errors = List<Error>(shaped.errors.items());
    report.shape_warnings = List<Error>(shaped.warnings.items());
#endif
    return report_check(report, l, &cfg.read_session());
}

int reference(const CommandLine& line, const CommandHooks& hooks) {
    Loaded l(hooks);
    if (!load(l, line)) return kCommandError;
    const Config& cfg = l.live.config();
    if (!line.json) {
        tools::reference(l.ctx, stdout_sink(), &cfg.read_session());
        return 0;
    }
    // The three documents in one object: the vocabulary, the tweaks, and the shapes the
    // enabled layers declare.
    Str ref, tweaks, shapes = "null";
    tools::write_reference_json(l.ctx, detail::str_out(ref), &cfg.read_session());
    baker::write_json(TweakRegistry::global(), detail::str_out(tweaks));
#if BAKER_HOST_SHAPE
    shape::Set set;
    shape::Report report;
    for (const Symbol layer : cfg.layers())
        if (cfg.layer_enabled(layer.str()))
            if (const Ref<Array> root = cfg.layer_root(layer.str()))
                set.add(*root, report, &cfg.read_session());
    shapes.clear();
    shape::write_json(set, detail::str_out(shapes));
#endif
    out("{\"reference\": " + ref + ", \"tweaks\": " + tweaks + ", \"shapes\": " + shapes + "}\n");
    return 0;
}

int eval(const CommandLine& line, const CommandHooks& hooks) {
    if (line.commands.empty()) {
        err("eval needs a command: eval \"{...}\"\n");
        return kCommandError;
    }
    Loaded l(hooks);
    if (!load(l, line)) return kCommandError;
    l.ctx.var("seed") = Node(static_cast<int64_t>(line.seed));
    if (hooks.reset) hooks.reset(l.ctx, line.seed);
    int status = 0;
    if (line.json) {
        // One baker.reply/1.1 line per command, as a running game's console answers a
        // request: a tool reads a headless game and a live one the same way.
        int64_t id = 0;
        for (const std::string_view command : line.commands) {
            Str reply;
            eval_request(l.ctx, "(" + detail::decimal(++id) + " " + Str(command) + ")",
                         detail::str_out(reply));
            out(reply);
            if (reply.find(R"(,"ok":false,)") != Str::npos) status = kCommandFailed;
        }
        return status;
    }
    for (const std::string_view command : line.commands) {
        auto r = l.ctx.eval(command);
        if (!r) {
            err(describe(r.error()));
            err("\n");
            status = kCommandFailed;
            continue;
        }
        if (auto text = r->as_str())
            out(*text);
        else
            out(detail::text_of(*r));
        out("\n");
    }
    return status;
}

// One line per value the layer sets: path, the value below it, its value, where.
int print_effect(const Config& cfg, std::string_view layer, std::string_view heading) {
    auto effect = tools::layer_effect(cfg, layer);
    if (!effect) {
        err(to_string(effect.error()));
        err("\n");
        return kCommandError;
    }
    if (effect->empty()) {
        out("layer '" + Str(layer) + "' changes nothing\n");
        return 0;
    }
    out(Str(heading) + "\n");
    for (const LayerEffectEntry& e : *effect) {
        out("  " + Str(e.path.str()) + "  " + (e.below ? detail::text_of(*e.below) : Str("(new)")) +
            " -> " + detail::text_of(e.value) + "  (" + Str(e.source.file.str()) + ":" +
            detail::decimal(e.source.line) + ")" + (e.shadowed ? "  shadowed" : "") + "\n");
    }
    return 0;
}

int diff(const CommandLine& line, const CommandHooks& hooks) {
    Loaded l(hooks);
    if (!load(l, line)) return kCommandError;
    return print_effect(l.live.config(), line.layer, "layer '" + Str(line.layer) + "' changes:");
}

// A file's lines, each with its newline; the last may have none.
Vec<std::string_view> lines_of(std::string_view text) {
    Vec<std::string_view> lines;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        const size_t next = end == std::string_view::npos ? text.size() : end + 1;
        lines.push_back(text.substr(at, next - at));
        at = next;
    }
    return lines;
}

// A unified diff of `before` to `after`, named `name` under a/ and b/ (git apply, patch -p1),
// with three lines of context; empty when they are the same. The common head and tail are
// cut first, so the line-by-line comparison runs only over what changed.
Str unified_diff(std::string_view before, std::string_view after, std::string_view name) {
    if (before == after) return {};
    const Vec<std::string_view> a = lines_of(before), b = lines_of(after);
    size_t head = 0;
    while (head < a.size() && head < b.size() && a[head] == b[head]) ++head;
    size_t tail = 0;
    while (tail < a.size() - head && tail < b.size() - head &&
           a[a.size() - 1 - tail] == b[b.size() - 1 - tail])
        ++tail;
    // The longest common subsequence of the middles, then the edit script over all lines.
    const size_t n = a.size() - head - tail, m = b.size() - head - tail;
    Vec<uint32_t> lcs((n + 1) * (m + 1), 0);
    auto at = [&](size_t i, size_t j) -> uint32_t& { return lcs[i * (m + 1) + j]; };
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            at(i, j) = a[head + i] == b[head + j] ? at(i + 1, j + 1) + 1
                                                  : std::max(at(i + 1, j), at(i, j + 1));
    struct Op {
        char kind;      // ' ', '-', '+'
        size_t ai, bi;  // the line's index in a (for ' ' and '-') and in b (for ' ' and '+')
    };
    Vec<Op> ops;
    for (size_t i = 0; i < head; ++i) ops.push_back({' ', i, i});
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && a[head + i] == b[head + j]) {
            ops.push_back({' ', head + i++, head + j++});
        } else if (i < n && (j == m || at(i + 1, j) >= at(i, j + 1))) {
            ops.push_back({'-', head + i++, head + j});  // removals first, as diff -u
        } else {
            ops.push_back({'+', head + i, head + j++});
        }
    }
    for (size_t k = 0; k < tail; ++k)
        ops.push_back({' ', a.size() - tail + k, b.size() - tail + k});

    Str text = "--- a/" + Str(name) + "\n+++ b/" + Str(name) + "\n";
    // Hunks: runs of changes no more than twice the context apart, with the context around.
    const size_t context = 3;
    Vec<size_t> changes;
    for (size_t k = 0; k < ops.size(); ++k)
        if (ops[k].kind != ' ') changes.push_back(k);
    for (size_t c = 0; c < changes.size();) {
        size_t last = c;
        while (last + 1 < changes.size() && changes[last + 1] - changes[last] <= 2 * context + 1)
            ++last;
        const size_t start = changes[c] > context ? changes[c] - context : 0;
        const size_t end = std::min(ops.size(), changes[last] + context + 1);
        size_t a_count = 0, b_count = 0;
        for (size_t x = start; x < end; ++x) {
            a_count += ops[x].kind != '+' ? 1 : 0;
            b_count += ops[x].kind != '-' ? 1 : 0;
        }
        // A side with no lines in the hunk names the line before it, as diff -u does.
        const size_t a_start = ops[start].ai + (a_count ? 1 : 0);
        const size_t b_start = ops[start].bi + (b_count ? 1 : 0);
        text += "@@ -" + detail::decimal(static_cast<int64_t>(a_start)) + "," +
                detail::decimal(static_cast<int64_t>(a_count)) + " +" +
                detail::decimal(static_cast<int64_t>(b_start)) + "," +
                detail::decimal(static_cast<int64_t>(b_count)) + " @@\n";
        for (size_t x = start; x < end; ++x) {
            const std::string_view l = ops[x].kind == '+' ? b[ops[x].bi] : a[ops[x].ai];
            text += ops[x].kind;
            text += l;
            if (l.empty() || l.back() != '\n') text += "\n\\ No newline at end of file\n";
        }
        c = last + 1;
    }
    return text;
}

// Through the host's reader when it gave one, as the stack was read.
bool read_whole(const Str& path, Str& text, const LoadSession::Reader& reader) {
    text.clear();
    if (reader) return reader(path.view(), detail::str_out(text));
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append({buf, n});
    std::fclose(f);
    return true;
}

bool write_whole(std::string_view path, std::string_view text) {
    std::FILE* f = std::fopen(Str(path).c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

int promote(const CommandLine& line, const CommandHooks& hooks) {
    Loaded l(hooks);
    if (!load(l, line)) return kCommandError;
    Config& cfg = l.live.config();
    auto report_refusals = [](const WriteReport& r) {
        for (const Error& e : r.refused) err("refused: " + Str(to_string(e)) + "\n");
    };
    if (!line.patch.empty()) {
        // The real promotion, with the Config's writer capturing each file instead of
        // writing it: nothing on disk is written, created, or locked.
        Vec<std::pair<Str, Str>> captured;
        cfg.set_writer([&captured](std::string_view path, std::string_view text) {
            captured.emplace_back(Str(path), Str(text));
            return true;
        });
        auto report = cfg.promote(line.layer, line.to);
        if (!report) {
            err(describe(report.error()));
            err("\n");
            return kCommandError;
        }
        report_refusals(*report);
        std::sort(captured.begin(), captured.end(),
                  [](const auto& x, const auto& y) { return x.first.view() < y.first.view(); });
        fs::path root = line.patch_root.empty()
                            ? fs::path(Str(l.live.project_file().str()).c_str()).parent_path()
                            : fs::path(Str(line.patch_root).c_str());
        if (root.empty()) root = ".";  // a project file named without a directory
        Str patch;
        if (report->ok())  // all or nothing, as --baker-write is
            for (const auto& [path, text] : captured) {
                Str before;
                (void)read_whole(path, before, l.hooks.reader);
                std::error_code ec;
                const fs::path rel = fs::relative(fs::path(path.c_str()), root, ec);
                patch += unified_diff(before, text,
                                      ec ? std::string_view(path.view()) : rel.generic_string());
            }
        if (!write_whole(line.patch, patch)) {
            err("cannot write " + Str(line.patch) + "\n");
            return kCommandError;
        }
        out("wrote " + Str(line.patch) + ": " +
            detail::decimal(static_cast<int64_t>(report->ok() ? captured.size() : 0)) +
            " files would change;\napply it from " + Str(root.generic_string()) +
            " with git apply -p1 (or patch -p1)\n");
        return report->ok() ? 0 : kCommandFailed;
    }
    if (!line.write)
        return print_effect(
            cfg, line.layer,
            "would write into " + Str(line.to) + " (rerun with --baker-write to write):");
    auto report = cfg.promote(line.layer, line.to);
    if (!report) {
        err(describe(report.error()));
        err("\n");
        return kCommandError;
    }
    report_refusals(*report);
    if (!report->ok()) return kCommandFailed;
    for (const WrittenLine& w : report->written)
        out("wrote " + Str(w.file.str()) + ":" + detail::decimal(w.line) + "\n");
    out("layer '" + Str(line.layer) + "' is unchanged: empty it once the promotion is reviewed\n");
    return 0;
}

// The output, sent where the command line or the host asked: the file --baker-output names, else
// the host's sink, else stdout, so an engine that owns stdout keeps the result apart.
int send(const CommandLine& line, const CommandHooks& hooks, const Str& output, int status) {
    if (!line.output.empty()) {
        if (!write_whole(line.output, output)) {
            err("cannot write " + Str(line.output) + "\n");
            return kCommandError;
        }
    } else if (hooks.output) {
        hooks.output(output.view());
    } else {
        std::fwrite(output.data(), 1, output.size(), stdout);
    }
    return status;
}

// sim, for a game that steps on demand: a SimRun driven with its reset and step.
int run_scenarios(const CommandLine& line, const CommandHooks& hooks) {
    if (!hooks.step) {
        err("sim: this game gives no step (CommandHooks::step), so it cannot run scenarios\n");
        return kCommandError;
    }
    SimRun run(line, hooks);
    for (SimRun::Wants w; (w = run.wants()) != SimRun::Wants::Done;) {
        if (w == SimRun::Wants::Reset) {
            if (hooks.reset) hooks.reset(run.context(), run.seed());
            run.ready();
        } else {
            hooks.step(run.context(), run.step_seconds());
            run.step();
        }
    }
    return run.status();
}

// Which objectives moved between two `sim --baker-results` directories, without running anything.
// Lays out files as Baker writes them (tools::format_text), with the project's layout.
int format(const CommandLine& line, const CommandHooks& hooks) {
    // The project that governs them: the one named, else the first file's, else here's.
    const std::string_view from = !line.project.empty() ? line.project
                                  : !line.rest.empty()  ? line.rest[0]
                                                        : std::string_view(".");
    WriteOptions layout = hooks.built ? hooks.built->format : WriteOptions{};
    const Symbol project = find_project(from, hooks.reader);
    if (!project.empty()) {
        auto p = read_project(project.str(), hooks.reader);
        if (!p) {
            err("format: " + Str(describe(p.error())) + "\n");
            return kCommandError;
        }
        layout = p->format;
    }
    Vec<Str> files;
    for (const std::string_view f : line.rest) files.emplace_back(f);
    if (files.empty()) {  // the stack's own files, under the project's directory
        Loaded l(hooks);
        if (!load(l, line)) return kCommandError;
        const std::string_view dir = detail::parent_path(l.live.project_file().str());
        for (const StackFile& f : l.live.config().stack_files())
            if (f.present && (dir.empty() || f.path.str().starts_with(dir)) &&
                std::none_of(files.begin(), files.end(),
                             [&](const Str& s) { return s.view() == f.path.str(); }))
                files.emplace_back(f.path.str());
    }
    int status = 0;
    Str json = R"({"schema": "baker.format/1", "files": [)";
    for (size_t i = 0; i < files.size(); ++i) {
        Str text, laid;
        if (!read_whole(files[i], text, hooks.reader)) {
            err("format: cannot read '" + files[i] + "'\n");
            return kCommandError;
        }
        const tools::Formatted r =
            tools::format_text(text.view(), detail::str_out(laid), {.layout = layout});
        for (const Error& e : r.errors) err("format: " + Str(describe(e)) + "\n");
        if (!r.errors.empty()) status = kCommandFailed;
        if (line.json) {
            json += i ? ",\n  {\"file\": " : "\n  {\"file\": ";
            detail::json_string(json, files[i].view());
            json += Str(", \"changed\": ") + (r.changed() ? "true" : "false") + ", \"line_end\": ";
            detail::json_string(json, r.line_end);
            json += ", \"edits\": [";
            for (size_t k = 0; k < r.edits.size(); ++k) {
                const tools::FormatEdit& e = r.edits[k];
                json += (k ? ", [" : "[") + detail::decimal(e.begin) + ", " +
                        detail::decimal(e.end) + ", " + detail::decimal(e.newlines) + ", " +
                        detail::decimal(e.spaces) + "]";
            }
            json += "], \"errors\": " + Str(r.errors.empty() ? "0" : "1") + "}";
        }
        if (!r.changed()) continue;
        if (line.check || line.json) {
            if (line.check) {
                if (!line.json) out(files[i] + "\n");
                status = kCommandFailed;
            }
        } else if (!write_whole(files[i].view(), laid.view())) {
            err("format: cannot write '" + files[i] + "'\n");
            return kCommandError;
        }
    }
    if (line.json) out(json + (files.empty() ? "]}\n" : "\n]}\n"));
    return status;
}

int compare(const CommandLine& line) {
    if (line.before.empty() || line.after.empty()) {
        err("compare needs --baker-before DIR --baker-after DIR, each written by sim "
            "--baker-results\n");
        return kCommandError;
    }
    Vec<sim::Run> before, after;
    Vec<sim::Median> medians_before, medians_after;
    for (const auto& [dir, runs, medians] : {std::tuple{line.before, &before, &medians_before},
                                             std::tuple{line.after, &after, &medians_after}}) {
        const Str file((fs::path(Str(dir).c_str()) / "results.dta").generic_string());
        if (auto r = sim::read_results(file.view(), *runs, *medians); !r) {
            err(describe(r.error()));
            err("\n");
            return kCommandError;
        }
    }
    bool broke = false;
    out(sim::compare_json(before, medians_before, after, medians_after, broke));
    return broke ? kCommandFailed : 0;
}

}  // namespace

// A sim in progress: the stack, the scenarios chosen, the run being stepped, and the output so
// far, which every call gathers while it runs.
struct SimRun::Impl {
    Impl(CommandLine l, const CommandHooks& h) : line(std::move(l)), loaded(h) {}
    CommandLine line;
    Loaded loaded;
    sim::Book book;
    Vec<const sim::Scenario*> chosen;
    size_t scenario = 0;  // into chosen
    uint64_t seed = 0, last = 0;
    std::optional<sim::Stepper> stepper;
    Vec<sim::Run> runs;
    Str output;
    Wants wants = Wants::Done;
    int status = kCommandError;

    // A line a person reads; the JSON has the rest.
    void print(const sim::Run& r) const {
        if (line.json) return;
        Str t = Str(r.scenario.str()) + " seed " + detail::decimal(static_cast<int64_t>(r.seed)) +
                ": " +
                (!r.finished  ? "STOPPED"
                 : r.passed() ? "passed"
                              : "FAILED");
        if (!r.ended.empty() && r.ended != Symbol("length"))  // the game's own end
            t += " (" +
                 Str(r.ended == Symbol("until") ? std::string_view("ended") : r.ended.str()) +
                 " at " +
                 Str(book.step_name.empty() ? std::string_view("step") : book.step_name.str()) +
                 " " + detail::decimal(static_cast<int64_t>(r.steps)) + ")";
        for (const sim::ObjectiveResult& o : r.objectives)
            t += "  " + Str(o.name.str()) + " " +
                 (o.failed ? "(" + Str(to_string(o.error)) + ")"
                           : sim::number_text(o.value) + (o.pass ? "" : " (fail)"));
        for (const Error& e : r.errors) t += "\n  error: " + Str(to_string(e));
        out(t + "\n");
    }
    void start() {
        stepper.emplace(loaded.ctx, book, *chosen[scenario], seed, line.step_hz);
        wants = Wants::Reset;
    }
    // The run has its last step: judged, then the next, or the end.
    void judge() {
        runs.push_back(stepper->judge());
        print(runs.back());
        if (seed < last) {
            ++seed;
        } else if (++scenario < chosen.size()) {
            seed = line.seed;
        } else {
            finish(false);
            return;
        }
        start();
    }
    void finish(bool stopped) {
        stepper.reset();
        const Vec<sim::Median> medians = sim::judge_medians(loaded.ctx, book, runs);
        if (!line.json)
            for (const sim::Median& m : medians)
                out(Str(m.scenario.str()) + " median of " + detail::decimal(m.seeds) +
                    " seeds: " + (m.pass ? "passed" : "FAILED") + "  " + Str(m.objective.str()) +
                    " " + sim::number_text(m.value) +
                    (m.failed ? "  " + Str(m.error.message()) : Str()) + "\n");
        const Str json = sim::results_json(runs, medians, book.step_name);
        if (line.json) out(json);
        status = stopped ? kCommandError : sim::all_passed(runs, medians) ? 0 : kCommandFailed;
        if (!line.results.empty()) {
            std::error_code ec;
            const fs::path dir(Str(line.results).c_str());
            fs::create_directories(dir, ec);
            if (!write_whole((dir / "results.json").generic_string(), json) ||
                !write_whole((dir / "results.dta").generic_string(),
                             sim::results_dta(runs, medians))) {
                err("sim: cannot write to " + Str(line.results) + "\n");
                status = kCommandError;
            }
        }
        done();
    }
    // The stack and the scenarios chosen; false, with the reason on stderr, if it cannot run.
    bool open() {
        if (line.command != "sim") {
            err("SimRun runs sim alone\n");
            return false;
        }
        if (!load(loaded, line)) return false;
        if (auto e = read_book(loaded, book); !e) {
            err(describe(e.error()));
            err("\n");
            return false;
        }
        // --baker-scenario picks by scenario name, or by its file's name without .dta.
        for (const sim::Scenario& s : book.scenarios) {
            const Str stem(fs::path(Str(s.file.str()).c_str()).stem().generic_string());
            bool wanted = line.scenarios.empty();
            for (const std::string_view n : line.scenarios)
                wanted = wanted || n == s.name.str() || n == stem.view();
            if (wanted) chosen.push_back(&s);
        }
        if (chosen.empty()) {
            err(book.scenarios.empty() ? "sim: the project names no scenarios (scenarios ...)\n"
                                       : "sim: no scenario by that name\n");
            return false;
        }
        seed = line.seed;
        last = line.seed_last ? line.seed_last : line.seed;
        return true;
    }
    // The end: the output sent, as run_command sends it.
    void done() {
        wants = Wants::Done;
        status = send(line, loaded.hooks, output, status);
    }
};

namespace {
// The output a SimRun call prints goes to its run, while the call lasts.
class Gather {
public:
    explicit Gather(Str& into) : outer_(t_output) { t_output = &into; }
    ~Gather() { t_output = outer_; }
    Gather(const Gather&) = delete;
    Gather& operator=(const Gather&) = delete;

private:
    Str* outer_;
};
}  // namespace

SimRun::SimRun(const CommandLine& line, const CommandHooks& hooks) : impl_(new Impl(line, hooks)) {
    const Gather gather(impl_->output);
    if (impl_->open())
        impl_->start();
    else
        impl_->done();
}

SimRun::~SimRun() {
    delete impl_;
}

SimRun::Wants SimRun::wants() const noexcept {
    return impl_->wants;
}
uint64_t SimRun::seed() const noexcept {
    return impl_->seed;
}
double SimRun::step_seconds() const noexcept {
    return impl_->stepper ? impl_->stepper->dt() : 0.0;
}
Context& SimRun::context() noexcept {
    return impl_->loaded.ctx;
}
int SimRun::status() const noexcept {
    return impl_->status;
}

void SimRun::ready() {
    Impl& r = *impl_;
    if (r.wants != Wants::Reset) return;
    const Gather gather(r.output);
    r.stepper->begin();
    r.wants = Wants::Steps;
    if (r.stepper->done()) r.judge();
}

void SimRun::step() {
    Impl& r = *impl_;
    if (r.wants != Wants::Steps) return;
    const Gather gather(r.output);
    r.stepper->advance();
    if (r.stepper->done()) r.judge();
}

void SimRun::stop() {
    Impl& r = *impl_;
    if (r.wants == Wants::Done) return;
    const Gather gather(r.output);
    if (r.wants == Wants::Steps) {  // a run waiting for its reset has not started
        r.runs.push_back(r.stepper->stop());
        r.print(r.runs.back());
    }
    r.finish(true);
}

namespace {
// Where an option that takes text keeps it, or null; `a` is its name after --baker-.
std::string_view* text_option(CommandLine& line, std::string_view a) {
    const std::pair<std::string_view, std::string_view*> options[] = {
        {"project", &line.project}, {"layer", &line.layer},   {"from", &line.layer},
        {"to", &line.to},           {"patch", &line.patch},   {"output", &line.output},
        {"results", &line.results}, {"before", &line.before}, {"after", &line.after}};
    for (const auto& [name, into] : options)
        if (a == name) return into;
    return nullptr;
}
}  // namespace

Expected<CommandLine> parse_command_line(int argc, const char* const* argv) {
    CommandLine line;
    if (argc > 1) line.command = argv[1];
    Vec<std::string_view> commands, rest, scenarios;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        // The kit's flags begin --baker-; everything else is the host's, or a command.
        constexpr std::string_view kPrefix = "--baker-";
        if (!arg.starts_with(kPrefix)) {
            (!arg.empty() && arg.front() == '{' ? commands : rest).push_back(arg);
            continue;
        }
        const std::string_view a = arg.substr(kPrefix.size());
        auto value = [&](std::string_view& into) -> bool {
            if (i + 1 >= argc) return false;
            into = argv[++i];
            return true;
        };
        auto missing = [&] {
            return Unexpected(make_error(ErrorCode::Syntax, Str(arg) + " needs a value"));
        };
        if (a == "json") {
            line.json = true;
        } else if (a == "write") {
            line.write = true;
        } else if (a == "check") {
            line.check = true;
        } else if (std::string_view* into = text_option(line, a)) {
            if (!value(*into)) return missing();
        } else if (a == "seed" || a == "step-hz") {
            std::string_view s;
            if (!value(s)) return missing();
            const auto n = whole(s);
            if (!n || (a == "step-hz" && *n == 0))
                return Unexpected(make_error(ErrorCode::Syntax, Str(arg) + " needs a number"));
            if (a == "seed")
                line.seed = *n;
            else
                line.step_hz = static_cast<double>(*n);
        } else if (a == "seeds") {
            std::string_view s;
            if (!value(s)) return missing();
            const size_t dash = s.find('-');
            const auto first = whole(s.substr(0, dash));
            const auto last = dash == std::string_view::npos ? first : whole(s.substr(dash + 1));
            if (!first || !last || *last < *first)
                return Unexpected(make_error(ErrorCode::Syntax, "--baker-seeds needs A-B, A <= B"));
            line.seed = *first;
            line.seed_last = *last;
        } else if (a == "scenario") {
            std::string_view s;
            if (!value(s)) return missing();
            scenarios.push_back(s);
        } else {  // a misspelled flag of the kit's is an error, never the host's
            return Unexpected(make_error(ErrorCode::Syntax, "unknown flag " + Str(arg)));
        }
    }
    line.commands =
        List<std::string_view>(std::span<const std::string_view>(commands.data(), commands.size()));
    line.rest = List<std::string_view>(std::span<const std::string_view>(rest.data(), rest.size()));
    line.scenarios = List<std::string_view>(
        std::span<const std::string_view>(scenarios.data(), scenarios.size()));
    return line;
}

namespace {

int dispatch(const CommandLine& line, const CommandHooks& hooks) {
    if (line.command == "check") return check(line, hooks);
    if (line.command == "reference") return reference(line, hooks);
    if (line.command == "eval") return eval(line, hooks);
    if (line.command == "compare") return compare(line);
    if (line.command == "format") return format(line, hooks);
    if (line.command == "diff" || line.command == "promote") {
        if constexpr (kShipping) {
            err(Str(line.command) + ": not in a shipping build\n");
            return kCommandError;
        } else {
            return line.command == "diff" ? diff(line, hooks) : promote(line, hooks);
        }
    }
    return -1;
}

}  // namespace

int run_command(const CommandLine& line, const CommandHooks& hooks) {
    if (line.command == "sim") return run_scenarios(line, hooks);  // a SimRun sends its own
    Str output;
    int status = 0;
    {
        const Gather gather(output);
        status = dispatch(line, hooks);
    }
    return status < 0 ? status : send(line, hooks, output, status);
}

std::string_view command_help() noexcept {
    return "The kit's flags begin --baker-; any other flag is the game's.\n"
           "  check                          baker.check/1.4 JSON; exit 1 on an error, a load\n"
           "                                 problem, a shape error, or an undefined function\n"
           "  reference [--baker-json]       the script vocabulary, as Markdown; --baker-json:\n"
           "                                 {reference, tweaks, shapes} as Baker's JSON\n"
           "  diff [--baker-layer L]         what layer L (default agent) changes\n"
           "  promote [--baker-from L] [--baker-to T] [--baker-write | --baker-patch FILE]\n"
           "                                 layer L into T (default default); --baker-write\n"
           "                                 writes; --baker-patch writes the change as a\n"
           "                                 unified diff\n"
           "  eval [--baker-seed N] [--baker-json] \"{command}\"...\n"
           "                                 each command's result, one a line, on the game\n"
           "                                 reset with the seed (default 1); --baker-json: one\n"
           "                                 baker.reply/1.1 line each, as the console answers\n"
           "  sim [--baker-scenario NAME]... [--baker-seed N | --baker-seeds A-B]\n"
           "      [--baker-step-hz N] [--baker-json] [--baker-results DIR]\n"
           "                                 the project's scenarios, judged by its objectives;\n"
           "                                 --baker-json: baker.sim/1.3; --baker-results: what\n"
           "                                 compare reads\n"
           "  compare --baker-before DIR --baker-after DIR\n"
           "                                 which objectives moved: baker.compare/1\n"
           "  format [--baker-check] [--baker-json] [FILE...]\n"
           "                                 lays out the files (no FILE: the stack's own) as\n"
           "                                 Baker writes them; --baker-check: exit 1, listing\n"
           "                                 each that would change\n"
           "every command also takes --baker-output FILE: its output there, not on stdout\n";
}

}  // namespace baker::host
