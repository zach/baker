// Baked tables (table.hpp): a viewed keyed shape's records as a packed array, built
// from the merged tree, read by index, key, path, ID and parent, rebuilt by identity. The
// views are tests/shape/views/, the golden headers.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/image.hpp>
#include <baker/parse.hpp>
#include <baker/project.hpp>
#include <baker/std_interop.hpp>
#include <baker/table.hpp>

#include "alloc_counter.hpp"
#include "item_read.hpp"
#include "move_read.hpp"
#include "ok.hpp"
#include "table_build.hpp"
#include "temp_dir.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

using namespace baker;
using baker::test::ok;
using views_test::Item;
using views_test::Move;

namespace {

const char* const kFighters =
    "(fighters\n"
    "  (kestrel (moves\n"
    "    (jab (@id a0000000000000000000000000000001) (startup 4) (damage 3.5) (name \"Jab\")"
    " (button light) (armored 1) (cancel_into heavy kick) (hitbox (x 1) (y 2.5))"
    " (points (p (x 0) (y 0)) (p (x 3) (y 1))) (ramp (0 0) (1 1)) (on_hit {do_hit}))\n"
    "    (heavy (@aka slam) (startup 9) (damage 12))\n"
    "    (kick (startup 7) (damage 8))))\n"
    "  (wren (moves\n"
    "    (poke (@id a0000000000000000000000000000002) (startup 3) (damage 2)))))\n"
    "(items (potion (price 12.5) (tag \"heals\") (sizes 1 2 3)) (sword (price 150)))\n";

std::string text(const Error& e) {
    return std::string(to_string(e));
}

bool same_curve(const Curve& a, const Curve& b) {
    if (a.size() != b.size() || a.mode() != b.mode() || a.extend() != b.extend()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const CurvePoint p = a.point(i), q = b.point(i);
        if (p.x != q.x || p.y != q.y || p.in != q.in || p.out != q.out || p.sloped != q.sloped)
            return false;
    }
    return true;
}

template <class V>
bool same_list(const ListView<V>& a, const ListView<V>& b) {
    if (a.size() != b.size()) return false;
    for (uint32_t i = 0; i < a.size(); ++i) {
        const V x = a[i], y = b[i];
        if constexpr (std::is_same_v<V, views_test::Vec2>) {
            if (x.x != y.x || x.y != y.y) return false;
        } else if (!(x == y)) {
            return false;
        }
    }
    return true;
}

bool same(const Move& a, const Move& b) {
    return a.startup == b.startup && a.damage == b.damage && a.name == b.name &&
           a.button == b.button && a.armored == b.armored &&
           same_list(a.cancel_into, b.cancel_into) && a.hitbox.x == b.hitbox.x &&
           a.hitbox.y == b.hitbox.y && same_list(a.points, b.points) && same_curve(a.ramp, b.ramp);
}

bool same(const Item& a, const Item& b) {
    return a.price == b.price && a.tag == b.tag && same_list(a.sizes, b.sizes);
}

// Every row of `t` reads back what the view's reader reads from its record in `root`, found
// by the row's path: the table changes how a row is stored, never what it reads.
template <class T>
void check_rows(const Table<T>& t, const Array& root, std::string_view at) {
    REQUIRE(t);
    for (uint32_t i = 0; i < t.size(); ++i) {
        std::string path(at);
        if (t.block()->wildcards()) {
            // The parent's tag: the one whose range holds the row.
            std::string parent;
            for (const Node& n : *root.try_get(Path(at)))
                if (const Array* a = n.try_array(); a && !a->tag().empty()) {
                    const IndexRange r = t.children_of(a->tag());
                    if (i >= r.first && i < r.first + r.count) parent = a->tag().str();
                }
            path += "." + parent + ".moves";
        }
        path += "." + std::string(t.key_of(i).str());
        const Array* record = root.try_get(Path(path));
        REQUIRE_MESSAGE(record, path);
        T read{};
        (void)ViewReader<T>::read(*record, read, nullptr);
        CHECK_MESSAGE(same(t[i], read), path);
    }
}

// A record ID's text: `n` in decimal digits, after an `a` and zeros to 32 digits.
std::string id_text(int n) {
    const std::string digits = std::to_string(n);
    return "a" + std::string(31 - digits.size(), '0') + digits;
}

// The fixtures at scale: 300 moves and 40 items, with strings (many repeated), symbol lists,
// lists of nested views, nested views, curves, aliases, IDs and a record in error.
std::string many_fighters() {
    std::string fighters = "(fighters\n";
    for (int f = 0; f < 6; ++f) {
        fighters += " (f" + std::to_string(f) + " (moves\n";
        for (int m = 0; m < 50; ++m) {
            const std::string i = std::to_string(f * 50 + m);
            fighters += "  (m" + std::to_string(m) + (m % 9 == 0 ? " (@aka old" + i + ")" : "");
            if (m % 7 == 0) fighters += " (@id " + id_text(100 + f * 50 + m) + ")";
            fighters += " (startup " + std::to_string(m % 60 + 1) + ") (damage " + i + ".5)";
            if (m % 4) fighters += " (name \"move number " + std::to_string(m % 10) + "\")";
            if (m % 3) fighters += " (cancel_into m1 m2 m" + std::to_string(m % 7) + ")";
            fighters += " (hitbox (x " + i + ") (y 2))";
            for (int p = 0; p < m % 4; ++p) fighters += p ? "" : " (points";
            for (int p = 0; p < m % 4; ++p)
                fighters += " (p (x " + std::to_string(p) + ") (y " + i + "))";
            if (m % 4) fighters += ")";
            if (m % 5 == 0) fighters += " (ramp (0 0) (1 " + i + ") (2 1))";
            if (m == 17) fighters += " (armored maybe)";  // not a bool: an error, located
            fighters += ")\n";
        }
        fighters += " ))\n";
    }
    fighters += ")\n(items";
    for (int k = 0; k < 40; ++k) {
        fighters += " (it" + std::to_string(k) + " (price " + std::to_string(k) + ")";
        if (k % 2) fighters += " (tag \"" + std::string(static_cast<size_t>(k % 6), 'x') + "\")";
        if (k % 3) fighters += " (sizes 1 2 " + std::to_string(k) + ")";
        fighters += ")";
    }
    fighters += ")\n";
    return fighters;
}

}  // namespace

