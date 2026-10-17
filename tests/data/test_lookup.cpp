// Lookup: find by tag, typed reads, fallbacks, paths, Convert<T>, sorted mode, errors.
#include <doctest.h>
#include <baker/array.hpp>
#include <baker/cache.hpp>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/path.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

#include "alloc_counter.hpp"
#include "ok.hpp"

#include <array>
#include <cstdint>
#include <random>
#include <ranges>
#include <string>
#include <vector>

using namespace baker;
using baker::test::ok;

namespace {

const char* const kMenu =
    "(menu\n"
    "  ('bacon and eggs'\n"
    "    (price 12.75)\n"
    "    (calories 120)\n"
    "    (tags hot savory))\n"
    "  ('fruit and cereal'\n"
    "    (price 11.75)\n"
    "    (calories 12)\n"
    "    (on_order {print \"yum\"})\n"
    "    (count $n)))\n";

Ref<Array> menu_root() {
    auto r = read_string(kMenu, "menu.dta");
    REQUIRE(r);
    return *r;
}

template <class T>
Error error_of(const Expected<T>& e) {
    REQUIRE_FALSE(e.has_value());
    return e.error();
}

struct Vec2 {
    float x = 0, y = 0;
};

}  // namespace

template <>
struct baker::Convert<Vec2> {
    static constexpr std::string_view name = "vec2";
    static bool from(const Values& v, Vec2& out) {
        return v.size() == 2 && v.read(0, out.x) && v.read(1, out.y);
    }
};

TEST_CASE("find returns the first child array with the tag") {
    auto root = menu_root();
    const Array* menu = root->try_find("menu");
    REQUIRE(menu);
    const Array* eggs = menu->try_find("bacon and eggs");
    REQUIRE(eggs);
    CHECK(eggs->line() == 2);
    CHECK(menu->try_find("waffles") == nullptr);
    // Only arrays tagged with a symbol match: not commands, not strings, not values.
    auto a = array("x", array(str("k"), 1), Node(array("k", 2), Type::Command), "k");
    CHECK(a->try_find("k") == nullptr);
    auto dup = array("x", array("k", 1), array("k", 2));
    CHECK(dup->try_find("k")->values()[0].try_int() == 1);
    CHECK(Array::make(0)->try_find("k") == nullptr);
    CHECK(array("x", Array::make(0))->try_find("k") == nullptr);
}

TEST_CASE("tag is an array's first symbol, or the empty symbol") {
    auto root = menu_root();
    const Array* menu = root->try_find("menu");
    REQUIRE(menu);
    CHECK(menu->tag() == Symbol("menu"));
    std::vector<std::string> names;
    for (const Array& item : menu->subarrays()) names.emplace_back(item.tag().str());
    CHECK(names == std::vector<std::string>{"bacon and eggs", "fruit and cereal"});
    CHECK(array(1, 2)->tag() == Symbol());
    CHECK(array(str("k"), 1)->tag() == Symbol());
    CHECK(Array::make(0)->tag() == Symbol());
    // A value at a position is a path: (pos 1 2 3), index 1.
    auto pos = array("thing", array("pos", 1.5, 2.5, 3.5));
    CHECK(pos->get<double>("pos.1", 0.0).value_or(-1) == 2.5);
    CHECK(pos->get<double>("pos.7", 9.0).value_or(-1) == 9.0);
}

TEST_CASE("'' is no tag, for lookup, merge, and the cache alike") {
    auto root = ok(read_string("('' 1) (a 2)", "t.dta"));
    CHECK(root->size() == 2);
    CHECK((*root)[0].try_array()->tag() == Symbol());
    CHECK_FALSE(root->try_find(Symbol("")));  // the tag run keys it as no tag
    CHECK(root->try_find("a"));
    auto sorted = Array::make_copy(root->nodes());
    sorted->sort_for_lookup();
    CHECK_FALSE(sorted->try_find(Symbol("")));
    CHECK(sorted->try_find("a"));
    // Merged as a value, not matched by key: the second replaces the first, children and
    // all, where a key would have merged their children.
    auto merged =
        merge(*ok(read_string("('' (x 1))", "a.dta")), *ok(read_string("('' (y 2))", "b.dta")));
    REQUIRE(merged->size() == 1);
    CHECK(merged->nodes()[0].try_array()->size() == 2);  // '' and (y 2)
    // A cache keeps the meaning.
    std::string bytes;
    REQUIRE(write_cache(*root, TextSink{[](void* u, std::string_view t) {
                                            static_cast<std::string*>(u)->append(t);
                                        },
                                        &bytes}));
    auto back = ok(read_cache_bytes(bytes, "t.datb"));
    CHECK_FALSE(back->try_find(Symbol("")));
    CHECK(back->try_find("a"));
}

