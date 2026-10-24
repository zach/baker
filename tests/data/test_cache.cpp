// The binary cache: a cache load gives the tree the text load gave, as one block.
#include <doctest.h>
#include <baker/cache.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>

#include "alloc_counter.hpp"
#include "dev_only.hpp"
#include "ok.hpp"
#include "temp_dir.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace baker;
using baker::test::ok;
namespace fs = std::filesystem;

namespace {

std::string bytes_of(const Array& root) {
    std::string out;
    ok(write_cache(root, string_out(out)));
    return out;
}

// Structurally equal, and the same in everything else a reader sees: sources, heights,
// lookup order, and which arrays and strings are shared.
void check_same(const Array& a, const Array& b, std::map<const void*, const void*>& seen) {
    auto [it, added] = seen.emplace(&a, &b);
    REQUIRE(it->second == &b);
    if (!added) return;
    REQUIRE(a.size() == b.size());
    CHECK(a.file() == b.file());
    CHECK(a.line() == b.line());
    CHECK(a.height() == b.height());
    CHECK(a.sorted_for_lookup() == b.sorted_for_lookup());
    for (uint32_t i = 0; i < a.size(); ++i) {
        const Node &x = a[i], &y = b[i];
        // The tag run came through: each tag finds the same child in both.
        if (const auto tag = x.try_array() && !x.try_array()->empty()
                                 ? x.try_array()->nodes()[0].try_sym()
                                 : std::nullopt) {
            const Array* in_a = a.try_find(*tag);
            const Array* in_b = b.try_find(*tag);
            REQUIRE((in_a && in_b));
            auto at = [](const Array& in, const Array* found) {
                uint32_t k = 0;
                while (in[k].array() != found) ++k;
                return k;
            };
            CHECK(at(a, in_a) == at(b, in_b));
        }
        REQUIRE(x.type() == y.type());
        if (x.array()) {
            check_same(*x.array(), *y.array(), seen);
        } else if (x.string()) {
            auto [s, fresh] = seen.emplace(x.string(), y.string());
            CHECK(s->second == y.string());
            CHECK(x.string()->view() == y.string()->view());
            CHECK(y.string()->data()[y.string()->size()] == '\0');
        } else {
            CHECK(structurally_equal(x, y));
        }
    }
}

Ref<Array> round_trip(const Array& root) {
    const std::string bytes = bytes_of(root);
    auto back = ok(read_cache_bytes(bytes, "test.datb"));
    std::map<const void*, const void*> seen;
    check_same(root, *back, seen);
    CHECK(structurally_equal(root, *back));
    return back;
}

std::string big_text(size_t bytes) {
    std::string text;
    for (int id = 0; text.size() < bytes; ++id) {
        const std::string n = std::to_string(id);
        text += "(entry_" + n + " (name \"Entry number " + n + "\") (enabled TRUE) (position " + n +
                ".5 -12.25 0.0) (tags fast loud blue) (limits (min 0) (max 100))" +
                " (on_hit {play_sfx \"hit.wav\"} {game add_score $score}))\n";
    }
    return text;
}

}  // namespace

TEST_CASE("every corpus file loads from a cache as it does from text") {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(BAKER_CORPUS_DIR))
        if (e.path().extension() == ".dta") files.push_back(e.path());
    REQUIRE(files.size() >= 8);
    for (const fs::path& p : files) {
        CAPTURE(p.string());
        LoadSession session;
        auto root = ok(read_file(p.generic_string(), session));
        (void)round_trip(*root);
    }
}

TEST_CASE("a cache keeps every kind of value, and what the tree shares") {
    auto root = ok(read_string(R"(
        [SHARED (one 1) (two 2.5)]
        (a SHARED) (b SHARED)
        (values -7 0x7FFFFFFFFFFFFFFF -0.0 1e300 '' 'odd sym' "" "text" $var [prop] {cmd 1})
        (empty ())
        (nested (((deep))))
    )",
                               "kinds.dta"));
    auto back = round_trip(*root);
    // The macro's arrays are shared by both expansions, in the cache as in the text load.
    CHECK((*back->try_find("a"))[1].array() == (*back->try_find("b"))[1].array());

    SUBCASE("a sorted array stays sorted") {
        auto sorted = ok(read_string("(z 1) (a 2) (m 3)", "sorted.dta"));
        sorted->sort_for_lookup();
        auto again = round_trip(*sorted);
        CHECK(again->sorted_for_lookup());
        CHECK(*again->find_int("m") == 3);
    }
}

