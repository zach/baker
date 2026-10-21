// The host kit's scenario runner (scenarios.hpp).

#include "host/scenarios.hpp"

#include <baker/task.hpp>

#include "data/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace baker::host::sim {
namespace {

Error at(const Array& form, ErrorCode code, std::string_view message) {
    return make_error(code, message, form.file(), form.line());
}

void commands(const Array& form, size_t from, Vec<Node>& out) {
    for (size_t i = from; i < form.size(); ++i)
        if (form[i].type() == Type::Command) out.push_back(form[i]);
}

std::string_view text(const Array& form, std::string_view tag) {
    if (const Array* a = form.try_find(Symbol(tag)); a && a->size() > 1)
        if (const auto s = (*a)[1].try_str()) return *s;
    return {};
}

// The form's name: its second value, a symbol. (objective NAME ...), (scenario NAME ...)
Symbol name_of(const Array& form) {
    return form.size() > 1 ? form[1].try_sym().value_or(Symbol()) : Symbol();
}

Expected<void> read_forms(const Array& tree, Book& book) {
    for (const Array& form : tree.subarrays()) {
        if (form.empty()) continue;
        if (form[0].is(Symbol("objective"))) {
            Objective o{.name = name_of(form), .file = form.file(), .line = form.line()};
            const Array* measure = form.try_find(Symbol("measure"));
            const Array* pass = form.try_find(Symbol("pass"));
            if (o.name.empty() || !measure || measure->size() < 2 || !pass || pass->size() < 2)
                return Unexpected(at(form, ErrorCode::Syntax,
                                     "an objective needs a name, (measure {command}), and "
                                     "(pass {command})"));
            o.desc = text(form, "desc");
            o.measure = (*measure)[1];
            o.pass = (*pass)[1];
            if (const Array* over = form.try_find(Symbol("over"))) {
                if (over->size() != 2 || !(*over)[1].is(Symbol("median")))
                    return Unexpected(at(*over, ErrorCode::Syntax,
                                         "(over median) is the one form of (over ...)"));
                o.over_median = true;
            }
            book.objectives.push_back(std::move(o));
        } else if (form[0].is(Symbol("scenario"))) {
            Scenario s{.name = name_of(form), .file = form.file(), .line = form.line()};
            if (s.name.empty())
                return Unexpected(
                    at(form, ErrorCode::Syntax, "a scenario needs a name: (scenario NAME ...)"));
            s.desc = text(form, "desc");
            const Array* sec = form.try_find(Symbol("seconds"));
            const auto seconds = sec && sec->size() > 1 ? (*sec)[1].try_float() : std::nullopt;
            if (!seconds || *seconds <= 0)
                return Unexpected(
                    at(form, ErrorCode::Syntax,
                       "scenario '" + Str(s.name.str()) + "' needs (seconds N), N above 0"));
            s.seconds = *seconds;
            for (const Array& part : form.subarrays()) {
                if (part.empty()) continue;
                if (part[0].is(Symbol("start"))) {
                    commands(part, 1, s.start);
                } else if (part[0].is(Symbol("at"))) {
                    const auto when = part.size() > 1 ? part[1].try_float() : std::nullopt;
                    if (!when)
                        return Unexpected(
                            at(part, ErrorCode::Syntax,
                               "scenario '" + Str(s.name.str()) + "': (at SECONDS {command}...)"));
                    Scenario::At a{.seconds = *when};
                    commands(part, 2, a.commands);
                    s.at.push_back(std::move(a));
                } else if (part[0].is(Symbol("objectives"))) {
                    for (size_t i = 1; i < part.size(); ++i)
                        if (const auto sym = part[i].try_sym()) s.objectives.push_back(*sym);
                }
            }
            std::stable_sort(s.at.begin(), s.at.end(),
                             [](const auto& a, const auto& b) { return a.seconds < b.seconds; });
            book.scenarios.push_back(std::move(s));
        }
    }
    return {};
}

// A number from a command's result, or the reason it is not one, located at `where`.
bool number_of(const Expected<Node>& r, double& out, Error& error, const char* what,
               const Objective& where) {
    if (!r) {
        error = r.error();
        return false;
    }
    if (const auto v = r->try_float()) {
        out = *v;
        return true;
    }
    error = make_error(ErrorCode::Type, Str(what) + " is not a number: " + detail::text_of(*r),
                       where.file, where.line);
    return false;
}

const Objective* objective_named(const Book& book, Symbol name) {
    for (const Objective& o : book.objectives)
        if (o.name == name) return &o;
    return nullptr;
}

}  // namespace

