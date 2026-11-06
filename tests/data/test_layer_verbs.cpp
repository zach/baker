// Layer verbs beside #remove: (key #append v...), (key #drop v...), and the (@was ...) pin.
#include <doctest.h>
#include <baker/cache.hpp>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

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
    return ok(read_string(text, "layer.dta"));
}

std::string merged(std::string_view base, std::string_view over) {
    return to_string(*merge(*parse(base), *parse(over)));
}

const LayerEffectEntry* entry(const List<LayerEffectEntry>& effect, const char* path) {
    for (const LayerEffectEntry& e : effect)
        if (e.path == Symbol(path)) return &e;
    return nullptr;
}

bool has(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

std::vector<std::string> messages(const List<Error>& errors) {
    std::vector<std::string> out;
    for (const Error& e : errors) out.emplace_back(e.message());
    return out;
}

}  // namespace

TEST_CASE("the verbs read in their slot, with metadata after, and print back bare") {
    for (const char* text :
         {"(items #append rope torch)", "(things #drop (phone -2 4.6 180))",
          "(menu (debug_menu #remove))", "(debug_menu #remove (@was 1))",
          "(cancels #append palm (@note \"for the combo test\") (@was jab low_kick))",
          "(w (@step #remove))"}) {
        auto tree = parse(text);
        INFO(text);
        CHECK(tree->has_marker());
        CHECK(to_string(*tree) == "(" + std::string(text) + ")");
    }
    // A pin alone flags its row: a check of the layer reads it.
    CHECK(parse("(reaction 26 (@was 30))")->has_marker());
    // Elsewhere a verb's symbol is quoted, so it reads back as the symbol it is.
    for (const char* text : {"('#append')", "(a 1 '#append')", "{f '#drop' 1}", "(a '#append')",
                             "(a (b 1 '#drop'))", "('#append' (k #remove))", "(x [p '#remove'])"}) {
        auto tree = ok(read_string(text, "q.dta"));
        const std::string printed = to_dta(*tree);
        INFO(text);
        CHECK(has(printed, "'#"));
        CHECK(structurally_equal(*tree, *ok(read_string(printed, "back.dta"))));
    }
    // A quoted verb in the slot is the verb, as a quoted '#remove' is: it prints bare.
    auto quoted = ok(read_string("(a '#append' (b #remove))", "q.dta"));
    CHECK(to_dta(*quoted) == "(a #append (b #remove))\n");
    // An array built in C++ is flagged where the verb is made: by make_from and by set().
    std::vector<Node> run{Node(Symbol("items")), Node(Symbol("#append")), Node(Symbol("rope"))};
    CHECK(Array::make_from(run)->has_marker());
    Ref<Array> built = Array::make(3);
    built->set(0, Node(Symbol("items")));
    built->set(2, Node(Symbol("rope")));
    CHECK_FALSE(built->has_marker());
    built->set(1, Node(Symbol("#append")));
    CHECK(built->has_marker());
    std::vector<Node> outer{Node(built)};
    CHECK(to_string(*merge(*parse("(items a)"), *Array::make_from(outer))) == "((items a rope))");
    // Data without a verb or pin carries no flag: the merge and the tools pay one test.
    CHECK_FALSE(parse("(items a b) (x (y 1 2 (@range 0 9)))")->has_marker());
}

TEST_CASE("a misplaced verb is a located Directive error with its form as the hint") {
    struct Case {
        const char* text;
        const char* hint;
    };
    const Case cases[] = {
        {"(a #append)", "(key #append v...)"},
        {"(a 1 #append 2)", "(key #append v...)"},
        {"(#append 1)", "(key #append v...)"},
        {"{f #append 1}", "(key #append v...)"},
        {"#drop", "(key #drop v...)"},
        {"(a #drop)", "(key #drop v...)"},
        {"(a #drop (@note \"x\"))", "(key #drop v...)"},
        {"(a #remove 1)", "(key #remove)"},
        {"(a #append #drop 1)", "(key #drop v...)"},
        {"[p #remove]", "(key #remove)"},
        {"((b 1) #append 2)", "(key #append v...)"},
    };
    for (const Case& c : cases) {
        auto r = read_string(c.text, "bad.dta");
        INFO(c.text);
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == ErrorCode::Directive);
        CHECK(r.error().line() == 1);
        INFO(r.error().message());
        CHECK(has(r.error().message(), c.hint));
    }
    // A macro's body may hold a row with a verb; it cannot splice a bare verb into a row.
    CHECK(to_string(*parse("[ADD (items #append rope)]\nADD")) == "((items #append rope))");
    CHECK_FALSE(read_string("[V #append]\n(items V rope)", "m.dta"));
}