TEST_CASE("a table holds the records of a keyed shape, packed, with their strings and lists") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(add_table<Move>(cfg));
    REQUIRE(add_table<Item>(cfg));
    REQUIRE(cfg.push_layer_text("base", kFighters));
    const Table<Move> moves = table<Move>(cfg);
    REQUIRE(moves);
    REQUIRE(moves.size() == 4);
    const Move& jab = moves[0];
    CHECK(jab.startup == 4);
    CHECK(jab.damage == 3.5f);
    CHECK(jab.name == "Jab");
    CHECK(jab.button == Symbol("light"));
    CHECK(jab.armored);
    // Lists are packed: the elements are the table's, converted once.
    CHECK(jab.cancel_into.is_packed());
    REQUIRE(jab.cancel_into.size() == 2);
    CHECK(jab.cancel_into[1] == Symbol("kick"));
    CHECK(jab.cancel_into.elements()[0] == Symbol("heavy"));
    CHECK(jab.hitbox.y == 2.5f);
    REQUIRE(jab.points.size() == 2);
    CHECK(jab.points[1].x == 3.0f);
    CHECK(jab.points.elements()[1].y == 1.0f);
    CHECK(jab.ramp.size() == 2);
    CHECK(jab.ramp.sample(0.5) == doctest::Approx(0.5));
    // The strings are the table's, not the tree's.
    const Ref<Array> root = cfg.root();
    const Array* tree_jab = root->try_get("fighters.kestrel.moves.jab");
    REQUIRE(tree_jab);
    const std::string_view tree_name = *tree_jab->find_str(Symbol("name"));
    CHECK(tree_name.data() != jab.name.data());
    // Rows are in tree order, grouped by parent; keys and IDs ride along.
    CHECK(moves.key_of(1) == Symbol("heavy"));
    CHECK(moves.key_of(3) == Symbol("poke"));
    CHECK(moves.id_of(0) == *RecordId::parse("a0000000000000000000000000000001"));
    CHECK(moves.id_of(1).empty());
    CHECK(moves[3].startup == 3);

    const Table<Item> items = table<Item>(cfg);
    REQUIRE(items.size() == 2);
    CHECK(items[0].price == 12.5f);
    CHECK(items[0].tag == "heals");
    REQUIRE(items[0].sizes.size() == 3);
    CHECK(items[0].sizes[2] == 3);
    CHECK(items[1].tag.empty());
    CHECK(items[1].sizes.empty());
}