Str number_text(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return Str(buf);
}
namespace {

Str json_num(double v) {
    return std::isfinite(v) ? number_text(v) : Str("null");
}

// A problem, as check's JSON writes one.
void json_problem(Str& out, const Error& e) {
    out += "{\"code\": ";
    detail::json_string(out, error_code_name(e.code()));
    out += ", \"file\": ";
    detail::json_string(out, e.file().str());
    out += ", \"line\": " + detail::decimal(e.line()) +
           ", \"column\": " + detail::decimal(e.column()) + ", \"path\": ";
    detail::json_string(out, e.path());
    out += ", \"message\": ";
    detail::json_string(out, e.message());
    out += '}';
}

// ", "error": " and the problem, or null.
void json_error(Str& out, bool failed, const Error& e) {
    out += ", \"error\": ";
    if (failed)
        json_problem(out, e);
    else
        out += "null";
}
const char* json_bool(bool b) {
    return b ? "true" : "false";
}

void json_place(Str& out, Symbol file, int32_t line) {
    out += ", \"file\": ";
    detail::json_string(out, file.str());
    out += ", \"line\": " + detail::decimal(line);
}

Str quoted(std::string_view s);
// Where a run or an objective is declared, in the results' data.
Str place_dta(Symbol file, int32_t line) {
    return " (file " + quoted(file.str()) + ") (line " + detail::decimal(line) + ")";
}

bool by_text(Symbol a, Symbol b) {
    return a.str() < b.str();
}

// One objective's results over runs: passes, fails, and its finite values, sorted.
struct Tally {
    int passed = 0, failed = 0;
    Vec<double> values;
    [[nodiscard]] double median() const { return values[values.size() / 2]; }
    void json(Str& out) const {
        out +=
            "{\"passed\": " + detail::decimal(passed) + ", \"failed\": " + detail::decimal(failed);
        if (values.empty()) {
            out += R"(, "min": null, "median": null, "max": null})";
            return;
        }
        out += ", \"min\": " + json_num(values.front()) + ", \"median\": " + json_num(median()) +
               ", \"max\": " + json_num(values.back()) + "}";
    }
};

// The objectives of `runs` by name, each with its tally and where it is declared.
struct Tallied {
    Symbol name, file;
    int32_t line = 0;
    Tally tally;
};
Vec<Tallied> tallies(const Vec<Run>& runs) {
    Vec<Tallied> by;
    for (const Run& r : runs)
        for (const ObjectiveResult& o : r.objectives) {
            auto it = std::find_if(by.begin(), by.end(),
                                   [&](const Tallied& t) { return t.name == o.name; });
            if (it == by.end()) {
                by.push_back({o.name, o.file, o.line, {}});
                it = by.end() - 1;
            }
            (o.pass ? it->tally.passed : it->tally.failed) += 1;
            if (!o.failed && std::isfinite(o.value)) it->tally.values.push_back(o.value);
        }
    for (Tallied& t : by) std::sort(t.tally.values.begin(), t.tally.values.end());
    std::sort(by.begin(), by.end(),
              [](const Tallied& a, const Tallied& b) { return by_text(a.name, b.name); });
    return by;
}

// A string in the data language: \q for a double quote, \n for a newline, and a backslash
// written as a slash, so a Windows path cannot form either escape.
Str quoted(std::string_view s) {
    Str out = "\"";
    for (const char c : s) {
        if (c == '"')
            out += "\\q";
        else if (c == '\n')
            out += "\\n";
        else
            out += c == '\\' ? '/' : c;
    }
    return out + "\"";
}

}  // namespace