TEST_CASE("#append composes in order, before the metadata; a key below is made from operands") {
    // Two packages extend one list, and both survive (content-updates.md scenario 2).
    CHECK(merged("(items climbing_gloves glider lamp_oil spyglass)", "(items #append rope)") ==
          "((items climbing_gloves glider lamp_oil spyglass rope))");
    CHECK(merged("(items a b (@note \"x\"))", "(items #append torch (sub 1))") ==
          "((items a b torch (sub 1) (@note \"x\")))");
    // The row's metadata merges as keys: a note and a pin ride along, and lookups read past.
    const Ref<Array> m = merge(*parse("(cancels jab (@desc \"d\"))"),
                               *parse("(cancels #append palm (@note \"n\") (@was jab))"));
    CHECK(to_string(*m) == "((cancels jab palm (@desc \"d\") (@note \"n\") (@was jab)))");
    CHECK(m->try_find("cancels")->values().size() == 2);
    // A key the stack lacks: #append creates it, #drop and #remove add nothing, and no verb
    // reaches the tree, at any depth or inside an operand.
    CHECK(merged("(x 1)", "(items #append rope) (gone #drop rope) (y 2 (@was 3))") ==
          "((x 1) (items rope) (y 2 (@was 3)))");
    CHECK(merged("(x 1)", "(k (a (b #append 1) (c #drop 2) (d #remove)))") ==
          "((x 1) (k (a (b 1))))");
    const Ref<Array> op = merge(*parse("(items a)"), *parse("(items #append (t (u #remove)) z)"));
    CHECK(to_string(*op) == "((items a (t) z))");
    // Where a plain row merges into the first of two same-tagged tuples, #append adds a third:
    // Hearth's farmhouse has two beds.
    CHECK(merged("(things (bed 6 -4.6 0) (bed 6 4.6 180))", "(things (bed 1 1 0))") ==
          "((things (bed 1 1 0) (bed 6 4.6 180)))");
    CHECK(merged("(things (bed 6 -4.6 0) (bed 6 4.6 180))", "(things #append (bed 1 1 0))") ==
          "((things (bed 6 -4.6 0) (bed 6 4.6 180) (bed 1 1 0)))");
}

TEST_CASE("#drop removes the first structurally equal value, tuples restated as written") {
    CHECK(merged("(things (fridge -6.3 -4.2 0) (phone -2 4.6 180) (bed 1 2 3))",
                 "(things #drop (phone -2 4.6 180))") ==
          "((things (fridge -6.3 -4.2 0) (bed 1 2 3)))");
    CHECK(merged("(things (phone -2 4.6 180))", "(things #drop (phone -2 4.6 0) lamp)") ==
          "((things (phone -2 4.6 180)))");
    CHECK(merged("(things (bed 6 -4.6 0) (bed 6 4.6 180) (bed 6 -4.6 0))",
                 "(things #drop (bed 6 -4.6 0))") == "((things (bed 6 4.6 180) (bed 6 -4.6 0)))");
    CHECK(merged("(cancels jab low_kick palm)", "(cancels #drop low_kick jab)") ==
          "((cancels palm))");
    // Never a metadata child, which is not a value.
    CHECK(merged("(c a (@note \"a\"))", "(c #drop a)") == "((c (@note \"a\")))");
    // #drop empties a list, which a restated row cannot.
    CHECK(merged("(cancels palm)", "(cancels #drop palm)") == "((cancels))");
}