TEST_CASE("a cache-loaded tree is one block") {
    const std::string text = big_text(200000);
    LoadSession session;
    auto root = ok(read_string(text, "big.dta", session));
    const std::string bytes = bytes_of(*root);
    (void)ok(read_cache_bytes(bytes, "warm.datb", session));  // interns the symbols
    if (test::counting_allocations()) {
        test::AllocationScope scope;
        auto back = ok(read_cache_bytes(bytes, "big.datb", session));
        CHECK(scope.count() == 1);
        test::AllocationScope teardown;
        back.reset();
        CHECK(teardown.count() == 0);
    }
    // Any reference keeps the whole block alive, as in a text load.
    auto back = ok(read_cache_bytes(bytes, "big.datb"));
    Ref<Array> inner(const_cast<Array*>(back->try_get("entry_7.limits.max")));
    const uint32_t refs = back->ref_count();
    back.reset();
    CHECK(inner->ref_count() == refs - 1);
    CHECK((*inner)[1].try_int() == 100);
}

TEST_CASE("a cache-loaded tree can be changed and freed") {
    auto root = ok(read_string("(a 1) (b \"two\")", "set.dta"));
    auto back = round_trip(*root);
    auto other = ok(read_string("(x \"elsewhere\")", "other.dta"));
    auto* a = const_cast<Array*>(back->try_find("a"));
    a->set(1, (*other)[0]);  // depends on the other load now
    other.reset();
    CHECK(to_string(*back) == "((a (x \"elsewhere\")) (b \"two\"))");
}

TEST_CASE("writing refuses what a parse never makes") {
    auto root = Array::make(2, "made.dta", 3);
    root->set(0, Node(Symbol("handle")));
    root->set(1, Node::object(ObjectHandle::make(1, 1)));
    Ref<Array> outer = Array::make(1);
    outer->set(0, Node(root));
    std::string out;
    auto r = write_cache(*outer, string_out(out));
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Refused);
    CHECK(to_string(r.error()).find("made.dta:3") != std::string::npos);
    CHECK(out.empty());
}

TEST_CASE("a damaged or foreign cache is an error, never a crash") {
    auto root = ok(read_string("(a 1 \"s\" sym) (b (c))", "small.dta"));
    const std::string good = bytes_of(*root);
    auto fails = [](const std::string& bytes, ErrorCode code, const char* says) {
        auto r = read_cache_bytes(bytes, "bad.datb");
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == code);
        CHECK_MESSAGE(to_string(r.error()).find(says) != std::string::npos, to_string(r.error()));
    };
    fails("", ErrorCode::Io, "not a Baker cache");
    fails("(a 1)", ErrorCode::Io, "not a Baker cache");
    fails(good.substr(0, good.size() - 8), ErrorCode::Io, "damaged");
    for (size_t at : {good.size() / 2, good.size() - 1, size_t{80}}) {
        std::string flipped = good;
        flipped[at] = static_cast<char>(flipped[at] ^ 0x10);
        fails(flipped, ErrorCode::Io, "damaged");
    }
    // The header's offsets too: a damaged root once loaded another array as the tree.
    for (size_t at = 24; at < 64; at += 4) {
        std::string flipped = good;
        flipped[at] = static_cast<char>(flipped[at] ^ 0x40);
        fails(flipped, ErrorCode::Io, "damaged");
    }
    std::string version = good;
    version[10] = 99;
    fails(version, ErrorCode::Refused, "format version 99");
    std::string endian = good;
    std::swap(endian[12], endian[15]);
    fails(endian, ErrorCode::Refused, "endianness");
}