Expected<void> Book::read(std::string_view path, const LoadSession::Reader& reader,
                          const Lister& list) {
    std::error_code ec;
    Vec<Str> files;
    const fs::path p(Str(path).c_str());
    if (list && list(path, [&](std::string_view f) {
            if (f.size() > 4 && f.substr(f.size() - 4) == ".dta") files.emplace_back(f);
        })) {
        std::sort(files.begin(), files.end());
    } else if (fs::is_directory(p, ec)) {
        for (const auto& entry : fs::directory_iterator(p, ec))
            if (entry.path().extension() == ".dta")
                files.emplace_back(entry.path().generic_string());
        std::sort(files.begin(), files.end());
    } else if (reader || fs::exists(p, ec)) {
        files.emplace_back(path);
    }
    for (const Str& f : files) {
        LoadSession session;
        if (reader) session.set_reader(reader);
        auto tree = read_file(f.view(), session);
        if (!tree) {
            if (reader && tree.error().code() == ErrorCode::Io) continue;  // the reader has none
            return Unexpected(std::move(tree).error());
        }
        if (auto r = read_forms(**tree, *this); !r) return r;
        trees.push_back(std::move(*tree));
    }
    return {};
}

bool Run::passed() const noexcept {
    if (!finished || !errors.empty()) return false;
    for (const ObjectiveResult& o : objectives)
        if (!o.pass && (!o.over_median || o.failed)) return false;
    return true;
}

Stepper::Stepper(Context& ctx, const Book& book, const Scenario& scenario, uint64_t seed,
                 double step_hz)
    : ctx_(&ctx),
      book_(&book),
      scenario_(&scenario),
      run_{.scenario = scenario.name, .file = scenario.file, .line = scenario.line, .seed = seed} {
    // Steps are counted, not timed: the same scenario takes the same steps everywhere.
    const auto step_ns = static_cast<uint64_t>(std::llround(1e9 / step_hz));
    dt_ = static_cast<double>(step_ns) / 1e9;
    total_ = static_cast<uint64_t>(std::llround(scenario.seconds * step_hz));
    ctx.var("seed") = Node(static_cast<int64_t>(seed));
}

void Stepper::begin() {
    for (const Node& c : scenario_->start)
        if (auto r = ctx_->eval(c); !r) run_.errors.push_back(r.error());
    run_due();
}

// Each (at ...) whose time has come, at the step about to be taken.
void Stepper::run_due() {
    const double now = static_cast<double>(run_.steps) * dt_;
    const Vec<Scenario::At>& at = scenario_->at;
    while (next_at_ < at.size() && at[next_at_].seconds <= now + dt_ * 0.5)
        for (const Node& c : at[next_at_++].commands)
            if (auto r = ctx_->eval(c); !r) run_.errors.push_back(r.error());
}

void Stepper::advance() {
    ctx_->scheduler().tick(dt_);
    ++run_.steps;
    if (!done()) run_due();
}

Run Stepper::stop() {
    run_.finished = false;
    return std::move(run_);
}