TEST_CASE("#merge skips the verbs as it skips #remove; a cache keeps the flag") {
    TempDir dir;
    dir.write("over.dta", "(a (b #append 2) (c #drop 1) (d #remove) (f 7))");
    const std::string main = dir.write("main.dta", "(a (b 1) (c 1))\n#merge over.dta\n");
    CHECK(to_string(*ok(read_file(main))) == "((a (b 1) (c 1) (f 7)))");

    auto layer = parse("(items #append rope)\n(cpu (easy (reaction 26 (@was 30))))");
    std::string bytes;
    REQUIRE(write_cache(*layer, string_out(bytes)));
    auto cached = ok(read_cache_bytes(bytes, "layer.datb"));
    CHECK(cached->has_marker());
    CHECK(to_string(*merge(*parse("(items a)\n(cpu (easy (reaction 30)))"), *cached)) ==
          "((items a rope) (cpu (easy (reaction 26 (@was 30)))))");
}

TEST_CASE("a verb is spelled in a row, and a key's verb rows stay apart in order") {
    CHECK(to_string(*parse("(/fighters.kestrel.moves.jab.cancels #append palm (@note \"n\"))")) ==
          "((fighters (kestrel (moves (jab (cancels #append palm (@note \"n\")))))))");
    const Ref<Array> rows = parse(
        "(/households.cottage.things #drop (phone -2 4.6 180))\n"
        "(/households.cottage.things #append (lamp -2 4 0))\n"
        "(/households.cottage.name \"Cottage\")\n");
    CHECK(to_string(*rows) ==
          "((households (cottage (things #drop (phone -2 4.6 180)) (things #append (lamp -2 4 0)) "
          "(name \"Cottage\"))))");
    CHECK(to_string(*merge(*parse("(households (cottage (things (phone -2 4.6 180) (bed 1))))"),
                           *rows)) ==
          "((households (cottage (things (bed 1) (lamp -2 4 0)) (name \"Cottage\"))))");
    // A removal still joins with nothing.
    CHECK_FALSE(read_string("(/a.b #remove)\n(/a.b 1)\n", "r.dta"));
}

TEST_CASE("two packages' #append compose in stack order, placement included") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer(
        "base", dir.write("base.dta", "(items climbing_gloves glider lamp_oil spyglass)\n")));
    REQUIRE(cfg.push_layer("dlc_a", dir.write("dlc_a.dta", "(items #append rope)\n")));
    REQUIRE(cfg.push_layer("dlc_b", dir.write("dlc_b.dta", "(items #append torch)\n")));
    CHECK(to_string(*cfg.root()) ==
          "((items climbing_gloves glider lamp_oil spyglass rope torch))");
    // The value's source is the list's own layer: a composing row supplies none.
    CHECK(ok(cfg.source("items")).layer == Symbol("base"));
    REQUIRE(cfg.remove_layer("dlc_a"));
    CHECK(to_string(*cfg.root()) == "((items climbing_gloves glider lamp_oil spyglass torch))");
    REQUIRE(cfg.push_layer("dlc_a", dir.path("dlc_a.dta"), {.below = "dlc_b"}));
    CHECK(to_string(*cfg.root()) ==
          "((items climbing_gloves glider lamp_oil spyglass rope torch))");
    REQUIRE(cfg.push_layer("patch", dir.write("patch.dta", "(items #drop glider rope)\n"),
                           {.below = "dlc_a"}));
    CHECK(to_string(*cfg.root()) == "((items climbing_gloves lamp_oil spyglass rope torch))");
    // A restated list above them all still replaces the whole list.
    REQUIRE(cfg.push_layer("fix", dir.write("fix.dta", "(items lamp_oil)\n")));
    CHECK(to_string(*cfg.root()) == "((items lamp_oil))");
}