TEST_CASE("every row reads back what the view reads from its record") {
    SUBCASE("the fixtures") {
        Config cfg({.tweaks = nullptr});
        REQUIRE(add_table<Move>(cfg));
        REQUIRE(add_table<Item>(cfg));
        REQUIRE(cfg.push_layer_text("base", kFighters));
        check_rows(table<Move>(cfg), *cfg.root(), "fighters");
        check_rows(table<Item>(cfg), *cfg.root(), "items");
    }
    SUBCASE("strings, lists and nested views, with aliases, IDs and a record in error") {
        Config cfg({.tweaks = nullptr});
        std::vector<std::string> warnings;
        cfg.set_warning_handler([&](const Error& e) { warnings.push_back(text(e)); });
        REQUIRE(add_table<Move>(cfg));
        REQUIRE(add_table<Item>(cfg));
        REQUIRE(cfg.push_layer_text("base", many_fighters()));
        REQUIRE(table<Move>(cfg).size() == 300);
        CHECK(table<Item>(cfg).size() == 40);
        // The record in error is reported once each build, from its one read.
        CHECK(std::count_if(warnings.begin(), warnings.end(), [](const std::string& w) {
                  return w.find("armored") != std::string::npos;
              }) == 6);
        check_rows(table<Move>(cfg), *cfg.root(), "fighters");
        check_rows(table<Item>(cfg), *cfg.root(), "items");
    }
}

TEST_CASE("a table stores each distinct string once") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(add_table<Move>(cfg));
    REQUIRE(add_table<Item>(cfg));
    REQUIRE(cfg.push_layer_text("base", many_fighters()));
    const Table<Move> moves = table<Move>(cfg);
    // Each name is one of ten texts: every row with the same text points at the same bytes.
    std::vector<std::string_view> seen;
    for (const Move& m : moves.rows()) {
        if (m.name.empty()) continue;
        const auto it = std::find(seen.begin(), seen.end(), m.name);
        if (it == seen.end())
            seen.push_back(m.name);
        else
            CHECK(it->data() == m.name.data());
    }
    CHECK(seen.size() == 10);
    // The empty strings share one byte too, and each is terminated.
    const Table<Item> items = table<Item>(cfg);
    CHECK(items[0].tag.data() == items[2].tag.data());
    CHECK(items[1].tag.data()[items[1].tag.size()] == '\0');
    // The same rows with every text distinct take more pool: 300 rows of names, ten texts.
    std::string distinct = many_fighters();
    for (size_t at = 0, n = 0; (at = distinct.find("move number ", at)) != std::string::npos; ++n) {
        at += 12;
        distinct.insert(at, std::to_string(n) + "_");
    }
    Config other({.tweaks = nullptr});
    REQUIRE(add_table<Move>(other));
    REQUIRE(other.push_layer_text("base", distinct));
    CHECK(table<Move>(other).block()->block_bytes() > moves.block()->block_bytes() + 200 * 13);
}