Run Stepper::judge() {
    Context& ctx = *ctx_;
    const Scenario& scenario = *scenario_;
    for (const Objective& o : book_->objectives) {
        if (!scenario.objectives.empty() &&
            std::find(scenario.objectives.begin(), scenario.objectives.end(), o.name) ==
                scenario.objectives.end())
            continue;
        ObjectiveResult r{
            .name = o.name, .file = o.file, .line = o.line, .over_median = o.over_median};
        if (number_of(ctx.eval(o.measure), r.value, r.error, "the measure", o)) {
            ctx.var("value") = Node(r.value);
            double p = 0;
            if (number_of(ctx.eval(o.pass), p, r.error, "the pass", o))
                r.pass = p != 0;
            else
                r.failed = true;
        } else {
            r.failed = true;
        }
        run_.objectives.push_back(std::move(r));
    }
    for (const Symbol name : scenario.objectives)
        if (!objective_named(*book_, name))
            run_.errors.push_back(make_error(ErrorCode::NotFound,
                                             "scenario '" + Str(scenario.name.str()) +
                                                 "' names objective '" + Str(name.str()) +
                                                 "', which no objective file declares",
                                             scenario.file, scenario.line));
    return std::move(run_);
}

Run simulate(Context& ctx, const Game& game, const Book& book, const Scenario& scenario,
             uint64_t seed, double step_hz) {
    Stepper s(ctx, book, scenario, seed, step_hz);
    if (game.reset) game.reset(ctx, seed);
    s.begin();
    while (!s.done()) {
        if (game.step) game.step(ctx, s.dt());
        s.advance();
    }
    return s.judge();
}

Vec<Median> judge_medians(Context& ctx, const Book& book, const Vec<Run>& runs) {
    // Each (scenario, objective) judged by its median, with its values.
    struct Key {
        Symbol scenario, objective;
        Vec<double> values;
    };
    Vec<Key> keys;
    for (const Run& r : runs)
        for (const ObjectiveResult& o : r.objectives) {
            if (!o.over_median) continue;
            auto it = std::find_if(keys.begin(), keys.end(), [&](const Key& k) {
                return k.scenario == r.scenario && k.objective == o.name;
            });
            if (it == keys.end()) {
                keys.push_back({r.scenario, o.name, {}});
                it = keys.end() - 1;
            }
            if (!o.failed && std::isfinite(o.value)) it->values.push_back(o.value);
        }
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        return a.scenario != b.scenario ? by_text(a.scenario, b.scenario)
                                        : by_text(a.objective, b.objective);
    });
    Vec<Median> out;
    for (Key& k : keys) {
        const Objective* o = objective_named(book, k.objective);
        Median m{.scenario = k.scenario,
                 .objective = k.objective,
                 .file = o ? o->file : Symbol(),
                 .line = o ? o->line : 0,
                 .seeds = static_cast<int>(k.values.size())};
        if (k.values.empty() || !o) {
            m.failed = true;
            m.error = make_error(ErrorCode::NotFound, "no value to judge", m.file, m.line);
        } else {
            std::sort(k.values.begin(), k.values.end());
            m.value = k.values[k.values.size() / 2];  // the upper median
            ctx.var("value") = Node(m.value);
            double p = 0;
            if (number_of(ctx.eval(o->pass), p, m.error, "the pass", *o))
                m.pass = p != 0;
            else
                m.failed = true;
        }
        out.push_back(std::move(m));
    }
    return out;
}

bool all_passed(const Vec<Run>& runs, const Vec<Median>& medians) {
    return std::all_of(runs.begin(), runs.end(), [](const Run& r) { return r.passed(); }) &&
           std::all_of(medians.begin(), medians.end(), [](const Median& m) { return m.pass; });
}

