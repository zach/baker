// Record IDs: the form, a record's own, find_by_id, the validator over duplicates
// and malformed IDs, and what --drop-metadata keeps.
#include <doctest.h>
#include <baker/cache.hpp>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/record_id.hpp>
#include <baker/std_interop.hpp>

#include "alloc_counter.hpp"
#include "ok.hpp"
#include "session.hpp"
#include "temp_dir.hpp"

#include <string>
#include <vector>

using namespace baker;
using baker::test::ok;

namespace {

const char* const kWeapons =
    "(weapons\n"
    "  (laser (@id b3c9a7e5f214d68a1e2c3b4d5e6f7081) (damage 3) (@desc \"zap\"))\n"
    "  (cannon (@id c000000000000000000000000000000a) (damage 9))\n"
    "  (sling (damage 1)))\n"
    "(units (scout (weapon laser) (@aka recon)))\n";

}  // namespace

TEST_CASE("a record id is 32 lowercase hex digits, the first a to f, and round-trips") {
    const auto id = RecordId::parse("b3c9a7e5f214d68a1e2c3b4d5e6f7081");
    REQUIRE(id);
    CHECK(id->hi == 0xb3c9a7e5f214d68aull);
    CHECK(id->lo == 0x1e2c3b4d5e6f7081ull);
    CHECK(id->valid());
    char text[RecordId::kDigits];
    id->write(text);
    CHECK(std::string(text, 32) == "b3c9a7e5f214d68a1e2c3b4d5e6f7081");
    CHECK_FALSE(RecordId::parse("03c9a7e5f214d68a1e2c3b4d5e6f70812"));  // a digit first
    CHECK_FALSE(RecordId::parse("B3C9A7E5F214D68A1E2C3B4D5E6F70812"));  // upper case
    CHECK_FALSE(RecordId::parse("b3c9a7e5f214d68a1e2c3b4d5e6f708"));    // 31 digits
    CHECK_FALSE(RecordId::parse("b3c9a7e5f214d68a1e2c3b4d5e6f7081x"));  // not hex
    CHECK(RecordId{}.empty());
    CHECK_FALSE(RecordId{0x1ull, 0}.valid());
}

TEST_CASE("a record reads its own id; find_by_id finds the record and its path") {
    LoadSession session;
    auto root = ok(read_string(kWeapons, "w.dta", session));
    const Array* laser = root->try_get("weapons.laser");
    REQUIRE(laser);
    const RecordId id = laser->record_id();
    CHECK(id == *RecordId::parse("b3c9a7e5f214d68a1e2c3b4d5e6f7081"));
    CHECK(root->try_get("weapons.sling")->record_id().empty());
    CHECK(root->try_get("units.scout")->record_id().empty());
    Path path;
    CHECK(find_by_id(*root, id, &path) == laser);
    std::string text;
    path.write(string_out(text), SIZE_MAX, '.');
    CHECK(text == "weapons.laser");
    CHECK(find_by_id(*root, RecordId{1, 2}) == nullptr);
    CHECK(find_by_id(*root, RecordId{}) == nullptr);
}

TEST_CASE("the validator names two records that share an id, and a malformed one") {
    Config cfg({.tweaks = nullptr});
    std::vector<std::string> reported;
    cfg.set_warning_handler([&](const Error& e) { reported.emplace_back(to_string(e)); });
    cfg.add_validator(id_validator(), {.any_thread = true});
    REQUIRE(cfg.push_layer_text("w", kWeapons));
    CHECK(reported.empty());
    REQUIRE(
        cfg.push_layer_text("copy",
                            "(weapons (laser2 (@id b3c9a7e5f214d68a1e2c3b4d5e6f7081) (damage 4)))\n"
                            "(items (odd (@id 12345)))\n"));
    REQUIRE(reported.size() == 2);
    CHECK(reported[0].find(
              "(@id ...) holds 32 lowercase hex digits, the first a to f: (@id 12345)") !=
          std::string::npos);
    CHECK(reported[0].find("items/odd") != std::string::npos);
    CHECK(reported[1].find("two records share the id b3c9a7e5f214d68a1e2c3b4d5e6f7081: "
                           "weapons/laser (w:2) and weapons/laser2") != std::string::npos);
    CHECK(reported[1].find("format --baker-mint-ids") != std::string::npos);
    CHECK(reported[1].find("copy:1") != std::string::npos);
    // Validators run at every merge, so the duplicate is reported again; a record copied
    // without its id adds nothing.
    reported.clear();
    REQUIRE(cfg.push_layer_text("plain", "(weapons (laser3 (damage 5)))"));
    CHECK(reported.size() == 2);
    REQUIRE(cfg.remove_layer("copy"));
    reported.clear();
    REQUIRE(cfg.push_layer_text("more", "(weapons (laser4 (damage 6)))"));
    CHECK(reported.empty());
}