TEST_CASE("the check reports each verb, a stale pin, and operands that change nothing") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer(
        "default", dir.write("default.dta",
                             "(cpu (easy (reaction 30) (block 0.2)))\n(menu (debug 1))\n"
                             "(things (fridge -6.3 -4.2 0) (phone -2 4.6 180))\n(items a b c)\n")));
    const char* agent =
        "(cpu (easy (reaction 26 (@note \"easy blocked the first low kick too often\") (@was "
        "30))))\n"
        "(menu (debug #remove (@was 1)))\n";
    const LayerReport fresh = ok(cfg.check_layer_text("agent", agent));
    CHECK(fresh.warnings.empty());
    for (const LayerEffectEntry& e : fresh.effect) CHECK_FALSE(e.stale);
    CHECK(entry(fresh.effect, "menu.debug")->op == Symbol("#remove"));
    CHECK(to_string(entry(fresh.effect, "cpu.easy.reaction")->values) == "26");

    Config moved({.tweaks = nullptr});
    REQUIRE(moved.push_layer(
        "default",
        dir.write("moved.dta", "(cpu (easy (reaction 32) (block 0.2)))\n(menu (debug 2))\n")));
    const LayerReport stale = ok(moved.check_layer_text("agent", agent));
    CHECK(messages(stale.warnings) ==
          std::vector<std::string>{
              "'cpu.easy.reaction' was 30 when this row was written; the stack has 32",
              "'menu.debug' was 1 when this row was written; the stack has 2"});
    CHECK(stale.warnings[1].line() == 2);
    CHECK(stale.ok());  // a warning, unless the policy says warnings refuse
    CHECK(entry(stale.effect, "cpu.easy.reaction")->stale);
    const LayerPolicy refuse{.warnings_refuse = true};
    CHECK_FALSE(ok(moved.check_layer_text("agent", agent, refuse)).ok());
    CHECK(ok(cfg.check_layer_text("agent", agent, refuse)).ok());
    // A pinned key the stack lacks now.
    CHECK(messages(ok(cfg.check_layer_text("x", "(cpu (hard (reaction 9 (@was 10))))")).warnings) ==
          std::vector<std::string>{
              "'cpu.hard.reaction' was 10 when this row was written; the stack has none"});

    // Both rows of a key are in the effect; a drop that matches nothing, and an append of what
    // is there (a layer left behind after its promotion), warn at their rows.
    const char* pack =
        "(things #drop (phone -2 4.6 180))\n(things #append (lamp -2 4 0))\n(items #drop z b)\n"
        "(items #append a)\n";
    const LayerReport r = ok(cfg.check_layer_text("pack", pack));
    REQUIRE(r.effect.size() == 4);
    CHECK(r.effect[0].op == Symbol("#drop"));
    CHECK(r.effect[1].op == Symbol("#append"));
    CHECK(r.effect[1].path == Symbol("things"));
    CHECK(to_string(r.effect[1].values) == "(lamp -2 4 0)");
    CHECK(messages(r.warnings) ==
          std::vector<std::string>{"'items' has no z to drop",
                                   "'items' already has a; #append adds another"});
    CHECK(r.warnings[0].line() == 3);
    CHECK(r.warnings[1].line() == 4);

    // A plain push of a layer sends the same warnings to the handler, and applies. A shipping
    // build does not look for them.
    std::vector<std::string> warned;
    moved.set_warning_handler([&](const Error& e) { warned.emplace_back(e.message()); });
    REQUIRE(moved.push_layer("agent", dir.write("agent.dta", agent)));
    REQUIRE(warned.size() == (kShipping ? 0 : 2));
    if constexpr (!kShipping)
        CHECK(warned[0] ==
              "'cpu.easy.reaction' was 30 when this row was written; the stack has 32");
    CHECK(moved.get<int>("cpu.easy.reaction") == 26);
    CHECK_FALSE(moved.get<int>("menu.debug"));
}

TEST_CASE("the policy reads a verb's operands; existing_only refuses a verb on a new key") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text(
        "default", "(households (cottage (things (phone -2 4.6 180))))\n(items a b)\n"));
    const LayerPolicy scalars{.scalars_only = true};
    auto r = ok(cfg.check_layer_text("dlc", "(households (cottage (things #append (lamp -2 4 0))))",
                                     scalars));
    REQUIRE(r.refusals.size() == 1);
    CHECK(has(r.refusals[0].message(), "is (lamp -2 4 0), not a number"));
    CHECK(ok(cfg.check_layer_text("dlc", "(items #append c d)", scalars)).ok());
    const LayerPolicy existing{.existing_only = true};
    CHECK(ok(cfg.check_layer_text("dlc", "(items #append c)\n(items2 #drop a)", existing))
              .refusals.size() == 1);
    CHECK(ok(cfg.check_layer_text("dlc", "(items #drop a)", existing)).ok());
    CHECK_FALSE(ok(cfg.check_layer_text("dlc", "(items #remove)", existing)).ok());
}