Str results_json(Vec<Run> runs, const Vec<Median>& medians) {
    for (Run& r : runs)
        std::sort(r.objectives.begin(), r.objectives.end(),
                  [](const auto& a, const auto& b) { return by_text(a.name, b.name); });
    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
        return a.scenario != b.scenario ? by_text(a.scenario, b.scenario) : a.seed < b.seed;
    });
    const bool stopped =
        std::any_of(runs.begin(), runs.end(), [](const Run& r) { return !r.finished; });
    Str s = "{\n  \"schema\": \"baker.sim/1.1\",\n  \"status\": ";
    s += stopped ? "\"stopped\"" : all_passed(runs, medians) ? "\"passed\"" : "\"failed\"";
    s += ",\n  \"runs\": [";
    for (size_t i = 0; i < runs.size(); ++i) {
        const Run& r = runs[i];
        s += i ? ",\n    {\"scenario\": " : "\n    {\"scenario\": ";
        detail::json_string(s, r.scenario.str());
        json_place(s, r.file, r.line);
        s += ", \"seed\": " + detail::decimal(static_cast<int64_t>(r.seed)) +
             ", \"steps\": " + detail::decimal(static_cast<int64_t>(r.steps)) +
             ", \"finished\": " + json_bool(r.finished) + ", \"passed\": " + json_bool(r.passed()) +
             ", \"errors\": [";
        for (size_t e = 0; e < r.errors.size(); ++e) {
            if (e) s += ", ";
            json_problem(s, r.errors[e]);
        }
        s += "], \"objectives\": [";
        for (size_t j = 0; j < r.objectives.size(); ++j) {
            const ObjectiveResult& o = r.objectives[j];
            s += j ? ", {\"name\": " : "{\"name\": ";
            detail::json_string(s, o.name.str());
            json_place(s, o.file, o.line);
            s += ", \"value\": " + (o.failed ? Str("null") : json_num(o.value)) +
                 ", \"pass\": " + json_bool(o.pass) +
                 ", \"over\": " + (o.over_median ? "\"median\"" : "\"seed\"");
            json_error(s, o.failed, o.error);
            s += '}';
        }
        s += "]}";
    }
    s += runs.empty() ? "],\n" : "\n  ],\n";
    s += "  \"medians\": [";
    for (size_t i = 0; i < medians.size(); ++i) {
        const Median& m = medians[i];
        s += i ? ",\n    {\"scenario\": " : "\n    {\"scenario\": ";
        detail::json_string(s, m.scenario.str());
        s += ", \"objective\": ";
        detail::json_string(s, m.objective.str());
        json_place(s, m.file, m.line);
        s += ", \"value\": " + (m.failed ? Str("null") : json_num(m.value)) +
             ", \"seeds\": " + detail::decimal(m.seeds) + ", \"pass\": " + json_bool(m.pass);
        json_error(s, m.failed, m.error);
        s += '}';
    }
    s += medians.empty() ? "],\n" : "\n  ],\n";
    s += "  \"summary\": [";
    const Vec<Tallied> by = tallies(runs);
    for (size_t k = 0; k < by.size(); ++k) {
        s += k ? ",\n    {\"objective\": " : "\n    {\"objective\": ";
        detail::json_string(s, by[k].name.str());
        json_place(s, by[k].file, by[k].line);
        s += ", \"tally\": ";
        by[k].tally.json(s);
        s += '}';
    }
    s += by.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return s;
}

Str results_dta(const Vec<Run>& runs, const Vec<Median>& medians) {
    Str s = "; baker sim results: what compare reads\n";
    for (const Run& r : runs) {
        s += "(run (scenario " + Str(r.scenario.str()) + ") (seed " +
             detail::decimal(static_cast<int64_t>(r.seed)) + ") (steps " +
             detail::decimal(static_cast<int64_t>(r.steps)) + ") (passed " +
             (r.passed() ? "1" : "0") + ")" + (r.finished ? "" : " (finished 0)") +
             place_dta(r.file, r.line);
        for (const ObjectiveResult& o : r.objectives) {
            s += "\n   (objective " + Str(o.name.str()) + " (value " + number_text(o.value) +
                 ") (pass " + (o.pass ? "1" : "0") + ")" + place_dta(o.file, o.line);
            if (o.over_median) s += " (over median)";
            if (o.failed) s += " (error " + quoted(o.error.message()) + ")";
            s += ")";
        }
        for (const Error& e : r.errors) s += "\n   (error " + quoted(e.message()) + ")";
        s += ")\n";
    }
    for (const Median& m : medians) {
        s += "(median " + Str(m.objective.str()) + " (scenario " + Str(m.scenario.str()) +
             ") (value " + number_text(m.value) + ") (seeds " + detail::decimal(m.seeds) +
             ") (pass " + (m.pass ? "1" : "0") + ")";
        if (m.failed) s += " (error " + quoted(m.error.message()) + ")";
        s += ")\n";
    }
    return s;
}