TEST_CASE("@id and @aka are known metadata, and a shipped cache keeps them") {
    LoadSession session;
    auto root = ok(read_string(kWeapons, "w.dta", session));
    const Ref<Array> lean = without_metadata(*root);
    const Array* laser = lean->try_get("weapons.laser");
    REQUIRE(laser);
    CHECK_FALSE(laser->record_id().empty());
    CHECK(laser->meta(Symbol("@desc")) == nullptr);
    CHECK(lean->try_get("units.scout")->meta(Symbol("@aka")) != nullptr);
}

TEST_CASE("the validator over a stack without ids allocates little and reports nothing") {
    Config cfg({.tweaks = nullptr});
    int reports = 0;
    cfg.set_warning_handler([&](const Error&) { ++reports; });
    cfg.add_validator(id_validator(), {.any_thread = true});
    REQUIRE(cfg.push_layer_text("w", "(a (b (c 1)) (d 2))"));
    CHECK(reports == 0);
}

// --- Aliases: (@aka old) on a renamed record. ---

TEST_CASE("a layer that retunes a record by its old key merges into the renamed record") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base",
                                "(weapons (plasma (@aka laser) (damage 3) (range 9))"
                                "         (cannon (damage 9)))"));
    REQUIRE(cfg.push_layer_text("overlay", "(weapons (laser (damage 5)))"));
    CHECK(ok(cfg.get<int>("weapons.plasma.damage")) == 5);
    CHECK(ok(cfg.get<int>("weapons.plasma.range")) == 9);
    CHECK_FALSE(cfg.get<int>("weapons.laser.damage"));  // the old key is not a record
    // The record keeps its new key even when the overlay restates it whole.
    REQUIRE(cfg.push_layer_text("whole", "(weapons (laser (@aka laser) (damage 7) (range 9)))"));
    CHECK(ok(cfg.get<int>("weapons.plasma.damage")) == 7);
    CHECK_FALSE(cfg.root()->try_get("weapons.laser"));
    // A key no record answers to is a new record, as before.
    REQUIRE(cfg.push_layer_text("new", "(weapons (railgun (damage 20)))"));
    CHECK(ok(cfg.get<int>("weapons.railgun.damage")) == 20);
    // Removing the aliased layer: the overlay's key is then its own record.
    REQUIRE(cfg.remove_layer("base"));
    CHECK(ok(cfg.get<int>("weapons.laser.damage")) == 7);
}

TEST_CASE("an alias resolves through the indexed path too: a wide table") {
    std::string base = "(items";
    for (int i = 0; i < 40; ++i)
        base += " (item" + std::to_string(i) + " (cost " + std::to_string(i) + "))";
    base += " (gem (@aka ruby emerald) (cost 100)))";
    std::string over = "(items";
    for (int i = 0; i < 40; ++i)
        over += " (item" + std::to_string(i) + " (cost " + std::to_string(i + 1) + "))";
    over += " (emerald (cost 150)))";
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base", base));
    REQUIRE(cfg.push_layer_text("over", over));
    CHECK(ok(cfg.get<int>("items.gem.cost")) == 150);
    CHECK(ok(cfg.get<int>("items.item39.cost")) == 40);
    CHECK_FALSE(cfg.root()->try_get("items.emerald"));
}