TEST_CASE("a table keeps IDs only when its view is (@view by_id)") {
    // Move is by_id; Item is not, so its table keeps no ID column and finds no ID.
    static_assert(Move::kById && !Item::kById);
    Config cfg({.tweaks = nullptr});
    REQUIRE(add_table<Move>(cfg));
    REQUIRE(add_table<Item>(cfg));
    REQUIRE(cfg.push_layer_text(
        "base", std::string(kFighters) +
                    "(items (shield (@id a0000000000000000000000000000003) (price 40)))\n"));
    const Table<Item> items = table<Item>(cfg);
    REQUIRE(items.size() == 3);
    CHECK_FALSE(items.block()->by_id());
    CHECK(items.id_of(2).empty());
    CHECK_FALSE(items.index_of(*RecordId::parse("a0000000000000000000000000000003")));
    CHECK(items.index_of(Symbol("shield")) == 2);
    const Table<Move> moves = table<Move>(cfg);
    CHECK(moves.block()->by_id());
    CHECK(moves.index_of(*RecordId::parse("a0000000000000000000000000000002")) == 3);
    // The same view registered without by_id: the same rows, smaller by the ID column and
    // its index, and no ID found.
    TableSpec spec{
        .digest = Move::kShapeDigest,
        .name = Move::kShapeName,
        .at = Move::kAt,
        .row_size = sizeof(Move),
        .row_align = alignof(Move),
        .fixups = Move::fixups(),
        .by_id = false,
        .init = [](void* row) { new (row) Move{}; },
        .read = &view_detail::read_erased<Move>,
    };
    Config plain({.tweaks = nullptr});
    REQUIRE(plain.add_table(spec));
    REQUIRE(plain.push_layer_text("base", many_fighters()));
    Config full({.tweaks = nullptr});
    REQUIRE(add_table<Move>(full));
    REQUIRE(full.push_layer_text("base", many_fighters()));
    const Table<Move> without = table<Move>(plain);
    const Table<Move> with = table<Move>(full);
    REQUIRE(without.size() == 300);
    CHECK(without.block()->block_bytes() + 300 * (sizeof(RecordId) + sizeof(BakedTable::IdEntry)) ==
          with.block()->block_bytes());
    CHECK(with.index_of(*RecordId::parse(id_text(100))) == 0);
    CHECK_FALSE(without.index_of(*RecordId::parse(id_text(100))));
    CHECK(without.id_of(0).empty());
    check_rows(without, *plain.root(), "fighters");
}

TEST_CASE("a table is read by key, by path, by ID, by alias and by parent") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(add_table<Move>(cfg));
    REQUIRE(add_table<Item>(cfg));
    REQUIRE(cfg.push_layer_text("base", kFighters));
    const Table<Move> moves = table<Move>(cfg);
    const Table<Item> items = table<Item>(cfg);
    // A shape with no `*`: by key.
    CHECK(items.index_of(Symbol("sword")) == 1);
    CHECK_FALSE(items.index_of(Symbol("potion.x")));
    CHECK(items.index_of("potion") == 0);
    // One with `*`: by the path below the table.
    CHECK(moves.index_of("kestrel.jab") == 0);
    CHECK(moves.index_of("kestrel.kick") == 2);
    CHECK(moves.index_of("wren.poke") == 3);
    CHECK(moves.index_of(Path("kestrel.heavy")) == 1);
    CHECK_FALSE(moves.index_of("kestrel.poke"));
    CHECK_FALSE(moves.index_of("jab"));  // a key alone is not a path below this table
    CHECK_FALSE(moves.index_of(Symbol("jab")));
    CHECK_FALSE(moves.index_of("kestrel.moves.jab"));
    CHECK_FALSE(moves.index_of("kestrel.jab.startup"));
    // By ID, and by an alias (a renamed record's old key).
    CHECK(moves.index_of(*RecordId::parse("a0000000000000000000000000000002")) == 3);
    CHECK_FALSE(moves.index_of(RecordId{}));
    CHECK_FALSE(moves.index_of(*RecordId::parse("a0000000000000000000000000000009")));
    CHECK(moves.index_of("kestrel.slam") == 1);
    CHECK_FALSE(moves.index_of("wren.slam"));
    // By parent: a contiguous range.
    const IndexRange k = moves.children_of(Symbol("kestrel"));
    CHECK(k.first == 0);
    CHECK(k.count == 3);
    int n = 0;
    for (const uint32_t i : k) n += moves[i].startup;
    CHECK(n == 20);
    CHECK(moves.children_of(Symbol("wren")).first == 3);
    CHECK(moves.children_of(Symbol("nobody")).empty());
    CHECK(items.children_of(Symbol("potion")).empty());
    CHECK(moves.block()->wildcards() == 1);
    CHECK(items.block()->wildcards() == 0);
    // at() checks, operator[] does not.
    CHECK(moves.at(4) == nullptr);
    CHECK(moves.at(3)->startup == 3);
}