TEST_CASE("an edit changes the cache near it, for delta patches") {
    // Sections whose symbols and strings all recur, so the insert below adds no name.
    std::string before, after;
    for (int i = 0; i < 60; ++i) {
        const std::string section =
            "(item (name \"sword\") (damage " + std::to_string(i) + ") (tags sharp heavy))\n";
        before += section;
        after += section;
        if (i == 30) after += "(item (name \"sword\") (damage 99) (tags heavy))\n";
    }
    const std::string a = bytes_of(*ok(read_string(before, "items.dta")));
    const std::string b = bytes_of(*ok(read_string(after, "items.dta")));
    REQUIRE(b.size() > a.size());
    // What a binary diff sees: nearly every 32 bytes of the old cache lie somewhere in the
    // new one, shifted; the header and the edit's neighbours differ. (Format 5, with absolute
    // offsets, kept 289 of 672.)
    size_t blocks = 0, kept = 0;
    for (size_t at = 64; at + 32 <= a.size(); at += 32, ++blocks)
        if (b.find(a.substr(at, 32)) != std::string::npos) ++kept;
    CHECK_MESSAGE(kept + 8 >= blocks, kept << " of " << blocks);
}

TEST_CASE("names that differ only by length and a first byte hash apart") {
    // '' and '\x01' once hashed alike, and the write refused them (found by the fuzzer).
    Ref<Array> root = Array::make(3, "names.dta", 1);
    root->set(0, Node(Symbol("")));
    root->set(1, Node(Symbol(std::string_view("\x01", 1))));
    root->set(2, Node(Symbol(std::string_view("\x02\x00", 2))));
    auto back = round_trip(*root);
    CHECK(structurally_equal(*root, *back));
}

TEST_CASE("a cache loads a tree deeper than the loader keeps on its stack") {
    // 300 deep, past the loader's 256 inline frames; walked without recursion, which MSVC's
    // debug stack would not take.
    constexpr int kDepth = 300;
    Ref<Array> root = Array::make(1, "deep.dta", 1);
    Array* at = root.get();
    for (int i = 0; i < kDepth; ++i) {
        Ref<Array> child = Array::make(2, "deep.dta", i + 2);
        child->set(0, Node(Symbol("d")));
        at->set(at->size() - 1, Node(child));
        at = child.get();
    }
    at->set(1, Node(String::make("bottom")));
    auto back = ok(read_cache_bytes(bytes_of(*root)));
    const Array* walk = back.get();
    for (int i = 0; i < kDepth; ++i) {
        walk = (*walk)[walk->size() - 1].array();
        REQUIRE(walk);
        CHECK(walk->line() == i + 2);
        CHECK(walk->tag() == Symbol("d"));
    }
    CHECK((*walk)[1].string()->view() == "bottom");
}

TEST_CASE("caches on disk, and through a session's reader") {
    test::TempDir dir;
    const std::string text = "(menu (bacon (price 12.75)))";
    auto root = ok(read_string(text, "menu.dta"));
    const std::string path = dir.path("menu.datb");
    ok(write_cache(*root, path));

    LoadSession session;
    auto back = ok(read_cache(path, session));
    CHECK(*back->get<double>("menu.bacon.price") == 12.75);
    CHECK(back->file() == Symbol("menu.dta"));

    // A custom reader is used as for text.
    const std::string bytes = dir.read("menu.datb");
    LoadSession custom;
    custom.set_reader([&](std::string_view, TextSink out) {
        out(bytes);
        return true;
    });
    auto served = ok(read_cache("anywhere.datb", custom));
    CHECK(structurally_equal(*served, *root));

    auto missing = read_cache(dir.path("missing.datb"));
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code() == ErrorCode::Io);
    auto unwritable = write_cache(*root, dir.path("no/such/dir/x.datb"));
    REQUIRE_FALSE(unwritable);
    CHECK(unwritable.error().code() == ErrorCode::Io);
}

