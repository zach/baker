// Metadata by spelling: a child tagged '@' and a word is metadata, never a value.
#include <doctest.h>
#include <baker/cache.hpp>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>

#include <array>
#include <iterator>
#include <string>
#include <vector>
#include "dev_only.hpp"
#include "ok.hpp"
#include "temp_dir.hpp"

using namespace baker;
using baker::test::ok;
using baker::test::TempDir;

namespace {
Ref<Array> parse(std::string_view text) {
    return ok(read_string(text, "m.dta"));
}
std::vector<std::string> tags(const Metadata& m) {
    std::vector<std::string> out;
    for (const Array& a : m) out.emplace_back(a.tag().str());
    return out;
}
size_t value_count(const Array& a) {
    return a.values().size();
}
}  // namespace

TEST_CASE("a value ends at its first metadata child, for every reader") {
    const Ref<Array> root = parse("(color 0.9 0.5 0.2 (@range 0 1) (@desc \"Cube color\"))\n");
    const Array& color = *root->try_find("color");
    CHECK(value_count(color) == 3);
    CHECK(color.meta_first() == 4);
    CHECK(tags(color.metadata()) == std::vector<std::string>{"@range", "@desc"});
    REQUIRE(color.meta("@range"));
    CHECK((*color.meta("@range"))[2].try_int() == 1);
    CHECK(!color.meta("@step"));
    CHECK(color.try_find("@desc"));  // metadata is a child: find and paths reach it

    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("d", root));
    const auto rgb = cfg.get<std::array<float, 3>>("color");
    REQUIRE(rgb);
    CHECK((*rgb)[2] == doctest::Approx(0.2));
    CHECK(cfg.get<std::string>("color.@desc") == "Cube color");
}

TEST_CASE("a field may have any name, and follow metadata") {
    // Both orders: no child is metadata, whatever it is called or where it is.
    for (const char* text :
         {"(ball (kind ball) (pos -6 6.5))\n", "(ball (pos -6 6.5) (kind ball))\n"}) {
        const Ref<Array> root = parse(text);
        const Array& ball = *root->try_find("ball");
        CHECK(ball.metadata().empty());
        CHECK(value_count(ball) == 2);
    }
    // Fields before, between and after metadata; only the '@' children are metadata.
    const Ref<Array> root =
        parse("(move (@at fighters * moves) (@keyed) (button symbol) (@desc \"x\") (at 1))\n");
    const Array& move = *root->try_find("move");
    CHECK(value_count(move) == 0);
    CHECK(tags(move.metadata()) == std::vector<std::string>{"@at", "@keyed", "@desc"});
    CHECK(move.try_find("button"));
    CHECK(move.try_find("at"));
}

TEST_CASE("metadata comes after the value: a value after it is a located error") {
    const auto bad = read_string("(a 1)\n(x (@range 0 1) 5)\n", "m.dta");
    REQUIRE(!bad);
    CHECK(bad.error().code() == ErrorCode::Syntax);
    CHECK(bad.error().line() == 2);
    CHECK(bad.error().column() == 1);
    CHECK(std::string(bad.error().message()).find("metadata comes after the value") !=
          std::string::npos);
    // A tuple after metadata is a value too; a field after it is not.
    CHECK(!read_string("(walls (0 1) (@note \"n\") (2 3))\n"));
    CHECK(read_string("(walls (0 1) (@note \"n\") (door 2 3))\n"));
    // A command's forms are positional: no rule there.
    CHECK(read_string("{f (@a 1) 2}\n"));
}

TEST_CASE("a lone '@' is a symbol like any other") {
    // A key bound to the '@' key, as shipped data has it.
    const Ref<Array> root = parse("('@' \"Game: win\" {fake_win 2})\n(k 1 ('@' 2) 3)\n");
    CHECK(!Symbol("@").is_meta());
    CHECK(Symbol("@a").is_meta());
    CHECK(!Symbol("a@").is_meta());
    CHECK(Symbol("@a").str() == "@a");
    CHECK(Symbol("@a").str().size() == 2);
    CHECK(value_count(*root->try_find("k")) == 3);
    // Quoted or not, '@' and a word is metadata: the sigil is the symbol's text.
    CHECK(parse("(k 1 ('@range' 0 1))\n")->try_find("k")->meta_first() == 2);
}