TEST_CASE("a table is rebuilt when its records change, a held handle keeps the old one") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(add_table<Move>(cfg));
    REQUIRE(add_table<Item>(cfg));
    REQUIRE(cfg.push_layer_text("base", kFighters));
    const Table<Move> before = table<Move>(cfg);
    const Table<Item> items_before = table<Item>(cfg);
    // A layer that retunes a move: the moves table is rebuilt, the items table is shared.
    REQUIRE(cfg.push_layer_text("tune", "(fighters (kestrel (moves (jab (damage 4)))))"));
    const Table<Move> after = table<Move>(cfg);
    CHECK(after.block() != before.block());
    CHECK(table<Item>(cfg).block() == items_before.block());
    CHECK(after[0].damage == 4.0f);
    CHECK(before[0].damage == 3.5f);  // the held handle reads what it held
    // Indices are stable while the set of records is unchanged.
    CHECK(after.index_of("kestrel.kick") == 2);
    CHECK(after.index_of("wren.poke") == 3);
    // A record added renumbers what follows it; one removed too.
    REQUIRE(cfg.push_layer_text("more",
                                "(fighters (kestrel (moves (throw (startup 11) (damage 1)))))"));
    const Table<Move> more = table<Move>(cfg);
    CHECK(more.size() == 5);
    CHECK(more.index_of("kestrel.throw") == 3);
    CHECK(more.index_of("wren.poke") == 4);
    REQUIRE(cfg.remove_layer("more"));
    CHECK(table<Move>(cfg).size() == 4);
    // A live edit reaches the table at apply_edits.
    if constexpr (!kShipping) {
        REQUIRE(cfg.set("fighters.wren.moves.poke.startup", 5));
        CHECK(table<Move>(cfg)[3].startup == 3);
        cfg.apply_edits();
        CHECK(table<Move>(cfg)[3].startup == 5);
    }
    // Registered after the load: built at once.
    Config late({.tweaks = nullptr});
    REQUIRE(late.push_layer_text("base", kFighters));
    CHECK_FALSE(table<Move>(late));
    REQUIRE(add_table<Move>(late));
    CHECK(table<Move>(late).size() == 4);
    auto twice = add_table<Move>(late);
    REQUIRE_FALSE(twice);
    CHECK(text(twice.error()).find("registered already") != std::string::npos);
}

TEST_CASE("an edit elsewhere keeps a table whose records a layer overrides") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(add_table<Move>(cfg));
    REQUIRE(cfg.push_layer_text("base", kFighters));
    // The tune layer overrides kestrel's moves: every merge builds that array anew.
    REQUIRE(cfg.push_layer_text("tune", "(fighters (kestrel (moves (jab (damage 4)))))"));
    const Table<Move> tuned = table<Move>(cfg);
    // A layer that touches nothing below fighters: an equal moves array, the table kept.
    REQUIRE(cfg.push_layer_text("other", "(volume 3)"));
    CHECK(table<Move>(cfg).block() == tuned.block());
    if constexpr (!kShipping) {
        REQUIRE(cfg.set("volume", 4));
        cfg.apply_edits();
        CHECK(table<Move>(cfg).block() == tuned.block());
        // An edit to a record does rebuild it.
        REQUIRE(cfg.set("fighters.kestrel.moves.kick.damage", 9));
        cfg.apply_edits();
        CHECK(table<Move>(cfg).block() != tuned.block());
        CHECK(table<Move>(cfg)[2].damage == 9.0f);
    }
}