TEST_CASE("read_cached uses a fresh cache and rewrites a stale one") {
    test::TempDir dir;
    const std::string main =
        dir.write("main.dta", "#include inc.dta\n#include_opt local.dta\n(a 1)");
    dir.write("inc.dta", "(b 2)");
    const std::string cache = dir.path("main.datb");

    // Baked, as baker_bake does: read_cached writes no cache in shipping builds.
    LoadSession first;
    first.record_for_cache = true;
    auto root = ok(read_file(main, first));
    ok(write_cache(*root, cache, &first));
    CHECK(to_string(*root) == "((b 2) (a 1))");
    LoadSession reuse;
    CHECK(structurally_equal(*root, *ok(read_cached(main, cache, reuse))));
    LoadSession again;
    CHECK(cache_fresh(cache, again));

    if constexpr (!kShipping) {
        SUBCASE("an included file changes") {
            dir.write("inc.dta", "(b 3)");
            CHECK_FALSE(cache_fresh(cache, again));
            CHECK(to_string(*ok(read_cached(main, cache, again))) == "((b 3) (a 1))");
            LoadSession third;
            CHECK(cache_fresh(cache, third));
        }
        SUBCASE("an optional file that was missing appears") {
            dir.write("local.dta", "(c 4)");
            CHECK_FALSE(cache_fresh(cache, again));
            CHECK(to_string(*ok(read_cached(main, cache, again))) == "((b 2) (c 4) (a 1))");
        }
        SUBCASE("the session's defines differ") {
            LoadSession debug;
            debug.define("DEBUG");
            CHECK_FALSE(cache_fresh(cache, debug));
        }
    } else {
        SUBCASE("shipping trusts the cache") {
            dir.write("inc.dta", "(b 3)");
            CHECK(to_string(*ok(read_cached(main, cache, again))) == "((b 2) (a 1))");
        }
    }

    // A cache written without a recorded load cannot say whether it is fresh.
    ok(write_cache(*root, dir.path("bare.datb")));
    CHECK_FALSE(cache_fresh(dir.path("bare.datb"), again));
}

TEST_CASE("a cache baked for other macros is not used, in shipping too") {
    // A DLC baked against a base whose macro a title update then changed.
    test::TempDir dir;
    const std::string dlc = dir.write("dlc/axe.dta", "(axe (damage DMG))");
    const std::string cache = dir.path("axe.datb");
    auto session_with = [](std::string_view dmg) {
        LoadSession s;
        s.define("DMG", ok(read_string(dmg, "base.dta")));
        return s;
    };
    LoadSession bake = session_with("10");
    bake.record_for_cache = true;
    ok(write_cache(*ok(read_file(dlc, bake)), cache, &bake));

    LoadSession same = session_with("10");
    CHECK(to_string(*ok(read_cached(dlc, cache, same))) == "((axe (damage 10)))");
    LoadSession patched = session_with("12");  // the title update's macro
    CHECK(to_string(*ok(read_cached(dlc, cache, patched))) == "((axe (damage 12)))");

    // Without the text, a cache for another session is refused rather than used.
    if constexpr (kShipping) {  // development would have rewritten the cache above
        std::filesystem::remove(dlc);
        LoadSession again = session_with("12");
        auto r = read_cached(dlc, cache, again);
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == ErrorCode::Refused);
        CHECK(std::string(r.error().message()).find("was baked for other macros") !=
              std::string::npos);
        LoadSession matching = session_with("10");
        CHECK(to_string(*ok(read_cached(dlc, cache, matching))) == "((axe (damage 10)))");
    }
}

TEST_CASE("shipping says why it read the text instead of a cache") {
    if constexpr (kShipping) {  // development rebuilds caches silently
        test::TempDir dir;
        const std::string text = dir.write("a.dta", "(a A)");
        const std::string cache = dir.path("a.datb");
        auto warned = [](const LoadSession& s, std::string_view what) {
            for (const Error& w : s.warnings())
                if (std::string(w.message()).find(what) != std::string::npos) return true;
            return false;
        };
        LoadSession none;
        CHECK(to_string(*ok(read_cached(text, cache, none))) == "((a A))");
        CHECK(warned(none, "no usable cache"));

        LoadSession bake;
        bake.define("A", ok(read_string("1", "d.dta")));
        bake.record_for_cache = true;
        ok(write_cache(*ok(read_file(text, bake)), cache, &bake));
        LoadSession other;
        other.define("A", ok(read_string("2", "d.dta")));
        CHECK(to_string(*ok(read_cached(text, cache, other))) == "((a 2))");
        CHECK(warned(other, "was baked for other macros"));

        std::filesystem::remove(text);
        std::filesystem::remove(cache);
        LoadSession gone;
        auto r = read_cached(text, cache, gone);
        REQUIRE_FALSE(r);
        CHECK(std::string(r.error().message()).find("no usable cache") != std::string::npos);
    }
}