BAKER_DEV_TEST_CASE("write-back refuses a path a layer composes; a pin is kept and written") {
    TempDir dir;
    const std::string base_text =
        "(fighters (kestrel (moves (jab (cancels jab low_kick) (damage 30)))))\n";
    const std::string agent_text =
        "(fighters (kestrel (moves (jab (cancels #append palm) (damage 32)))))\n";
    const char* cancels = "fighters.kestrel.moves.jab.cancels";
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", base_text)));
    REQUIRE(cfg.push_layer("agent", dir.write("agent.dta", agent_text)));
    // Writing the composed list at or below the verb would append palm twice.
    const std::vector<Node> live{Node(Symbol("jab")), Node(Symbol("palm"))};
    REQUIRE(cfg.set(cancels, std::span<const Node>(live)));
    for (const auto& r : {cfg.write_back(cancels), cfg.write_back_to(cancels, "default"),
                          cfg.write_back_to(cancels, "agent")}) {
        REQUIRE_FALSE(r);
        CHECK(r.error().code() == ErrorCode::Refused);
        INFO(r.error().message());
        CHECK(has(r.error().message(),
                  "'fighters/kestrel/moves/jab/cancels' is composed by layer 'agent': "
                  "(cancels #append palm); write the layer below it, or promote the layer"));
        CHECK(r.error().line() == 1);
    }
    CHECK(dir.read("default.dta") == base_text);
    CHECK(dir.read("agent.dta") == agent_text);
    // A layer above the verb restates the list, which is what it reads.
    REQUIRE(cfg.push_layer("top", dir.path("top.dta"), {.optional = true}));
    REQUIRE(cfg.write_back_to(cancels, "top"));
    CHECK(dir.read("top.dta") == "(/fighters.kestrel.moves.jab.cancels jab palm)\n");
    REQUIRE(cfg.remove_layer("top"));

    // write_back keeps a pin; write_back_to with a pin writes one, after the values or over
    // the pin a row has.
    Config pinned({.tweaks = nullptr});
    REQUIRE(pinned.push_layer(
        "default", dir.write("cpu.dta", "(cpu (easy (reaction 30) (block 0.2) (jump 3)))\n")));
    REQUIRE(pinned.push_layer(
        "agent", dir.write("agent2.dta", "(cpu (easy (reaction 26 (@was 30)) (jump 4)))\n")));
    REQUIRE(pinned.set("cpu.easy.reaction", Node(int64_t{24})));
    REQUIRE(pinned.write_back("cpu.easy.reaction"));
    CHECK(dir.read("agent2.dta") == "(cpu (easy (reaction 24 (@was 30)) (jump 4)))\n");
    REQUIRE(pinned.set("cpu.easy.block", Node(0.35)));
    REQUIRE(pinned.write_back_to("cpu.easy.block", "agent", true));
    CHECK(dir.read("agent2.dta") ==
          "(cpu (easy (reaction 24 (@was 30)) (jump 4) (block 0.35 (@was 0.2))))\n");
    REQUIRE(pinned.set("cpu.easy.jump", Node(int64_t{5})));
    REQUIRE(pinned.write_back_to("cpu.easy.jump", "agent", true));
    CHECK(dir.read("agent2.dta") ==
          "(cpu (easy (reaction 24 (@was 30)) (jump 5 (@was 3)) (block 0.35 (@was 0.2))))\n");
    CHECK(pinned.get<double>("cpu.easy.block") == 0.35);
    REQUIRE(pinned.set("cpu.easy.reaction", Node(int64_t{22})));
    REQUIRE(pinned.write_back_to("cpu.easy.reaction", "agent", true));
    CHECK(has(dir.read("agent2.dta"), "(reaction 22 (@was 30))"));
    CHECK(ok(pinned.check_layer_text("again", dir.read("agent2.dta"))).warnings.size() == 3);
}

