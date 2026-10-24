// #include_dir: a directory as the children of one array, each file a record named by its
// file.
#include <doctest.h>
#include <baker/cache.hpp>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "dev_only.hpp"
#include "ok.hpp"
#include "temp_dir.hpp"

using namespace baker;
using baker::test::ok;
using baker::test::TempDir;

namespace {

// A card game's data: two cards, a folder of fire cards, and a deck beside them.
struct Cards {
    TempDir dir;
    std::string main;
    Cards() {
        main = dir.write("main.dta",
                         "(cards\n"
                         "   #include_dir \"cards\"\n"
                         "   (starter_deck fireball shield))\n");
        dir.write("cards/fireball.dta", "(cost 3)\n(damage 6)\n");
        dir.write("cards/shield.dta", "(cost 1)\n");
    }
};

std::vector<std::string> texts(std::span<const Error> errors) {
    std::vector<std::string> out;
    for (const Error& e : errors) out.emplace_back(to_string(e));
    return out;
}

}  // namespace

TEST_CASE("each file of the directory is a record named by its file") {
    Cards c;
    LoadSession s;
    const auto root = ok(read_file(c.main, s));
    CHECK(to_string(*root) ==
          "((cards (fireball (cost 3) (damage 6)) (shield (cost 1)) "
          "(starter_deck fireball shield)))");
    // A record's nodes keep their own file and line.
    const Array* fireball = (*root)[0].array()->try_find("fireball");
    REQUIRE(fireball);
    CHECK(std::filesystem::path(fireball->file().str()).filename() == "fireball.dta");
    CHECK(fireball->try_find("damage")->line() == 2);
}

TEST_CASE("a folder is a nested table, and order.dta orders a directory") {
    Cards c;
    c.dir.write("cards/fire/ember.dta", "(cost 2)\n");
    c.dir.write("cards/fire/blaze.dta", "(cost 5)\n");
    c.dir.write("cards/order.dta", "(order shield fire ghost)\n");
    c.dir.write("cards/notes.txt", "not a record\n");
    c.dir.write("cards/.drafts/secret.dta", "(cost 0)\n");
    LoadSession s;
    const auto root = ok(read_file(c.main, s));
    CHECK(to_string(*root) ==
          "((cards (shield (cost 1)) (fire (blaze (cost 5)) (ember (cost 2))) "
          "(fireball (cost 3) (damage 6)) (starter_deck fireball shield)))");
    REQUIRE(s.warnings().size() == 1);
    CHECK(texts(s.warnings())[0].find("order.dta lists 'ghost', which") != std::string::npos);
    CHECK(texts(s.warnings())[0].find("order.dta:1") != std::string::npos);
}

TEST_CASE("a layer above retunes a record by name, and adds one") {
    Cards c;
    Config cfg;
    REQUIRE(cfg.push_layer("base", c.main));
    REQUIRE(cfg.push_layer("tuning", ok(read_string("(cards (fireball (cost 4)) "
                                                    "(frost (cost 2)))",
                                                    "tuning.dta"))));
    CHECK(cfg.get<int>("cards.fireball.cost") == 4);
    CHECK(cfg.get<int>("cards.fireball.damage") == 6);
    CHECK(cfg.get<int>("cards.frost.cost") == 2);
    const auto where = cfg.source("cards.fireball.damage");
    REQUIRE(where);
    CHECK(where->layer == Symbol("base"));
    CHECK(std::filesystem::path(where->file.str()).filename() == "fireball.dta");
    CHECK(where->line == 2);
    // stack_files() names the directory beside the files.
    bool listed = false;
    for (const StackFile& f : cfg.stack_files())
        listed = listed || (f.directory && f.present &&
                            std::filesystem::path(f.path.str()).filename() == "cards");
    CHECK(listed);
}

TEST_CASE("a directory is listed and read through the host's lister and reader") {
    std::map<std::string, std::string> files = {
        {"pak://main.dta", "(cards #include_dir \"cards\")\n"},
        {"pak://cards/fireball.dta", "(cost 3)\n"},
        {"pak://cards/fire/ember.dta", "(cost 2)\n"},
    };
    LoadSession s;
    s.set_reader([&](std::string_view path, TextSink out) {
        const auto it = files.find(std::string(path));
        if (it == files.end()) return false;
        out(it->second);
        return true;
    });
    s.set_lister([&](std::string_view dir, Callback<void(std::string_view)> entry) {
        const std::string prefix = std::string(dir) + "/";
        std::vector<std::string> seen;
        for (const auto& [path, text] : files) {
            if (path.rfind(prefix, 0) != 0) continue;
            const std::string rest = path.substr(prefix.size());
            const std::string first = prefix + rest.substr(0, rest.find('/'));
            if (std::find(seen.begin(), seen.end(), first) == seen.end()) {
                seen.push_back(first);
                entry(first);
            }
        }
        return !seen.empty();
    });
    const auto root = ok(read_file("pak://main.dta", s));
    CHECK(to_string(*root) == "((cards (fire (ember (cost 2))) (fireball (cost 3))))");
}