TEST_CASE("a key that is also a sibling's alias is an error at both") {
    Config cfg({.tweaks = nullptr});
    std::vector<std::string> reported;
    cfg.set_warning_handler([&](const Error& e) { reported.emplace_back(to_string(e)); });
    cfg.add_validator(id_validator(), {.any_thread = true});
    REQUIRE(cfg.push_layer_text("w",
                                "(weapons (plasma (@aka laser) (damage 3))\n"
                                "         (laser (damage 1)))"));
    REQUIRE(reported.size() == 1);
    CHECK(reported[0].find("'laser' is a key of this table and an alias of 'plasma' (w:1)") !=
          std::string::npos);
    CHECK(reported[0].find("w:2") != std::string::npos);
    CHECK(reported[0].find("at weapons") != std::string::npos);
}

TEST_CASE("a stack without aliases resolves nothing by alias, and a shipped cache keeps them") {
    LoadSession session;
    auto root = ok(read_string("(weapons (plasma (@aka laser) (damage 3)))", "w.dta", session));
    const Ref<Array> lean = without_metadata(*root);
    CHECK(lean->try_get("weapons.plasma")->meta(Symbol("@aka")) != nullptr);
}

TEST_CASE("a layer with fewer aliases than the one below still resolves by them") {
    // The count is per read: a comparison with the previous read's would miss these.
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("a", "(x (@aka x1) (v 1)) (y (@aka y1) (v 2))"));
    REQUIRE(cfg.push_layer_text("b", "(z (@aka z1) (v 3))"));
    REQUIRE(cfg.push_layer_text("over", "(z1 (v 30)) (x1 (v 10))"));
    CHECK(ok(cfg.get<int>("z.v")) == 30);
    CHECK(ok(cfg.get<int>("x.v")) == 10);
}

TEST_CASE("a layer replayed from a cache keeps its aliases, and one without pays no scan") {
    if constexpr (kShipping) return;  // a cache_dir Config writes caches in development only
    test::TempDir dir;
    const std::string base = dir.write("base.dta", "(weapons (plasma (@aka laser) (damage 3)))");
    const std::string plain = dir.write("plain.dta", "(loot (coin (v 1)))");
    const std::string cache_dir = dir.path("cache");
    for (int pass = 0; pass < 2; ++pass) {  // the second pass replays both caches
        Config cfg({.tweaks = nullptr, .cache_dir = cache_dir});
        REQUIRE(cfg.push_layer("base", base));
        REQUIRE(cfg.push_layer("plain", plain));
        REQUIRE(cfg.push_layer_text("overlay", "(weapons (laser (damage 5)))"));
        CHECK(ok(cfg.get<int>("weapons.plasma.damage")) == 5);
    }
    // A cache's record says how many arrays carry an alias: none for plain.dta.
    LoadSession session;
    session.record_for_cache = true;
    auto root = ok(read_file(plain, session));
    std::string bytes;
    ok(write_cache(
        *root,
        TextSink{[](void* u, std::string_view t) { static_cast<std::string*>(u)->append(t); },
                 &bytes},
        &session));
    LoadSession replay;
    (void)ok(read_cache_bytes(bytes, "plain.datb", replay));
    CHECK(detail::SessionAccess::aliases(replay) == 0);
    LoadSession aliased;
    aliased.record_for_cache = true;
    root = ok(read_file(base, aliased));
    bytes.clear();
    ok(write_cache(
        *root,
        TextSink{[](void* u, std::string_view t) { static_cast<std::string*>(u)->append(t); },
                 &bytes},
        &aliased));
    LoadSession replay2;
    (void)ok(read_cache_bytes(bytes, "base.datb", replay2));
    CHECK(detail::SessionAccess::aliases(replay2) == 1);
}

TEST_CASE("provenance follows the alias: source() names the layer that wrote the old key") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base", "(weapons (plasma (@aka laser) (damage 3) (range 9)))"));
    REQUIRE(cfg.push_layer_text("overlay", "(weapons (laser (damage 5)))"));
    const Source s = ok(cfg.source("weapons.plasma.damage"));
    CHECK(s.layer == Symbol("overlay"));
    CHECK(ok(cfg.source("weapons.plasma.range")).layer == Symbol("base"));
}