TEST_CASE("typed finds read the value after the tag") {
    auto root = menu_root();
    const Array& eggs = *root->try_find("menu")->try_find("bacon and eggs");
    CHECK(eggs.find_float("price") == 12.75);
    CHECK(eggs.find_int("calories") == 120);
    CHECK(eggs.find_float("calories") == 120.0);  // an int reads as a float
    CHECK(eggs.find_sym("tags") == Symbol("hot"));
    CHECK(eggs.find<float>("price") == 12.75f);
    CHECK(eggs.find<int32_t>("calories") == 120);
    CHECK(eggs.find<uint8_t>("calories") == 120);
    CHECK(eggs.find<std::vector<Symbol>>("tags") ==
          std::vector<Symbol>{Symbol("hot"), Symbol("savory")});

    auto t = array("t", array("name", str("Bob")), array("flag", 1), array("off", 0),
                   array("list", array(1, 2)), array("width", 2, array("range", 0, 10)),
                   array("big", 300), array("whole", 3.0));
    CHECK(t->find_str("name") == "Bob");
    CHECK(t->find<std::string>("name") == "Bob");
    CHECK(t->find<bool>("flag") == true);
    CHECK(t->find<bool>("off") == false);
    REQUIRE(t->find_array("list"));
    CHECK(to_string(**t->find_array("list")) == "(1 2)");
    CHECK(t->find_int("width") == 2);  // trailing metadata is ignored
    CHECK(t->find_int("whole") == 3);  // an integral float reads as an int
    CHECK(t->find<Node>("big")->try_int() == 300);
    CHECK_FALSE(t->find<uint8_t>("big"));
    CHECK(t->find<uint16_t>("big") == 300);
}

TEST_CASE("fallbacks apply only when the key is absent") {
    auto root = menu_root();
    const Array& eggs = *root->try_find("menu")->try_find("bacon and eggs");
    CHECK(eggs.find_int("fat", 7) == 7);
    CHECK(eggs.find_float("price", 1.0) == 12.75);
    CHECK(eggs.find_sym("size", "large") == Symbol("large"));
    CHECK(eggs.find_str("note", "none") == "none");
    CHECK(eggs.find<int32_t>("fat", -1) == -1);
    // Present but the wrong type: an error, not the fallback.
    CHECK(error_of(eggs.find_int("price", 0)).code() == ErrorCode::Type);
    CHECK(error_of(eggs.find_str("tags", "x")).code() == ErrorCode::Type);

    CHECK(root->get<int>("menu.'bacon and eggs'.fat", 3) == 3);
    CHECK(root->get<int>("menu.waffles.price", 3) == 3);
    CHECK(root->get<int>("menu.'bacon and eggs'.price.4", 3) == 3);
    CHECK(root->get<double>("menu.'bacon and eggs'.price", 0.0) == 12.75);
    CHECK(error_of(root->get<int>("menu.'bacon and eggs'.price", 0)).code() == ErrorCode::Type);
    CHECK(error_of(root->get<int>("menu..price", 0)).code() == ErrorCode::Syntax);
}

