// A name's stable ID: symbol_hash, "x"_hash, and "x"_sym's compile-time hash.
#include <doctest.h>
#include <baker/parse.hpp>
#include <baker/symbol.hpp>

#include <string>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;

// The promise: FNV-1a 64 over the bytes as written.
static_assert(symbol_hash("") == 0xcbf29ce484222325ull);
static_assert(symbol_hash("jump") == 0xeb1776ddf832a4cdull);
static_assert("jump"_hash == 0xeb1776ddf832a4cdull);
static_assert(symbol_hash("\xC3\xA9p\xC3\xA9"
                          "e") == 0x3ae05f984f97964aull);  // "épée"
static_assert("épée"_hash == 0x3ae05f984f97964aull);       // a UTF-8 source, as the build requires

namespace {

const char* action(Symbol name) {
    switch (name.hash()) {
        case "jump"_hash: return "jumping";
        case "land"_hash: return "landing";
        case "épée"_hash: return "fencing";
        default: return "idle";
    }
}

}  // namespace

TEST_CASE("a switch over a symbol's hash finds its name, however it was interned") {
    CHECK(std::string(action("jump"_sym)) == "jumping");  // a literal
    const std::string runtime = std::string("la") + "nd";
    CHECK(std::string(action(Symbol(runtime))) == "landing");  // runtime text
    CHECK(std::string(action(Symbol("\xC3\xA9p\xC3\xA9"
                                    "e"))) == "fencing");
    CHECK(std::string(action(Symbol("crouch"))) == "idle");
    const auto tree = ok(read_string("(moves jump land épée sprint)", "t.dta"));
    std::string seen;
    for (const Node& n : (*tree)[0].try_array()->values())  // names from a data file
        seen += std::string(action(*n.try_sym())) + " ";
    CHECK(seen == "jumping landing fencing idle ");
}

TEST_CASE("the hash is the same at compile time, at run time, and on the Symbol") {
    constexpr uint64_t compiled = symbol_hash("player.speed");
    std::string text = "player.speed";
    CHECK(symbol_hash(text) == compiled);
    CHECK(Symbol(text).hash() == compiled);
    CHECK("player.speed"_sym.hash() == compiled);
    CHECK(Symbol().hash() == symbol_hash(""));
    CHECK(""_sym == Symbol());
    // "x"_sym interns with its compiled hash: the same Symbol as one interned from text.
    CHECK("a name only a literal has interned"_sym ==
          Symbol(std::string("a name only a literal has interned")));
    for (int i = 0; i < 1000; ++i) {
        const std::string name = "n" + std::to_string(i * 7919);
        CHECK(Symbol(name).hash() == symbol_hash(name));
    }
}
