// The tool commands (host/commands.hpp), run from a game's own binary.

#include <baker/host/commands.hpp>
#include <baker/host/live.hpp>
#include <baker/tweak.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#if BAKER_HOST_SHAPE
#include <baker/shape.hpp>
#endif

namespace fs = std::filesystem;

namespace baker::host {
namespace {

void out(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
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
    o.open = l.hooks.open;
    o.watch = false;
    o.changed = l.hooks.changed;
    o.log = [](const Error& e) {
        err(describe(e));
        err("\n");
    };
    return static_cast<bool>(l.live.open(o));
}

int report_check(CheckReport& report, const Loaded& l, const LoadSession* packs) {
    report.load = List<Error>(l.live.load_errors());
    write_json(report, stdout_sink(), packs);
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
    List<Ref<Array>> trees;
    if (hooks.extra_trees) {
        auto t = hooks.extra_trees(cfg);
        if (!t) {
            report.load = List<Error>(std::span<const Error>(&t.error(), 1));
            write_json(report, stdout_sink(), &cfg.read_session());
            out("\n");
            err(describe(t.error()));
            err("\n");
            return kCommandError;
        }
        trees = std::move(*t);
    }
    Vec<const Array*> extra;
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
        baker::reference(l.ctx, stdout_sink(), &cfg.read_session());
        return 0;
    }
    // The three documents in one object: the vocabulary, the tweaks, and the shapes the
    // enabled layers declare.
    Str ref, tweaks, shapes = "null";
    write_reference_json(l.ctx, detail::str_out(ref), &cfg.read_session());
    write_json(TweakRegistry::global(), detail::str_out(tweaks));
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
    auto effect = cfg.layer_effect(layer);
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

bool read_whole(const Str& path, Str& text) {
    text.clear();
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
        if (report->ok())  // all or nothing, as --write is
            for (const auto& [path, text] : captured) {
                Str before;
                (void)read_whole(path, before);
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
        return print_effect(cfg, line.layer,
                            "would write into " + Str(line.to) + " (rerun with --write to write):");
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

}  // namespace

Expected<CommandLine> parse_command_line(int argc, const char* const* argv) {
    CommandLine line;
    if (argc > 1) line.command = argv[1];
    Vec<std::string_view> commands, rest;
    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&](std::string_view& into) -> bool {
            if (i + 1 >= argc) return false;
            into = argv[++i];
            return true;
        };
        auto missing = [&] {
            return Unexpected(make_error(ErrorCode::Syntax, Str(a) + " needs a value"));
        };
        if (a == "--json") {
            line.json = true;
        } else if (a == "--write") {
            line.write = true;
        } else if (a == "--project") {
            if (!value(line.project)) return missing();
        } else if (a == "--layer" || a == "--from") {
            if (!value(line.layer)) return missing();
        } else if (a == "--to") {
            if (!value(line.to)) return missing();
        } else if (a == "--patch") {
            if (!value(line.patch)) return missing();
        } else if (a == "--seed") {
            std::string_view s;
            if (!value(s)) return missing();
            uint64_t n = 0;
            if (s.empty())
                return Unexpected(make_error(ErrorCode::Syntax, "--seed needs a number"));
            for (const char c : s) {
                if (c < '0' || c > '9')
                    return Unexpected(make_error(ErrorCode::Syntax, "--seed needs a number"));
                n = n * 10 + static_cast<uint64_t>(c - '0');
            }
            line.seed = n;
        } else if (!a.empty() && a.front() == '{') {
            commands.push_back(a);
        } else {
            rest.push_back(a);
        }
    }
    line.commands =
        List<std::string_view>(std::span<const std::string_view>(commands.data(), commands.size()));
    line.rest = List<std::string_view>(std::span<const std::string_view>(rest.data(), rest.size()));
    return line;
}

int run_command(const CommandLine& line, const CommandHooks& hooks) {
    if (line.command == "check") return check(line, hooks);
    if (line.command == "reference") return reference(line, hooks);
    if (line.command == "eval") return eval(line, hooks);
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

std::string_view command_help() noexcept {
    return "  check                          baker.check/1.2 JSON; exit 1 on an error, a load\n"
           "                                 problem, a shape error, or an undefined function\n"
           "  reference [--json]             the script vocabulary, as Markdown; --json:\n"
           "                                 {reference, tweaks, shapes} as Baker's JSON\n"
           "  diff [--layer L]               what layer L (default agent) changes\n"
           "  promote [--from L] [--to T] [--write | --patch FILE]\n"
           "                                 layer L into T (default default); --write writes;\n"
           "                                 --patch writes the change as a unified diff\n"
           "  eval [--seed N] [--json] \"{command}\"...\n"
           "                                 each command's result, one a line, on the game\n"
           "                                 reset with the seed (default 1); --json: one\n"
           "                                 baker.reply/1.1 line each, as the console answers\n";
}

}  // namespace baker::host
