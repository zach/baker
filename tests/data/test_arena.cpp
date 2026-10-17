// A load's tree lives in one arena, counted as a whole.
#include <doctest.h>
#include <baker/array.hpp>
#include <baker/parse.hpp>

#include "alloc_counter.hpp"
#include "ok.hpp"

#include <string>

using namespace baker;
using baker::test::ok;

namespace {
// `n` tagged arrays, each with a string: (k0 "v0") (k1 "v1") ...
std::string many(int n) {
    std::string text;
    for (int i = 0; i < n; ++i)
        text += "(k" + std::to_string(i) + " \"v" + std::to_string(i) + "\" " + std::to_string(i) +
                ")\n";
    return text;
}
}  // namespace

TEST_CASE("a parse allocates blocks, not arrays") {
    if (!test::counting_allocations()) return;
    const std::string text = many(2000);  // 2001 arrays and 2000 strings
    LoadSession session;
    (void)ok(read_string(text, "warm.dta", session));  // interns the symbols
    test::AllocationScope scope;
    auto root = ok(read_string(text, "many.dta", session));
    const size_t parsed = scope.count();
    CHECK(root->size() == 2000);
    CHECK(parsed < 40);  // the arena's blocks, the parser's scratch, and its frames
    MESSAGE("parsing 2001 arrays and 2000 strings: " << parsed << " allocations");
    test::AllocationScope teardown;
    root.reset();
    CHECK(teardown.count() == 0);
}

TEST_CASE("a node outlives its load, and holds all of it") {
    Node kept;
    const Array* first = nullptr;
    {
        auto root = ok(read_string("(a (b 1 \"two\")) (c 3)"));
        first = root->try_find("a");
        kept = (*first)[1];             // (b 1 "two")
        CHECK(root->ref_count() == 2);  // the root's Ref and `kept` count the one arena
        CHECK(first->ref_count() == 2);
    }
    // The load is alive while `kept` is: (a ...) is still readable, through the arena.
    CHECK(kept.array()->ref_count() == 1);
    CHECK(*(*kept.array())[2].try_str() == "two");
    CHECK(first->try_find("b") == kept.array());
}

TEST_CASE("a macro body from an earlier load keeps that load alive") {
    LoadSession session;
    auto defs = ok(read_string("[POINT (x 1) (y \"two\")]", "defs.dta", session));
    const uint32_t before = defs->ref_count();
    auto use = ok(read_string("(p POINT)", "use.dta", session));
    CHECK(defs->ref_count() == before + 1);  // `use`'s arena counts `defs`'s, once
    defs.reset();
    session = LoadSession();  // and the session no longer holds the macro
    const Array& p = *use->try_find("p");
    CHECK(p.size() == 3);
    CHECK(p.find_int("x") == 1);
    CHECK(p.find_str("y") == "two");
}

TEST_CASE("set on a loaded array counts a node from elsewhere once") {
    auto root = ok(read_string("(slot 0 0)"));
    Node slot = (*root)[0];
    const Array* far = nullptr;
    {
        auto other = ok(read_string("(payload \"far\")"));
        far = other->try_find("payload");
        slot.mutable_array()->set(1, (*other)[0]);
        slot.mutable_array()->set(2, (*other)[0]);
        CHECK(other->ref_count() == 2);  // `other`, and `root`'s arena once
    }
    CHECK(slot.array()->try_find("payload") == far);
    CHECK((*far)[1].try_str() == "far");
    CHECK(far->ref_count() == 1);
}

TEST_CASE("arrays built outside a load are solo, as before") {
    Ref<Array> a = array(1, 2);
    Ref<Array> b = array("b", Node(a));
    CHECK(a->ref_count() == 2);
    b.reset();
    CHECK(a->ref_count() == 1);
}

TEST_CASE("#merge builds in the load's arena, so the load is not its own dependency") {
    LoadSession session;
    session.set_reader([](std::string_view path, TextSink out) {
        if (path != "extra.dta") return false;
        out("(window (height 480) (title \"x\"))");
        return true;
    });
    auto root = ok(read_string("(window (width 640))\n#merge extra.dta\n", "main.dta", session));
    CHECK(root->try_find("window")->find_int("height") == 480);
    CHECK(root->ref_count() == 1);  // only `root`: nothing in the load counts the load
}