Expected<void> read_results(std::string_view file, Vec<Run>& runs, Vec<Median>& medians) {
    auto tree = read_file(file);
    if (!tree) return Unexpected(std::move(tree).error());
    auto number = [](const Array* a) {
        return a && a->size() > 1 ? (*a)[1].try_float().value_or(0.0) : 0.0;
    };
    auto sym = [](const Array* a) {
        return a && a->size() > 1 ? (*a)[1].try_sym().value_or(Symbol()) : Symbol();
    };
    auto place = [](const Array& form) {
        const Array* a = form.try_find(Symbol("file"));
        return a && a->size() > 1 ? Symbol((*a)[1].try_str().value_or("")) : Symbol();
    };
    for (const Array& form : (*tree)->subarrays()) {
        if (form.empty()) continue;
        if (form[0].is(Symbol("run"))) {
            Run r{.scenario = sym(form.try_find(Symbol("scenario"))),
                  .file = place(form),
                  .line = static_cast<int32_t>(number(form.try_find(Symbol("line")))),
                  .seed = static_cast<uint64_t>(number(form.try_find(Symbol("seed")))),
                  .steps = static_cast<uint64_t>(number(form.try_find(Symbol("steps")))),
                  .finished = !form.try_find(Symbol("finished")) ||
                              number(form.try_find(Symbol("finished"))) != 0};
            for (const Array& part : form.subarrays()) {
                if (part.empty()) continue;
                if (part[0].is(Symbol("error")) && part.size() > 1) {
                    r.errors.push_back(
                        make_error(ErrorCode::Refused, part[1].try_str().value_or("")));
                } else if (part[0].is(Symbol("objective")) && part.size() > 1) {
                    ObjectiveResult o{
                        .name = part[1].try_sym().value_or(Symbol()),
                        .file = place(part),
                        .line = static_cast<int32_t>(number(part.try_find(Symbol("line"))))};
                    o.value = number(part.try_find(Symbol("value")));
                    o.pass = number(part.try_find(Symbol("pass"))) != 0;
                    o.over_median = part.try_find(Symbol("over")) != nullptr;
                    if (const Array* e = part.try_find(Symbol("error")); e && e->size() > 1) {
                        o.failed = true;
                        o.error = make_error(ErrorCode::Refused, (*e)[1].try_str().value_or(""));
                    }
                    r.objectives.push_back(std::move(o));
                }
            }
            runs.push_back(std::move(r));
        } else if (form[0].is(Symbol("median")) && form.size() > 1) {
            Median m{.scenario = sym(form.try_find(Symbol("scenario"))),
                     .objective = form[1].try_sym().value_or(Symbol())};
            m.value = number(form.try_find(Symbol("value")));
            m.seeds = static_cast<int>(number(form.try_find(Symbol("seeds"))));
            m.pass = number(form.try_find(Symbol("pass"))) != 0;
            m.failed = form.try_find(Symbol("error")) != nullptr;
            medians.push_back(std::move(m));
        }
    }
    return {};
}