TEST_CASE("the boundary survives sorting, set(), merging and the cache") {
    SUBCASE("sort_for_lookup keeps the values first and the metadata last") {
        Ref<Array> root = parse("(k 3 (z 1) (@range 0 9) (a 2) (@desc \"d\"))\n");
        Array& k = const_cast<Array&>(*root->try_find("k"));
        k.sort_for_lookup();
        CHECK(value_count(k) == 3);  // 3, then the fields a and z
        CHECK(k[1].try_int() == 3);
        CHECK(tags(k.metadata()).size() == 2);
        CHECK(k.try_find("a"));
        CHECK(k.try_find("z"));
        CHECK(k.try_find("@range"));
        CHECK(k.try_find("@desc"));
        CHECK(!k.try_find("@step"));
        // In an untagged array, slot 0 may be tagged '@': no boundary names it, and the
        // sorted search still finds it (a lookup fuzzer input).
        Ref<Array> list = parse("((@range 0 10) (@ssmall 1))\n");
        Array& l = const_cast<Array&>(*(*list)[0].try_array());
        CHECK(l.meta_first() == 1);
        l.sort_for_lookup();
        CHECK(l.try_find("@range"));
        CHECK(l.try_find("@ssmall"));
    }
    SUBCASE("set() moves it when the slot it replaces is or becomes the first") {
        Ref<Array> a = Array::make(4);
        a->set(0, Node(Symbol("k")));
        a->set(1, Node(1));
        a->set(2, Node(2));
        CHECK(a->meta_first() == 0);
        a->set(3, (*parse("(@range 0 9)"))[0]);
        CHECK(a->meta_first() == 3);
        a->set(2, (*parse("(@step 1)"))[0]);
        CHECK(a->meta_first() == 2);
        a->set(2, Node(5));
        CHECK(a->meta_first() == 3);
        a->set(3, Node(6));
        CHECK(a->meta_first() == 0);
        CHECK(value_count(*a) == 3);
    }
    SUBCASE("a layer restating the value keeps the metadata below; #remove removes one") {
        Config cfg({.tweaks = nullptr});
        REQUIRE(cfg.push_layer("d", parse("(w 2 (@range 0 10) (@step 1))\n")));
        REQUIRE(cfg.push_layer("l", parse("(w 3)\n")));
        const Array* w = cfg.root()->try_find("w");
        REQUIRE(w);
        CHECK(value_count(*w) == 1);
        CHECK((*w)[1].try_int() == 3);
        CHECK(w->meta("@range"));
        REQUIRE(cfg.push_layer("r", parse("(w (@step #remove))\n")));
        w = cfg.root()->try_find("w");
        CHECK(w->meta("@range"));
        CHECK(!w->meta("@step"));
        CHECK(cfg.get<int>("w") == 3);
    }
    SUBCASE("a cache keeps it") {
        const Ref<Array> root = parse("(k 1 2 (@range 0 9))\n");
        std::string bytes;
        REQUIRE(write_cache(*root, string_out(bytes)));
        const Ref<Array> back = ok(read_cache_bytes(bytes));
        CHECK(back->try_find("k")->meta_first() == 3);
        CHECK(value_count(*back->try_find("k")) == 2);
    }
}

TEST_CASE("metadata past child 8190 is found by a scan, read or merged") {
    // Up to child 8190 the boundary is kept; past it, a scan finds it.
    for (const int values : {8189, 8190, 8191, 9000}) {
        std::string text = "(k";
        for (int i = 0; i < values; ++i) text += " 1";
        text += " (@range 0 1))\n";
        const auto read = read_string(text, "m.dta");
        REQUIRE(read);
        const Array* k = (*read)->try_find("k");
        CHECK(k->meta_first() == static_cast<uint32_t>(values + 1));
        CHECK(k->values().size() == static_cast<size_t>(values));
        CHECK(k->meta("@range"));
    }
    // A merge that grows a list past the boundary keeps its metadata out of the values.
    std::string base = "(items";
    for (int i = 0; i < 8000; ++i) base += " 1";
    base += " (@desc \"d\"))\n";
    std::string more = "(items #append";
    for (int i = 0; i < 500; ++i) more += " 2";
    more += ")\n";
    Config cfg;
    REQUIRE(cfg.push_layer("base", ok(read_string(base, "b.dta"))));
    REQUIRE(cfg.push_layer("more", ok(read_string(more, "m.dta"))));
    const auto items = cfg.get<std::vector<int>>("items");
    REQUIRE(items);
    CHECK(items->size() == 8500);
}

BAKER_DEV_TEST_CASE("write-back grows a value before its metadata, and inserts after it") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("d", dir.write("d.dta", "(tint 1 0 (@range 0 1) (@note \"n\"))\n")));
    std::vector<Node> three{Node(1), Node(0), Node(0.5)};
    REQUIRE(cfg.set(Path("tint"), std::span<const Node>(three)));
    REQUIRE(cfg.write_back("tint"));
    CHECK(dir.read("d.dta") == "(tint 1 0 0.5 (@range 0 1) (@note \"n\"))\n");
    REQUIRE(cfg.set("tint.alpha", Node(1)));
    REQUIRE(cfg.write_back_to("tint.alpha", "d"));
    const std::string text = dir.read("d.dta");
    CHECK(text.find("(@note \"n\") (alpha 1))") != std::string::npos);
    CHECK(read_string(text, "d.dta"));
}

TEST_CASE("check warns on the old spelling and on metadata Baker does not read") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("d", parse("(base 1)\n")));
    const auto old = cfg.check_layer_text("o", "(a (b 5 (desc \"x\") (range 0 9)))\n");
    REQUIRE(old);
    REQUIRE(old->warnings.size() == 1);
    CHECK(std::string(old->warnings[0].message()).find("write (@desc ...)") != std::string::npos);
    // A record with no value keeps its fields quiet, whatever they are called.
    const auto record = cfg.check_layer_text("r", "(ball (kind ball) (desc \"x\"))\n");
    REQUIRE(record);
    CHECK(record->warnings.empty());
    const auto typo = cfg.check_layer_text("t", "(speed 4 (@rnage 0 9))\n");
    REQUIRE(typo);
    REQUIRE(typo->warnings.size() == 1);
    CHECK(std::string(typo->warnings[0].message()).find("unknown metadata (@rnage") !=
          std::string::npos);
}