TEST_CASE("errors carry key, types, location, and path") {
    auto root = menu_root();
    const Array& menu = *root->try_find("menu");
    const Array& eggs = *menu.try_find("bacon and eggs");

    // The spec's example, through a path: the whole path is known.
    Error e = error_of(root->get<int>("menu.'bacon and eggs'.price"));
    CHECK(e.code() == ErrorCode::Type);
    CHECK(to_string(e) ==
          "baker: 'price' is float, not int  (menu.dta:3, at menu/'bacon and eggs')");

    // Through a chain of finds, only the array's own tag is known.
    e = error_of(eggs.find_int("price"));
    CHECK(to_string(e) == "baker: 'price' is float, not int  (menu.dta:3, at 'bacon and eggs')");

    e = error_of(eggs.find_int("fat"));
    CHECK(e.code() == ErrorCode::NotFound);
    CHECK(to_string(e) == "baker: 'fat' not found  (menu.dta:2, at 'bacon and eggs')");

    e = error_of(root->get<double>("menu.waffles.price"));
    CHECK(to_string(e) == "baker: 'waffles' not found  (menu.dta:1, at menu)");

    e = error_of(eggs.find_sym("price"));
    CHECK(e.message() == "'price' is float, not symbol");
    e = error_of(eggs.find_str("tags"));
    CHECK(e.message() == "'tags' is symbol, not string");
    e = error_of(eggs.find_array("price"));
    CHECK(e.message() == "'price' is float, not array");
    e = error_of(eggs.find<uint8_t>("price"));
    CHECK(e.message() == "'price' is float, not int");
    CHECK(eggs.find<int8_t>("calories") == 120);
    e = error_of(array("t", array("calories", 300))->find<int8_t>("calories"));
    CHECK(e.code() == ErrorCode::Range);
    CHECK(e.message() == "'calories' value 300 is out of range for int8");
    e = error_of(eggs.find<float>("tags"));
    CHECK(e.message() == "'tags' is symbol, not float");
    e = error_of(eggs.find<Vec2>("tags"));
    CHECK(e.message() == "'tags' (hot savory) does not read as vec2");
    CHECK(e.line() == 5);

    auto empty = array("t", array("nothing"));
    CHECK(error_of(empty->find_int("nothing")).message() == "'nothing' has no value");
}

TEST_CASE("commands and variables need baker::script to be read as values") {
    auto root = menu_root();
    const Array& fruit = *root->try_get("menu.'fruit and cereal'");
    Error e = error_of(fruit.find_str("on_order"));
    CHECK(e.message() == "'on_order' is a command; reading it as a value needs baker::script");
    CHECK(e.line() == 9);
    e = error_of(fruit.find_int("count"));
    CHECK(e.message() == "'count' is a var; reading it as a value needs baker::script");
    // Raw access never evaluates.
    CHECK(fruit.try_find("on_order")->values()[0].type() == Type::Command);
    CHECK(fruit.find<Node>("count")->type() == Type::Var);
}

TEST_CASE("failed lookups suggest the likely mistake") {
    auto r =
        read_string("(menu\n (\"bacon and eggs\" (price 1))\n (Price 2)\n (calories 3))", "m.dta");
    REQUIRE(r);
    const Array& menu = *(*r)->try_find("menu");
    Error e = error_of(menu.find_float("bacon and eggs"));
    CHECK(e.message() ==
          "'bacon and eggs' not found; \"bacon and eggs\" at line 2 is a string, but keys are "
          "symbols: write 'bacon and eggs'");
    e = error_of(menu.find_float("price"));
    CHECK(e.message() == "'price' not found; did you mean 'Price'?");
    e = error_of((*r)->get<float>("menu.CALORIES"));
    CHECK(e.message() == "'CALORIES' not found; did you mean 'calories'?");
}

TEST_CASE("paths parse names, quoted names, and indexes") {
    Path p("renderer.clear_color.0");
    REQUIRE(p.valid());
    REQUIRE(p.size() == 3);
    CHECK(p[0].name == Symbol("renderer"));
    CHECK(p[2].is_index);
    CHECK(p[2].index == 0);
    CHECK(to_string(p) == "renderer/clear_color/0");
    CHECK(to_string(p, 1) == "renderer");

    Path q("menu/'bacon and eggs'/price");
    REQUIRE(q.valid());
    CHECK(q[1].name == Symbol("bacon and eggs"));
    CHECK(to_string(q) == "menu/'bacon and eggs'/price");
    CHECK(to_string(Path("'a.b'.'7'.c")) == "'a.b'/'7'/c");
    CHECK(Path("'a.b'")[0].name == Symbol("a.b"));
    CHECK(Path("'7'")[0].name == Symbol("7"));
    CHECK_FALSE(Path("'7'")[0].is_index);
    CHECK(Path("x.-1")[1].name == Symbol("-1"));

    CHECK(Path("").valid());
    CHECK(Path("").empty());
    for (const char* bad : {".", "a.", ".a", "a..b", "a/", "'a", "'a'b", "a.99999999999"}) {
        CAPTURE(bad);
        Path b(bad);
        CHECK_FALSE(b.valid());
        CHECK(to_string(b) == bad);
    }
    auto root = menu_root();
    Error e = error_of(root->get<int>("menu..price"));
    CHECK(e.code() == ErrorCode::Syntax);
    CHECK(e.message() == "malformed path 'menu..price'");
    CHECK(root->try_get("menu..price") == nullptr);
}

