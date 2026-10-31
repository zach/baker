// Two names sharing a hash are reported in a build that is not shipping. A real FNV-1a
// collision is impractical here, so detail::intern_hashed plants a name under another's hash.
#include <doctest.h>
#include <baker/error.hpp>
#include <baker/parse.hpp>
#include <baker/symbol.hpp>

#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "ok.hpp"
#include "temp_dir.hpp"

using namespace baker;
using baker::test::ok;

namespace {

std::mutex reports_lock;
std::vector<std::string>& reports() {
    static std::vector<std::string> r;
    return r;
}

void keep(const Error& report) {
    const std::lock_guard lock(reports_lock);
    reports().emplace_back(to_string(report));
}

// Reports go to `keep` for the test's length, and the default comes back after it.
struct Reporting {
    Reporting() {
        reports().clear();
        previous = set_symbol_collision_handler(keep);
    }
    ~Reporting() { set_symbol_collision_handler(previous); }
    SymbolCollisionHandler previous;
};

// `text` interned under `hash`, as if its own hash were that.
Symbol planted(std::string_view text, uint64_t hash) {
    return Symbol::from_id(detail::intern_hashed(text, hash));
}

}  // namespace

TEST_CASE("a name that shares another's hash is reported once, with both names") {
    const Reporting reporting;
    const Symbol first("collision.first");
    const Symbol second = planted("collision.second", first.hash());
    if constexpr (kShipping) {
        CHECK(reports().empty());  // compiled out
    } else {
        REQUIRE(reports().size() == 1);
        CHECK(reports()[0].find("the names 'collision.first' and 'collision.second' share the "
                                "hash 0x") != std::string::npos);
        CHECK(reports()[0].find("code that keys on Symbol::hash() cannot tell them apart") !=
              std::string::npos);
    }
    // Interning either again reports nothing, and both stay distinct, working Symbols.
    CHECK(planted("collision.second", first.hash()) == second);
    CHECK(Symbol("collision.first") == first);
    CHECK(reports().size() == (kShipping ? 0u : 1u));
    CHECK(first != second);
    CHECK(second.str() == "collision.second");
    CHECK(second.hash() == first.hash());
}

TEST_CASE("a colliding name in a data file is reported at its file and line") {
    const Reporting reporting;
    (void)planted("collision.planted", symbol_hash("moon_rock_x"));  // nothing to collide yet
    CHECK(reports().empty());
    const auto tree = ok(read_string("(items\n   sun_stone\n   moon_rock_x)", "items.dta"));
    CHECK((*tree)[0].try_array()->size() == 3);  // the name is still read
    if constexpr (!kShipping) {
        REQUIRE(reports().size() == 1);
        CHECK(reports()[0].find("'collision.planted' and 'moon_rock_x'") != std::string::npos);
        CHECK(reports()[0].find("(items.dta:3)") != std::string::npos);
    } else {
        CHECK(reports().empty());
    }
    // An included file's names are reported at that file, and the includer's at its own line.
    (void)planted("collision.planted2", symbol_hash("star_shard_x"));
    (void)planted("collision.planted3", symbol_hash("comet_dust_x"));
    reports().clear();
    const test::TempDir dir;
    dir.write("inner.dta", "(c star_shard_x)\n");
    const std::string outer = dir.write("outer.dta", "(a\n #include inner.dta\n comet_dust_x)\n");
    LoadSession session;
    (void)ok(read_file(outer, session));
    if constexpr (!kShipping) {
        REQUIRE(reports().size() == 2);
        CHECK(reports()[0].find("'star_shard_x'") != std::string::npos);
        CHECK(reports()[0].find("inner.dta:1)") != std::string::npos);
        CHECK(reports()[1].find("'comet_dust_x'") != std::string::npos);
        CHECK(reports()[1].find("outer.dta:3)") != std::string::npos);
    }
}

TEST_CASE("a collision found while threads intern is reported once") {
    const Reporting reporting;
    const Symbol target("collision.threaded");
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([t, &target] {
            for (int i = 0; i < 2000; ++i)
                (void)Symbol("collision.thread" + std::to_string(t) + "." + std::to_string(i));
            if (t == 0) (void)planted("collision.threaded.twin", target.hash());
        });
    for (std::thread& th : threads) th.join();
    CHECK(reports().size() == (kShipping ? 0u : 1u));
}