TEST_CASE("a parent renamed above an unchanged records array rebuilds the table") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(add_table<Move>(cfg));
    REQUIRE(cfg.push_layer_text("base", "(fighters (kestrel (moves (jab (startup 4)))))"));
    CHECK(table<Move>(cfg).index_of("kestrel.jab") == 0);
    // The same moves, under another fighter: the rows' tags change with the `*`.
    REQUIRE(cfg.remove_layer("base"));
    REQUIRE(cfg.push_layer_text("base", "(fighters (falcon (moves (jab (startup 4)))))"));
    CHECK_FALSE(table<Move>(cfg).index_of("kestrel.jab"));
    CHECK(table<Move>(cfg).index_of("falcon.jab") == 0);
}

TEST_CASE("a field that is not a literal reads as its default, with a located error") {
    Config cfg({.tweaks = nullptr});
    std::vector<std::string> warnings;
    cfg.set_warning_handler([&](const Error& e) { warnings.push_back(text(e)); });
    REQUIRE(add_table<Move>(cfg));
    REQUIRE(cfg.push_layer_text("base",
                                "(fighters (kestrel (moves (jab (startup 4) (damage {compute}))"
                                " (kick (startup \"soon\") (damage 1)))))"));
    const Table<Move> moves = table<Move>(cfg);
    REQUIRE(moves.size() == 2);
    CHECK(moves[0].startup == 4);
    CHECK(moves[0].damage == 0.0f);
    CHECK(moves[1].damage == 1.0f);  // the reader goes on past the failed field
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].find("damage") != std::string::npos);
    CHECK(warnings[0].find("fighters.kestrel.moves.jab") != std::string::npos);
    CHECK(warnings[1].find("startup") != std::string::npos);
}

TEST_CASE("reading a table allocates nothing") {
    // Building one allocates through the Config's allocator alone: baker_alloc_tests.
    if (!test::counting_allocations()) return;
    Config cfg({.tweaks = nullptr});
    REQUIRE(add_table<Move>(cfg));
    REQUIRE(cfg.push_layer_text("base", kFighters));
    const Table<Move> moves = table<Move>(cfg);
    size_t allocations = 0;
    Symbol first;
    {
        const test::AllocationScope scope;
        const auto i = moves.index_of("kestrel.jab");
        const auto by_id = moves.index_of(*RecordId::parse("a0000000000000000000000000000001"));
        const Move& jab = moves[i.value_or(9)];
        first = jab.cancel_into[0];
        allocations = scope.count() + (by_id == i ? 0 : 100);
    }
    CHECK(first == Symbol("heavy"));
    CHECK(allocations == 0);
}

namespace {

// A project of two contract layers and an overlay, written to `dir`.
Project two_layers(test::TempDir& dir) {
    Project p;
    const std::string core = dir.write("core.dta", kFighters);
    const std::string content = dir.write(
        "content.dta",
        "(fighters (wren (moves (poke (name \"Poke\")))))\n(items (sword (tag \"sharp\")))\n");
    const std::string over =
        dir.write("over.dta", "(fighters (kestrel (moves (jab (damage 5)))))\n");
    std::vector<ProjectLayer> layers;
    layers.push_back({.name = Symbol("core"), .path = Symbol(core)});
    layers.push_back({.name = Symbol("content"), .path = Symbol(content)});
    layers.push_back({.name = Symbol("over"), .path = Symbol(over), .optional = true});
    p.layers = List<ProjectLayer>({layers.data(), layers.size()});
    return p;
}

}  // namespace