TEST_CASE("paths walk names and indexes") {
    auto r = read_string(
        "(renderer (clear_color 0.1 0.2 0.3) (screen_size 1920 1080)\n"
        "  (points (1 2) (3 4)) (nested (inner (x 5))))",
        "cfg.dta");
    REQUIRE(r);
    const Array& root = **r;
    CHECK(root.get<double>("renderer.clear_color.0") == 0.1);
    CHECK(root.get<double>("renderer.clear_color.2") == 0.3);
    CHECK(root.get<double>("renderer.clear_color") == 0.1);  // a scalar reads the first value
    CHECK(root.get<std::array<float, 3>>("renderer.clear_color") ==
          std::array<float, 3>{0.1f, 0.2f, 0.3f});
    CHECK_FALSE(root.get<std::array<float, 2>>("renderer.clear_color"));
    auto size = root.get<Vec2>("renderer.screen_size");
    REQUIRE(size);
    CHECK(size->x == 1920);
    CHECK(size->y == 1080);
    auto second = root.get<Vec2>("renderer.points.1");
    REQUIRE(second);
    CHECK(second->x == 3);
    auto all = root.get<std::vector<Vec2>>("renderer.points");
    REQUIRE(all);
    CHECK(all->size() == 2);
    CHECK(root.get<int>("renderer.nested.inner.x") == 5);
    CHECK(root.get<int>("renderer/nested/inner/x") == 5);

    // The array forms.
    CHECK(root.try_get("renderer.nested")->line() == 2);
    CHECK(root.try_get("") == &root);
    CHECK(to_string(*root.try_get("renderer.points.0")) == "(1 2)");
    CHECK(root.try_get("renderer.clear_color.0") == nullptr);  // a leaf, not an array

    Error e = error_of(root.get<double>("renderer.clear_color.3"));
    CHECK(e.code() == ErrorCode::NotFound);
    CHECK(to_string(e) ==
          "baker: element 3 is out of range (3 values)  (cfg.dta:1, at renderer/clear_color)");
    e = error_of(root.get<Symbol>("renderer.clear_color.1"));
    CHECK(to_string(e) ==
          "baker: element 1 is float, not symbol  (cfg.dta:1, at renderer/clear_color)");
    e = error_of(root.get<int>("renderer.clear_color.0.x"));
    CHECK(e.code() == ErrorCode::NotFound);
    CHECK(e.path() == "renderer/clear_color/0");
    e = error_of(root.get<Vec2>("renderer.points"));
    CHECK(e.message() == "'points' ((1 2) (3 4)) does not read as vec2");
}

TEST_CASE("Values::read descends into arrays") {
    auto a = array("v", array(1, 2), 3);
    Values v = a->values();
    Vec2 p;
    CHECK(v.read(0, p));
    CHECK(p.y == 2);
    int i = 0;
    CHECK(v.read(1, i));
    CHECK(i == 3);
    CHECK_FALSE(v.read(2, i));
    CHECK_FALSE(v.read(0, i));
    CHECK(v.source() == a.get());
}

TEST_CASE("sort_for_lookup keeps find's answers and slot 0") {
    auto a = array("root", 1, array("b", 1), array("a", 2), "loose", array("b", 3),
                   array(str("s"), 4), array("c", 5));
    std::vector<std::pair<std::string, const Array*>> before;
    for (const char* t : {"a", "b", "c", "d", "root"}) before.emplace_back(t, a->try_find(t));
    a->sort_for_lookup();
    CHECK(a->sorted_for_lookup());
    CHECK((*a)[0].is("root"));
    CHECK((*a)[1].try_int() == 1);
    CHECK((*a)[2].is("loose"));
    for (const auto& [t, found] : before) {
        CAPTURE(t);
        CHECK(a->try_find(Symbol(t)) == found);
    }
    CHECK(a->find_int("b") == 1);  // duplicates keep their order
    a->set(1, 7);
    CHECK_FALSE(a->sorted_for_lookup());

    auto empty = Array::make(0);
    empty->sort_for_lookup();
    CHECK(empty->try_find("x") == nullptr);
}