Str compare_json(const Vec<Run>& before, const Vec<Median>& medians_before, const Vec<Run>& after,
                 const Vec<Median>& medians_after, bool& broke) {
    const Vec<Tallied> tb = tallies(before), ta = tallies(after);
    Vec<Symbol> names;
    for (const Vec<Tallied>* side : {&tb, &ta})
        for (const Tallied& t : *side)
            if (std::find(names.begin(), names.end(), t.name) == names.end())
                names.push_back(t.name);
    std::sort(names.begin(), names.end(), by_text);
    auto find = [](const Vec<Tallied>& v, Symbol n) {
        const auto it =
            std::find_if(v.begin(), v.end(), [&](const Tallied& t) { return t.name == n; });
        return it == v.end() ? nullptr : &*it;
    };
    auto judged_by_median = [](const Vec<Median>& ms, Symbol n) {
        return std::any_of(ms.begin(), ms.end(), [&](const Median& m) { return m.objective == n; });
    };

    Str objectives;
    int broken = 0, fixed_count = 0;
    for (size_t k = 0; k < names.size(); ++k) {
        const Symbol name = names[k];
        const Tallied* b = find(tb, name);
        const Tallied* a = find(ta, name);
        Str changed;
        bool any_broke = false, any_fixed = false;
        // A run, or a median ("over": "median"), whose pass moved.
        auto moved_pass = [&](Symbol scenario, const Str& which, bool was, bool now) {
            (now ? any_fixed : any_broke) = true;
            changed += changed.empty() ? "{\"scenario\": " : ", {\"scenario\": ";
            detail::json_string(changed, scenario.str());
            changed +=
                which + ", \"before\": " + json_bool(was) + ", \"after\": " + json_bool(now) + "}";
        };
        if (b && a &&
            (judged_by_median(medians_before, name) || judged_by_median(medians_after, name))) {
            // Judged by its median: a seed's pass moving is not a change; its verdict is.
            for (const Median& now : medians_after) {
                if (now.objective != name) continue;
                const auto was = std::find_if(
                    medians_before.begin(), medians_before.end(), [&](const Median& m) {
                        return m.objective == name && m.scenario == now.scenario;
                    });
                if (was != medians_before.end() && was->pass != now.pass)
                    moved_pass(now.scenario, R"(, "over": "median")", was->pass, now.pass);
            }
        } else if (b && a) {
            for (const Run& r : after)
                for (const ObjectiveResult& now : r.objectives) {
                    if (now.name != name) continue;
                    const ObjectiveResult* was = nullptr;
                    for (const Run& p : before)
                        if (p.scenario == r.scenario && p.seed == r.seed)
                            for (const ObjectiveResult& o : p.objectives)
                                if (o.name == name) was = &o;
                    if (was && was->pass != now.pass)
                        moved_pass(r.scenario,
                                   ", \"seed\": " + detail::decimal(static_cast<int64_t>(r.seed)),
                                   was->pass, now.pass);
                }
        }
        const char* moved = !b          ? "new"
                            : !a        ? "gone"
                            : any_broke ? "broke"
                            : any_fixed ? "fixed"
                                        : "same";
        broken += any_broke && b && a ? 1 : 0;
        fixed_count += !any_broke && any_fixed && b && a ? 1 : 0;
        objectives += k ? ",\n    {\"name\": " : "\n    {\"name\": ";
        detail::json_string(objectives, name.str());
        json_place(objectives, (a ? a : b)->file, (a ? a : b)->line);
        objectives += ", \"moved\": ";
        detail::json_string(objectives, moved);
        objectives += ", \"before\": ";
        if (b)
            b->tally.json(objectives);
        else
            objectives += "null";
        objectives += ", \"after\": ";
        if (a)
            a->tally.json(objectives);
        else
            objectives += "null";
        objectives += ", \"median_change\": ";
        objectives += b && a && !b->tally.values.empty() && !a->tally.values.empty()
                          ? json_num(a->tally.median() - b->tally.median())
                          : Str("null");
        objectives += ", \"changed\": [" + changed + "]}";
    }
    broke = broken > 0;
    return "{\n  \"schema\": \"baker.compare/1\",\n  \"broke\": " + detail::decimal(broken) +
           ",\n  \"fixed\": " + detail::decimal(fixed_count) + ",\n  \"objectives\": [" +
           objectives + (names.empty() ? "]\n}\n" : "\n  ]\n}\n");
}

}  // namespace baker::host::sim