TEST_CASE("a text-only session refuses caches and reads the text") {
    test::TempDir dir;
    const std::string text = dir.write("mod.dta", "(mod (speed 9))");
    const std::string cache = dir.path("mod.datb");
    LoadSession bake;
    bake.record_for_cache = true;
    auto root = ok(read_file(text, bake));
    ok(write_cache(*root, cache, &bake));
    std::string bytes;
    ok(write_cache(*root, string_out(bytes), &bake));

    LoadSession mods;
    mods.text_only = true;
    auto from_file = read_cache(cache, mods);
    REQUIRE_FALSE(from_file);
    CHECK(from_file.error().code() == ErrorCode::Refused);
    CHECK(std::string(from_file.error().message()).find("reads text only") != std::string::npos);
    auto from_bytes = read_cache_bytes(bytes, "mod.datb", mods);
    REQUIRE_FALSE(from_bytes);
    CHECK(from_bytes.error().code() == ErrorCode::Refused);
    CHECK(to_string(*ok(read_cached(text, cache, mods))) == "((mod (speed 9)))");
    LoadSession copy = mods;  // an option, carried by copies
    CHECK_FALSE(read_cache(cache, copy));
}

BAKER_DEV_TEST_CASE("a define's body decides freshness, by structure") {
    test::TempDir dir;
    const std::string main = dir.write("main.dta", "(a BODY)");
    const std::string cache = dir.path("main.datb");
    auto session_with = [](std::string_view body) {
        LoadSession s;
        s.define("BODY", ok(read_string(body, "d.dta")));
        return s;
    };
    LoadSession first = session_with("(x 1) (y \"s\" 2.5)");
    first.record_for_cache = true;
    ok(write_cache(*ok(read_file(main, first)), cache, &first));
    LoadSession same = session_with("(x 1) (y \"s\" 2.5)");  // another tree, equal
    CHECK(cache_fresh(cache, same));
    LoadSession number = session_with("(x 2) (y \"s\" 2.5)");
    CHECK_FALSE(cache_fresh(cache, number));
    LoadSession string = session_with("(\"x\" 1) (y \"s\" 2.5)");  // a string, not a symbol
    CHECK_FALSE(cache_fresh(cache, string));
    LoadSession nesting = session_with("((x 1) (y \"s\" 2.5))");
    CHECK_FALSE(cache_fresh(cache, nesting));
}

TEST_CASE("a cache load leaves the session as the text load did") {
    test::TempDir dir;
    const std::string main = dir.write("main.dta", R"(
        #define FLAG
        #define BODY (x 1)
        #undef GONE
        #ifdef PRE (pre yes) #endif
        #autorun {print "hello"}
        (a BODY)
    )");
    const std::string cache = dir.path("main.datb");
    auto fresh_session = [] {
        LoadSession s;
        s.define("PRE");
        s.define("GONE");
        return s;
    };

    LoadSession text = fresh_session();
    text.record_for_cache = true;
    auto from_text = ok(read_file(main, text));
    ok(write_cache(*from_text, cache, &text));

    LoadSession cached = fresh_session();
    CHECK(cache_fresh(cache, cached));
    auto from_cache = ok(read_cache(cache, cached));
    CHECK(structurally_equal(*from_text, *from_cache));
    for (const char* name : {"FLAG", "BODY", "GONE", "PRE"}) {
        CAPTURE(name);
        CHECK(cached.is_defined(name) == text.is_defined(name));
    }
    REQUIRE(cached.macro("BODY"));
    CHECK(structurally_equal(*cached.macro("BODY"), *text.macro("BODY")));
    REQUIRE(cached.autorun().size() == 1);
    CHECK(to_string(cached.autorun()[0]) == "{print \"hello\"}");
    // The replayed macro keeps the cache's tree alive after the tree is dropped.
    from_cache.reset();
    CHECK(to_string(*cached.macro("BODY")) == "(x 1)");
}