TEST_CASE("sorted and linear find agree on random arrays") {
    std::mt19937 rng(1234);
    std::vector<Symbol> names;
    for (int i = 0; i < 40; ++i) names.emplace_back("key" + std::to_string(i));
    for (int round = 0; round < 300; ++round) {
        const auto n = static_cast<size_t>(rng() % 30);
        std::vector<Node> nodes;
        for (size_t i = 0; i < n; ++i) {
            const Symbol tag = names[rng() % names.size()];
            switch (rng() % 4) {
                case 0: nodes.emplace_back(tag); break;
                case 1: nodes.emplace_back(static_cast<int64_t>(i)); break;
                default: nodes.emplace_back(array(tag, static_cast<int64_t>(i))); break;
            }
        }
        auto linear = Array::make_copy(nodes);
        auto sorted = Array::make_copy(nodes);
        sorted->sort_for_lookup();
        for (Symbol s : names) {
            const Array* l = linear->try_find(s);
            const Array* r = sorted->try_find(s);
            REQUIRE((l == nullptr) == (r == nullptr));
            if (l) CHECK(l == r);  // the same (shared) child array
        }
    }
}

TEST_CASE("subarrays is a forward range of the child arrays") {
    static_assert(std::ranges::forward_range<Subarrays>);
    auto a = array("x", 1, array("a"), Node(array("c"), Type::Command), array("b"), "y");
    std::vector<std::string> tags;
    for (const Array& sub : a->subarrays()) tags.emplace_back(sub[0].try_sym()->str());
    CHECK(tags == std::vector<std::string>{"a", "b"});
    CHECK(std::ranges::distance(a->subarrays()) == 2);
    CHECK(std::ranges::distance(Array::make(0)->subarrays()) == 0);
}

TEST_CASE("lookups that succeed do not allocate") {
    if (!test::counting_allocations()) {
        MESSAGE("allocation counting is unavailable in this build (ThreadSanitizer)");
        return;
    }
    auto root = menu_root();
    const Array& eggs = *root->try_get("menu.'bacon and eggs'");
    const Symbol price("price"), calories("calories"), tags("tags");
    const Path path("menu.'bacon and eggs'.price");
    auto sorted = array("s", array("a", 1), array("b", 2), array("c", 3));
    sorted->sort_for_lookup();
    double sum = 0;
    test::AllocationScope scope;
    for (int i = 0; i < 100; ++i) {
        sum += *eggs.find_float(price);
        sum += static_cast<double>(*eggs.find_int(calories));
        sum += static_cast<double>(eggs.find_sym(tags)->str().size());
        sum += *eggs.find<float>(price);
        sum += static_cast<double>(*eggs.find_int(Symbol(), 1));  // absent: the fallback
        sum += *root->get<double>(path);
        sum += static_cast<double>(*sorted->find_int("b"));
    }
    CHECK(scope.count() == 0);
    CHECK(sum > 0);
}

TEST_CASE("typed reads with fallbacks, and their errors") {
    auto root = menu_root();
    const Array& menu = *root->try_find("menu");
    const Array& eggs = *menu.try_find("bacon and eggs");
    CHECK(eggs.find_float("price") == 12.75);
    CHECK(eggs.find_int("calories") == 120);
    CHECK(eggs.find_sym("tags") == Symbol("hot"));
    CHECK(eggs.find<int>("calories") == 120);
    CHECK(eggs.find_int("fat", 4) == 4);
    CHECK(eggs.find_float("fat", 0.5) == 0.5);
    CHECK(eggs.find_sym("fat", "none") == Symbol("none"));
    CHECK(eggs.find_str("fat", "none") == "none");
    CHECK(eggs.find("fat", 2.5) == 2.5);
    CHECK(root->get<float>("menu.'bacon and eggs'.price") == 12.75f);
    CHECK(root->get<int>("menu.'bacon and eggs'.fat", 9) == 9);
    CHECK(root->try_get("menu") == &menu);

    auto t = array("t", array("name", str("Bob")), array("list", array(1, 2)));
    CHECK(t->find_str("name") == "Bob");
    CHECK((*t->find_array("list"))->size() == 2);

    const auto wrong = eggs.find_int("price");
    REQUIRE_FALSE(wrong);
    CHECK(wrong.error().code() == ErrorCode::Type);
    CHECK(to_string(wrong.error()) ==
          "baker: 'price' is float, not int  (menu.dta:3, at 'bacon and eggs')");
    CHECK_FALSE(menu.find_array("waffles"));
    CHECK_FALSE(eggs.find_int("price", 0));  // present, wrong type
    CHECK_FALSE(root->get<int>("menu.'bacon and eggs'.price"));
    CHECK(root->try_get("menu.'bacon and eggs'.price.0") == nullptr);
    CHECK_FALSE(root->get<int>("a..b", 0));
    CHECK_FALSE(t->find_array("name"));
}