BAKER_DEV_TEST_CASE("promote deletes a removed key and writes a composed list resolved") {
    TempDir dir;
    const std::string base_text =
        "(menu\n"
        "   (debug_menu 1)   ; for the team\n"
        "   (items play options credits))\n"
        "(enemies (grunt (hp 10)) (exploit_boss (hp 1)))\n";
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("base", dir.write("base.dta", base_text)));
    REQUIRE(cfg.push_layer(
        "patch", dir.write("patch.dta",
                           "(menu (debug_menu #remove) (items #drop options (@note \"cut\")))\n"
                           "(enemies (exploit_boss #remove))\n(missing #drop x)\n")));
    CHECK(to_string(*cfg.root()) ==
          "((menu (items play credits (@note \"cut\"))) (enemies (grunt (hp 10))))");
    const WriteReport r = ok(cfg.promote("patch", "base"));
    CHECK(r.ok());
    CHECK(dir.read("base.dta") ==
          "(menu\n"
          "   (items play credits))\n"
          "(enemies (grunt (hp 10)))\n");
    REQUIRE(cfg.remove_layer("patch"));  // the values read the same; the note was the patch's
    CHECK(to_string(*cfg.root()) == "((menu (items play credits)) (enemies (grunt (hp 10))))");

    // A row a layer above the target composes again is refused, and nothing is written.
    REQUIRE(cfg.push_layer("dlc", dir.write("dlc.dta", "(menu (items #append bonus))\n")));
    REQUIRE(cfg.set("menu.items", Node(Symbol("play"))));
    const WriteReport refused = ok(cfg.promote("scratch", "base"));
    REQUIRE(refused.refused.size() == 1);
    CHECK(has(refused.refused[0].message(), "is composed by layer 'dlc'"));
    CHECK(dir.read("base.dta") == "(menu\n   (items play credits))\n(enemies (grunt (hp 10)))\n");

    // A verb that moves keyed tuples is refused, located.
    Config things({.tweaks = nullptr});
    REQUIRE(things.push_layer("default", dir.write("t.dta", "(things (phone -2 4.6 180))\n")));
    REQUIRE(things.push_layer("pack", dir.write("tp.dta", "(things #append (lamp -2 4 0))\n")));
    const WriteReport w = ok(things.promote("pack", "default"));
    REQUIRE(w.refused.size() == 1);
    CHECK(has(w.refused[0].message(), "its verb moves keyed tuples"));
    CHECK(w.refused[0].line() == 1);
}

TEST_CASE("verbs over a long list: each dropped value once per operand, appends in order") {
    // Two operands of one value drop its first two, wherever they are; a third operand
    // finds none left.
    CHECK(merged("(l a b a c a)", "(l #drop a a)") == "((l b c a))");
    CHECK(merged("(l a b)", "(l #drop a a a)") == "((l b))");
    CHECK(merged("(l 1 1.0 -0.0 0.0)", "(l #drop 1.0 0.0)") == "((l 1 -0.0))");
    // Thousands of operands: the result, not the time, which the D-entry gives.
    std::string base = "(l", drop = "(l #drop", append = "(l #append";
    for (int i = 0; i < 4000; ++i) {
        base += " " + std::to_string(i);
        if (i % 2 == 0) drop += " " + std::to_string(i);
        append += " " + std::to_string(4000 + i);
    }
    base += ")";
    drop += ")";
    append += ")";
    const auto dropped = merge(*parse(base), *parse(drop));
    REQUIRE(dropped->try_find("l"));
    CHECK(dropped->try_find("l")->values().size() == 2000);
    CHECK(*dropped->try_find("l")->values()[0].try_int() == 1);
    const auto appended = merge(*parse(base), *parse(append));
    CHECK(appended->try_find("l")->values().size() == 8000);
    CHECK(*appended->try_find("l")->values()[4000].try_int() == 4000);
}