TEST_CASE("a Cipher enciphers what it writes and deciphers what it reads") {
    // Not a cipher anyone should ship: the hook is what matters.
    Cipher x{[](void*, std::span<std::byte> b) {
                 for (std::byte& c : b) c ^= std::byte{0x5A};
             },
             [](void*, std::span<std::byte> b) {
                 for (std::byte& c : b) c ^= std::byte{0x5A};
             },
             nullptr};
    auto root = ok(read_string("(secret \"cheat codes\")", "secret.dta"));
    LoadSession with;
    with.cipher = &x;
    std::string bytes;
    ok(write_cache(*root, string_out(bytes), &with));
    CHECK(bytes.find("cheat codes") == std::string::npos);
    CHECK(structurally_equal(*root, *ok(read_cache_bytes(bytes, "secret.datb", with))));

    auto without = read_cache_bytes(bytes, "secret.datb");
    REQUIRE_FALSE(without);
    CHECK(without.error().code() == ErrorCode::Refused);
    CHECK(to_string(without.error()).find("Cipher") != std::string::npos);
}

TEST_CASE("a cache records the files #require read, and which it found already read") {
    test::TempDir dir;
    dir.write("pack.dta", "#define SPEED (3)\n(pack_read)");
    const std::string main = dir.write("main.dta", "#require pack.dta\n(a SPEED)");
    const std::string cache = dir.path("main.datb");
    LoadSession text;
    text.record_for_cache = true;
    ok(write_cache(*ok(read_file(main, text)), cache, &text));

    // Read from the cache, the pack counts as required, as after the text load.
    LoadSession cached;
    auto tree = ok(read_cache(cache, cached));
    CHECK(to_string(*tree) == "((pack_read) (a 3))");
    CHECK(to_string(*ok(read_string("#require pack.dta\n(b SPEED)", dir.path("x.dta"), cached))) ==
          "((b 3))");
    // A session that has already required the pack would splice nothing: the cache is stale
    // for it.
    CHECK_FALSE(cache_fresh(cache, cached));
    LoadSession clean;
    CHECK(cache_fresh(cache, clean));
}

TEST_CASE("a cache replays packs, and is stale for other pack levels") {
    test::TempDir dir;
    dir.write("task.dta", "(pack boss_task (level task))\n#define PHASES (3)");
    const std::string main = dir.write("main.dta", "#require task.dta\n(a PHASES)");
    const std::string cache = dir.path("main.datb");
    LoadSession text;
    text.set_pack_levels({"game", "task"});
    text.record_for_cache = true;
    ok(write_cache(*ok(read_file(main, text)), cache, &text));

    LoadSession cached;
    cached.set_pack_levels({"game", "task"});
    REQUIRE(cache_fresh(cache, cached));
    (void)ok(read_cache(cache, cached));
    const Pack* p = cached.pack_of(Symbol(dir.path("task.dta")));
    REQUIRE(p);
    CHECK(p->rank == 1);
    // The replayed macro keeps its pack: a game pack using it is warned about.
    const std::string game = dir.write("game.dta", "(pack rules (level game))\n(b PHASES)");
    (void)ok(read_file(game, cached));
    CHECK(cached.warnings().size() == 1);

    LoadSession other;
    other.set_pack_levels({"task", "game"});
    CHECK_FALSE(cache_fresh(cache, other));
}

TEST_CASE("a cache replays packs' harness metadata and files' origins") {
    test::TempDir dir;
    dir.write("task.dta",
              "(pack boss (level task) (owners \"td-a\") (for designers) (stage genesis))\n"
              "(origin \"bosses.xlsx\" (tool export))\n(a 1)");
    const std::string main = dir.write("main.dta", "#require task.dta\n(b 2)");
    const std::string cache = dir.path("main.datb");
    LoadSession text;
    text.set_pack_levels({"task"});
    text.record_for_cache = true;
    ok(write_cache(*ok(read_file(main, text)), cache, &text));

    LoadSession cached;
    cached.set_pack_levels({"task"});
    (void)ok(read_cache(cache, cached));
    const Symbol task(dir.path("task.dta"));
    const Pack* p = cached.pack_of(task);
    REQUIRE(p);
    REQUIRE(p->owners.size() == 1);
    CHECK(p->owners[0] == Symbol("td-a"));
    REQUIRE(p->audience.size() == 1);
    CHECK(p->audience[0] == Symbol("designers"));
    CHECK(p->stage == Symbol("genesis"));
    const Origin* o = cached.origin_of(task);
    REQUIRE(o);
    CHECK(o->source == Symbol("bosses.xlsx"));
    CHECK(o->tool == Symbol("export"));
}