TEST_CASE("the corpus config reads through lookups") {
    LoadSession s;
    auto app = read_file(std::string(BAKER_CORPUS_DIR) + "/config_app.dta", s);
    REQUIRE(app);
    const Array& cfg = **app;
    CHECK(cfg.get<Vec2>("renderer.screen_size")->x == 1920);  // the app's override
    CHECK(cfg.get<int>("renderer.shadows.resolution") == 4096);
    CHECK(cfg.get<int>("renderer.shadows.cascades") == 3);  // merged from the defaults
    CHECK(cfg.get<bool>("renderer.vsync") == true);         // TRUE is a macro for 1
    CHECK(cfg.get<double>("audio.master_volume") == 0.8);
    CHECK(cfg.get<int>("audio.channels") == 32);
    CHECK(cfg.get<std::string>("app.title") == "Corpus Demo");
    CHECK(cfg.get<int>("app.max_players") == 4);
    CHECK(cfg.get<double>("input.deadzone") == 0.15);
    Error e = error_of(cfg.get<int>("input.deadzone"));
    CHECK(e.file().str().ends_with("config_default.dta"));
    CHECK(e.line() == 15);
    CHECK(e.path() == "input");
}

TEST_CASE("a Path holds its segments inline, up to kMaxSegments") {
    std::string deep = "a";
    for (size_t i = 1; i < Path::kMaxSegments; ++i) deep += ".a";
    CHECK(Path(deep).valid());
    CHECK(Path(deep).size() == Path::kMaxSegments);
    CHECK_FALSE(Path(deep + ".a").valid());
    (void)Path("menu.'bacon and eggs'.price");  // intern the names first
    test::AllocationScope scope;
    const Path p("menu.'bacon and eggs'.price");
    CHECK(p.size() == 3);
    if (test::counting_allocations()) CHECK(scope.count() == 0);
}

TEST_CASE("the tag run follows set(), sorting, and the documented re-tag rule") {
    auto root = baker::read_string("(a 1) (b 2) 7 (c 3) ()", "run.dta");
    REQUIRE(root);
    Array& r = *const_cast<Array*>(root->get());
    CHECK(r.try_find("b") == r[1].array());
    CHECK(r.try_find("zz") == nullptr);
    // set() replaces a child, and its tag.
    r.set(1, Node(Array::make_copy((*baker::read_string("(z 9)"))->nodes()[0].array()->nodes())));
    CHECK(r.try_find("b") == nullptr);
    REQUIRE(r.try_find("z"));
    CHECK((*r.try_find("z"))[1].try_int() == 9);
    // A non-array, or an untagged array, has no tag.
    r.set(0, Node(5));
    CHECK(r.try_find("a") == nullptr);
    // Re-tagging a child in place: the parent finds it under neither tag until it sets the
    // child again.
    auto* c = const_cast<Array*>(r[3].array());
    c->set(0, Node(Symbol("renamed")));
    CHECK(r.try_find("renamed") == nullptr);
    CHECK(r.try_find("c") == nullptr);
    r.set(3, r[3]);
    CHECK(r.try_find("renamed") == c);
    CHECK(r.try_find("c") == nullptr);
    // Sorting moves the run with the nodes; set() turns sorting off, and the run still finds.
    r.sort_for_lookup();
    CHECK(r.try_find("renamed") == c);
    r.set(0, Node(Symbol("x")));
    CHECK(r.try_find("z"));
    CHECK(r.try_find("renamed") == c);
}