TEST_CASE("a stack image holds the contract layers and the tables, and a Config opens from it") {
    test::TempDir dir;
    const Project project = two_layers(dir);
    // The build: the contract layers alone, the tables built over them.
    std::string image;
    {
        Config build({.tweaks = nullptr});
        REQUIRE(add_table<Move>(build));
        REQUIRE(add_table<Item>(build));
        REQUIRE(build.push_layer("core", project.layers[0].path.str()));
        REQUIRE(build.push_layer("content", project.layers[1].path.str()));
        const Symbol contract[] = {Symbol("core"), Symbol("content")};
        REQUIRE(write_stack_image(build, contract, string_out(image)));
        CHECK(image.substr(0, 10) == "MOIRA.IMG1");
    }
    const std::string path = dir.write("game.datb", image);
    // The game: the contract layers from the image, the overlay from its file.
    Config cfg({.tweaks = nullptr, .stack_image = path});
    std::vector<std::string> warnings;
    cfg.set_warning_handler([&](const Error& e) { warnings.push_back(text(e)); });
    REQUIRE(add_table<Move>(cfg));
    REQUIRE(add_table<Item>(cfg));
    REQUIRE(open_project(cfg, project));
    CHECK(warnings.empty());
    CHECK(cfg.layers().size() == 4);  // core, content, over, scratch
    CHECK(ok(cfg.get<float>("fighters.kestrel.moves.jab.damage")) == 5.0f);
    const Table<Move> moves = table<Move>(cfg);
    const Table<Item> items = table<Item>(cfg);
    REQUIRE(moves.size() == 4);
    // The overlay touched the moves, so they were rebuilt from the tree; the items are the
    // image's block, strings and lists fixed up.
    CHECK(moves[0].damage == 5.0f);
    CHECK(moves[3].name == "Poke");
    CHECK(moves.index_of("kestrel.slam") == 1);
    CHECK(moves.id_of(0) == *RecordId::parse("a0000000000000000000000000000001"));
    REQUIRE(items.size() == 2);
    CHECK(items[1].tag == "sharp");
    CHECK(items[0].tag == "heals");
    CHECK(items[0].sizes.is_packed());
    CHECK(items[0].sizes[2] == 3);
    CHECK(items.index_of(Symbol("sword")) == 1);
    CHECK(items.key_of(0) == Symbol("potion"));
    // A layer pushed above rebuilds the table it touches.
    REQUIRE(cfg.push_layer_text("live", "(items (sword (price 1)))"));
    CHECK(table<Item>(cfg)[1].price == 1.0f);
    CHECK(table<Item>(cfg)[1].tag == "sharp");
}