TEST_CASE("a cache is stale once the directory or its order.dta changes") {
    Cards c;
    const std::string cache = c.dir.path("main.datb");
    LoadSession first;
    first.record_for_cache = true;
    const auto root = ok(read_file(c.main, first));
    REQUIRE(write_cache(*root, cache, &first));
    LoadSession again;
    CHECK(cache_fresh(cache, again));
    if constexpr (!kShipping) {
        SUBCASE("a file added") {
            c.dir.write("cards/frost.dta", "(cost 2)\n");
            CHECK_FALSE(cache_fresh(cache, again));
            LoadSession reread;
            CHECK(to_string(*ok(read_cached(c.main, cache, reread))).find("(frost (cost 2))") !=
                  std::string::npos);
        }
        SUBCASE("a file removed") {
            std::filesystem::remove(c.dir.path("cards/shield.dta"));
            CHECK_FALSE(cache_fresh(cache, again));
        }
        SUBCASE("an order.dta written") {
            c.dir.write("cards/order.dta", "(order shield)\n");
            CHECK_FALSE(cache_fresh(cache, again));
        }
        SUBCASE("a record's file edited") {
            c.dir.write("cards/shield.dta", "(cost 2)\n");
            CHECK_FALSE(cache_fresh(cache, again));
        }
    }
}

TEST_CASE("a bad name, a file that does not parse, and a missing directory are errors") {
    Cards c;
    SUBCASE("a name that is not a symbol") {
        c.dir.write("cards/my card.dta", "(cost 1)\n");
        LoadSession s;
        const auto r = read_file(c.main, s);
        REQUIRE_FALSE(r);
        CHECK(to_string(r.error()).find("cannot be a record: its name is not a symbol") !=
              std::string::npos);
        CHECK(to_string(r.error()).find("main.dta:2") != std::string::npos);
    }
    SUBCASE("a file and a folder of one name") {
        c.dir.write("cards/shield/tower.dta", "(cost 1)\n");
        LoadSession s;
        const auto r = read_file(c.main, s);
        REQUIRE_FALSE(r);
        CHECK(to_string(r.error()).find("two records named 'shield'") != std::string::npos);
    }
    SUBCASE("a file that does not parse") {
        c.dir.write("cards/shield.dta", "(cost 1\n");
        LoadSession s;
        const auto r = read_file(c.main, s);
        REQUIRE_FALSE(r);
        CHECK(to_string(r.error()).find("shield.dta:1") != std::string::npos);
    }
    SUBCASE("a missing directory") {
        const std::string other = c.dir.write("other.dta", "(x #include_dir \"none\")\n");
        LoadSession s;
        const auto r = read_file(other, s);
        REQUIRE_FALSE(r);
        CHECK(to_string(r.error()).find("cannot list the directory 'none' of #include_dir") !=
              std::string::npos);
    }
    SUBCASE("text that may include nothing") {
        LoadSession s;
        s.refuse_includes = true;
        CHECK_FALSE(read_string("(cards #include_dir \"cards\")", c.dir.path("t.dta"), s));
    }
    SUBCASE("the include directories are searched") {
        const std::string elsewhere = c.dir.write("sub/main.dta", "(c #include_dir cards)\n");
        LoadSession s;
        s.add_include_dir(c.dir.path(""));
        CHECK(to_string(*ok(read_file(elsewhere, s))) ==
              "((c (fireball (cost 3) (damage 6)) (shield (cost 1))))");
    }
}

BAKER_DEV_TEST_CASE("a Config's reload reads a file added to the directory") {
    Cards c;
    Config cfg;
    REQUIRE(cfg.push_layer("base", c.main));
    CHECK_FALSE(*cfg.reload());
    c.dir.write("cards/frost.dta", "(cost 2)\n");
    REQUIRE(*cfg.reload());
    CHECK(cfg.get<int>("cards.frost.cost") == 2);
    std::filesystem::remove(c.dir.path("cards/frost.dta"));
    REQUIRE(*cfg.reload());
    CHECK_FALSE(cfg.get<int>("cards.frost.cost"));
}

BAKER_DEV_TEST_CASE("write-back edits a record in its own file") {
    Cards c;
    Config cfg({.edit_mode = true});
    REQUIRE(cfg.push_layer("base", c.main));
    REQUIRE(cfg.set("cards.fireball.damage", Node(int64_t{8})));
    REQUIRE(cfg.write_back(Path("cards.fireball.damage")));
    CHECK(c.dir.read("cards/fireball.dta") == "(cost 3)\n(damage 8)\n");
    CHECK(c.dir.read("main.dta").find("#include_dir") != std::string::npos);
}

BAKER_DEV_TEST_CASE("a value added under a folder's table is refused, and nothing written") {
    Cards c;
    c.dir.write("cards/fire/ember.dta", "(cost 2)\n");
    Config cfg({.edit_mode = true});
    REQUIRE(cfg.push_layer("base", c.main));
    // The folder's table is assembled from files, written in none.
    REQUIRE(cfg.set("cards.fire.ember.heat", Node(int64_t{7})));
    const auto in_record = cfg.write_back_to(Path("cards.fire.ember.heat"), "base");
    REQUIRE_FALSE(in_record);
    CHECK(to_string(in_record.error()).find("assembled by #merge or #include_dir") !=
          std::string::npos);
    REQUIRE(cfg.set("cards.fire.blaze", Node(int64_t{1})));
    const auto in_table = cfg.write_back_to(Path("cards.fire.blaze"), "base");
    REQUIRE_FALSE(in_table);
    CHECK(to_string(in_table.error()).find("#include_dir") != std::string::npos);
    CHECK(c.dir.read("cards/fire/ember.dta") == "(cost 2)\n");
    CHECK(c.dir.read("main.dta").find("blaze") == std::string::npos);
}
