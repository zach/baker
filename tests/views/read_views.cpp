// The views a project's build generates, compiled and read: baker_generate_views made
// the headers under this test's binary directory from tests/views/project. Exits 1 on a
// mismatch, naming it.
#include <baker/config.hpp>
#include <baker/project.hpp>
#include <baker/view_read.hpp>

#include "boss_read.hpp"
#include "item_read.hpp"
#include "tables.hpp"
#include "views.hpp"

#include <cstdio>
#include <string_view>

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

}  // namespace

// The umbrella lists every view, the listed one included, sorted.
static_assert(std::size(views_gen::kViews) == 3);
static_assert(views_gen::Boss::kShapeDigest != 0);

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    auto project = baker::read_project(argv[1]);
    if (!project) {
        std::fprintf(stderr, "%s\n", baker::to_string(project.error()).data());
        return 1;
    }
    baker::Config cfg({.tweaks = nullptr});
    if (auto r = views_gen::register_tables(cfg); !r) {
        std::fprintf(stderr, "%s\n", baker::to_string(r.error()).data());
        return 1;
    }
    if (auto r = baker::open_project(cfg, *project); !r) {
        std::fprintf(stderr, "%s\n", baker::to_string(r.error()).data());
        return 1;
    }
    auto dragon = baker::read_view<views_gen::Boss>(cfg, "bosses.dragon");
    expect(dragon.has_value(), "the dragon reads as a Boss");
    if (dragon) {
        expect(dragon->hp == 4000, "hp");
        expect(dragon->pos.y == 2.5f, "pos, a nested view");
        expect(dragon->tags.size() == 2 && dragon->tags[1] == baker::Symbol("flying"), "tags");
        // The content layer's field is in the struct; the mod's is in the tree only.
        expect(dragon->hint == "Stay low", "hint, from the content layer");
        expect(cfg.get<int>("bosses.dragon.armor").value_or(0) == 3, "armor, in the tree");
    }
    auto golem = baker::read_view<views_gen::Boss>(cfg, "bosses.golem");
    expect(golem && golem->pos.x == 0 && golem->tags.empty() && golem->hint.empty(),
           "absent optional fields read as their defaults");
    expect(golem && golem->hp == 2500, "a field under its old name reads as the field");
    auto potion = cfg.get<views_gen::Item>("items.potion");
    expect(potion && potion->price == 12.5f && potion->stack == 20,
           "a listed view, through get<T>");
    expect(views_gen::kViews[0].name == "boss" || views_gen::kViews[0].name.empty(),
           "kViews is sorted by name, or nameless under shipping");
    // The generated register_tables added both keyed views' tables.
    const baker::Table<views_gen::Boss> bosses = baker::table<views_gen::Boss>(cfg);
    const baker::Table<views_gen::Item> items = baker::table<views_gen::Item>(cfg);
    expect(bosses.size() == 2 && bosses.index_of("dragon") == 0u && bosses[0].hp == 4000 &&
               bosses[1].hp == 2500,
           "the bosses table, the golem's hp read under its old name");
    expect(bosses[0].pos.y == 2.5f && bosses[0].tags.size() == 2 && bosses[0].hint == "Stay low",
           "a nested view, a packed list and a string in the table");
    expect(items.size() == 2 && items.index_of(baker::Symbol("sword")) == 1u &&
               items[1].price == 150.0f,
           "the items table, a listed view");
    return failures ? 1 : 0;
}