TEST_CASE("a table section reads back every row, carries no addresses, and is checked") {
    const std::string fighters = many_fighters();
    // Two builds of the same data write the same bytes: the section holds offsets and names.
    std::vector<std::string> sections[2];
    std::vector<TableSpec> specs;
    for (int build = 0; build < 2; ++build) {
        Config cfg({.tweaks = nullptr});
        REQUIRE(add_table<Move>(cfg));
        REQUIRE(add_table<Item>(cfg));
        REQUIRE(cfg.push_layer_text("base", fighters));
        for (const detail::TableSlot& slot : detail::table_slots(cfg)) {
            Str out;
            REQUIRE(detail::write_table(*slot.built, slot.spec, out));
            sections[build].emplace_back(out.data(), out.size());
            if (build == 0) specs.push_back(slot.spec);
        }
        if (build == 1) continue;
        // Read back into blocks of their own: every row as the view reads its record.
        for (size_t i = 0; i < specs.size(); ++i) {
            Error why;
            Ref<const BakedTable> block =
                detail::read_table(sections[0][i], specs[i], default_allocator(), why);
            REQUIRE_MESSAGE(block, text(why));
            if (specs[i].digest == Move::kShapeDigest) {
                const Table<Move> moves(block);
                check_rows(moves, *cfg.root(), "fighters");
                CHECK(moves.index_of(*RecordId::parse(id_text(107))) == 7);
                CHECK(moves.index_of("f3.old159") == 159);
                CHECK(moves.children_of(Symbol("f5")).first == 250);
            } else {
                check_rows(Table<Item>(block), *cfg.root(), "items");
            }
        }
    }
    REQUIRE(sections[0].size() == 2);
    CHECK(sections[0] == sections[1]);
    // A section is refused, by name, for a view that differs, and as damaged when it does not
    // hold together.
    const std::string& moves =
        specs[0].digest == Move::kShapeDigest ? sections[0][0] : sections[0][1];
    const TableSpec& spec = specs[0].digest == Move::kShapeDigest ? specs[0] : specs[1];
    const auto refused = [&](const std::string& bytes, const TableSpec& s) {
        Error why;
        const Ref<const BakedTable> block = detail::read_table(bytes, s, default_allocator(), why);
        return block ? std::string() : text(why);
    };
    TableSpec other = spec;
    other.by_id = false;
    CHECK(refused(moves, other).find("lookup by ID differs") != std::string::npos);
    CHECK(refused(moves.substr(0, moves.size() - 64), spec).find("damaged") != std::string::npos);
    std::string more_rows = moves;
    const uint32_t rows = 1u << 20;
    std::memcpy(more_rows.data() + 20, &rows, sizeof rows);  // after digest, fixups, row_size
    CHECK(refused(more_rows, spec).find("damaged") != std::string::npos);
}

TEST_CASE("an image's table that does not match the compiled view is skipped, with a warning") {
    test::TempDir dir;
    const Project project = two_layers(dir);
    std::string image;
    {
        Config build({.tweaks = nullptr});
        REQUIRE(add_table<Item>(build));  // the image has items, and no moves
        REQUIRE(build.push_layer("core", project.layers[0].path.str()));
        REQUIRE(build.push_layer("content", project.layers[1].path.str()));
        const Symbol contract[] = {Symbol("core"), Symbol("content")};
        REQUIRE(write_stack_image(build, contract, string_out(image)));
    }
    const std::string path = dir.write("game.datb", image);
    Config cfg({.tweaks = nullptr, .stack_image = path});
    std::vector<std::string> warnings;
    cfg.set_warning_handler([&](const Error& e) { warnings.push_back(text(e)); });
    REQUIRE(add_table<Move>(cfg));
    REQUIRE(add_table<Item>(cfg));
    REQUIRE(open_project(cfg, project));
    REQUIRE(warnings.size() == 1);
    // Shipping keeps no view names: the warning then names "a view".
    CHECK(warnings[0].find(kShipping ? "no table for a view" : "no table for move") !=
          std::string::npos);
    CHECK(warnings[0].find("built from the tree") != std::string::npos);
    CHECK(table<Move>(cfg).size() == 4);  // built from the tree
    CHECK(table<Item>(cfg).size() == 2);  // the image's
    // A damaged image is refused.
    const std::string broken = dir.write("broken.datb", image.substr(0, image.size() / 2));
    Config bad({.tweaks = nullptr, .stack_image = broken});
    auto r = open_project(bad, project);
    REQUIRE_FALSE(r);
    CHECK(text(r.error()).find("damaged") != std::string::npos);
    // An image of an older version is refused by its version, not as damaged; the
    // table section's layout made it 3.
    std::string old = image;
    old[10] = 1;  // the version, after the ten bytes of magic
    old[11] = 0;
    Config older({.tweaks = nullptr, .stack_image = dir.write("old.datb", old)});
    auto o = open_project(older, project);
    REQUIRE_FALSE(o);
    CHECK(o.error().code() == ErrorCode::Refused);
    CHECK(text(o.error()).find("of version 1; this build reads version 3") != std::string::npos);
}
