// A view read: one pass over a record's children into a generated struct, against
// the same fields read one find at a time, as a game without views reads them. Its own
// translation unit, so bench_lookup's rows stay comparable across releases.
#include <baker/array.hpp>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/table.hpp>
#include <baker/view_read.hpp>

#include "move_read.hpp"
#include "scalars_read.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

namespace {

using namespace baker::literals;

volatile double g_sink;

template <class T>
const T& opaque(const T& v) {
    const T* volatile p = &v;
    return *p;
}

template <class F>
double ns_per_call(F&& f) {
    constexpr int kCalls = 2'000'000;
    double best = 1e30;
    for (int rep = 0; rep < 7; ++rep) {
        double sum = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kCalls; ++i) sum += f(i);
        const auto t1 = std::chrono::steady_clock::now();
        g_sink = sum;
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / kCalls);
    }
    return best;
}

void row(const char* name, double ns) {
    std::printf("%-44s %7.2f ns\n", name, ns);
}

}  // namespace

int main() {
    auto fighters = baker::read_string(
        "(fighters (kestrel (moves (jab (startup 4) (damage 3.5) (name \"Jab\") (button light)"
        " (armored 1) (cancel_into heavy kick) (hitbox (x 1) (y 2.5)) (points (p (x 0) (y 0)))"
        " (ramp (0 0) (1 1)) (on_hit {do_hit}) (taunt \"ha\")))))",
        "view.dta");
    if (!fighters) return 1;
    const baker::Array& jab = *(*fighters)->try_get("fighters.kestrel.moves.jab");
    row("read_view<Move>, 10 fields of 11 children", ns_per_call([&](int) {
            views_test::Move m;
            baker::Error err;
            (void)baker::ViewReader<views_test::Move>::read(opaque(jab), m, &err);
            return static_cast<double>(m.startup) + m.damage;
        }));
    row("read_view<Scalars>, 5 fields of 11 children", ns_per_call([&](int) {
            views_test::Scalars m;
            baker::Error err;
            (void)baker::ViewReader<views_test::Scalars>::read(opaque(jab), m, &err);
            return static_cast<double>(m.startup) + m.damage;
        }));
    // A baked table: the same record read from the table by index, found by
    // its path, its ID and its parent, against the tree's find.
    baker::Config cfg({.tweaks = nullptr});
    if (!baker::add_table<views_test::Move>(cfg)) return 1;
    std::string text = "(fighters";
    for (int f = 0; f < 8; ++f) {
        text += " (fighter" + std::to_string(f) + " (moves";
        for (int m = 0; m < 16; ++m)
            text += " (move" + std::to_string(m) + " (@id a" + std::to_string(f) +
                    "00000000000000000000000000000" + std::to_string(m / 10) +
                    std::to_string(m % 10) + ") (startup " + std::to_string(m + 1) +
                    ") (damage 2.5) (name \"n\") (button light) (cancel_into a b))";
        text += "))";
    }
    text += ")";
    if (!cfg.push_layer_text("bench", text)) return 1;
    const baker::Table<views_test::Move> moves = baker::table<views_test::Move>(cfg);
    if (moves.size() != 128) return 1;
    const baker::RecordId id = *baker::RecordId::parse("a3000000000000000000000000000007");
    row("table[i].damage, by index", ns_per_call([&](int i) {
            return static_cast<double>(moves[static_cast<uint32_t>(i) & 127].damage);
        }));
    row("table.index_of(\"fighter3.move7\"), a path", ns_per_call([&](int) {
            return static_cast<double>(
                moves.index_of(opaque(std::string_view("fighter3.move7"))).value_or(0));
        }));
    const baker::Path path("fighter3.move7");
    row("table.index_of(path), a Path held", ns_per_call([&](int) {
            return static_cast<double>(moves.index_of(opaque(path)).value_or(0));
        }));
    row("table.index_of(id), a record id", ns_per_call([&](int) {
            return static_cast<double>(moves.index_of(opaque(id)).value_or(0));
        }));
    row("table.children_of(fighter3), a parent", ns_per_call([&](int) {
            return static_cast<double>(moves.children_of(opaque(baker::Symbol("fighter3"))).count);
        }));
    const baker::Ref<baker::Array> root = cfg.root();
    row("the tree: try_get(\"fighters.fighter3.moves.move7\").damage", ns_per_call([&](int) {
            const baker::Array* r = opaque(*root).try_get("fighters.fighter3.moves.move7");
            return static_cast<double>(r->find_float("damage"_sym).value_or(0));
        }));
    row("the same 5 scalar fields, one find each", ns_per_call([&](int) {
            const baker::Array& a = opaque(jab);
            const auto startup = *a.find_int("startup"_sym);
            const auto damage = *a.find_float("damage"_sym);
            const auto name = *a.find_str("name"_sym);
            const auto button = *a.find_sym("button"_sym);
            const auto armored = *a.find_int("armored"_sym);
            return static_cast<double>(startup + armored + name.size() + button.index()) + damage;
        }));
    return 0;
}
