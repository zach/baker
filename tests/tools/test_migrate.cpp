// The language's migrations as a call: findings, edits and text.
#include <doctest.h>
#include <baker/std_interop.hpp>
#include <baker/tools/migrate.hpp>

#include <string>
#include <vector>

using namespace baker;
using namespace baker::tools;

namespace {

struct Run {
    std::string text;
    Migrated m;
};

Run migrated(std::string_view text, const MigrateOptions& options = {}) {
    std::string out;
    Migrated m = migrate_text(text, string_out(out), options);
    return {std::move(out), std::move(m)};
}

std::string one(std::string_view text, const MigrationFinding& f) {
    std::string out;
    apply_findings(text, {&f, 1}, string_out(out));
    return out;
}

constexpr uint32_t only(Migration m) {
    return 1U << static_cast<uint32_t>(m);
}

}  // namespace

TEST_CASE("metadata after a value gets its sigil, one finding a tag") {
    const std::string_view text = "(player (speed 4.5 (range 0 10) (desc \"Run speed\")))\n";
    const Run r = migrated(text);
    CHECK(r.text == "(player (speed 4.5 (@range 0 10) (@desc \"Run speed\")))\n");
    REQUIRE(r.m.findings.size() == 2);
    const MigrationFinding& f = r.m.findings[0];
    CHECK(f.migration == Migration::Metadata);
    CHECK(text.substr(f.begin, f.end - f.begin) == "range");
    CHECK(f.warning.message() == "(range ...) after a value is metadata; write (@range ...)");
    CHECK(f.warning.line() == 1);
    REQUIRE(f.edits.size() == 1);
    CHECK(f.edits[0].begin == f.begin);
    CHECK(f.edits[0].end == f.begin);
    CHECK(f.edits[0].text == "@");
    // One finding's edit alone.
    CHECK(one(text, r.m.findings[1]) ==
          "(player (speed 4.5 (range 0 10) (@desc \"Run speed\")))\n");
}

TEST_CASE("the declarations become directives, one finding a form") {
    const std::string_view text =
        "(pack core (level 1))\n"
        "(origin \"base game\")\n"
        "#define RED (1 0 0)\n"
        "(shapes\n"
        "  (enemy (hp int))\n"
        "  (item (cost int)))\n";
    const Run r = migrated(text);
    CHECK(r.text ==
          "#pack (core (level 1))\n"
          "#origin (\"base game\")\n"
          "[RED 1 0 0]\n"
          "#shape (enemy (hp int))\n"
          "#shape (item (cost int))\n");
    REQUIRE(r.m.findings.size() == 4);
    for (const MigrationFinding& f : r.m.findings) CHECK(f.migration == Migration::Declarations);
    CHECK(r.m.findings[0].warning.message() ==
          "(pack ...) is data; a file declares its pack with #pack (NAME ...)");
    CHECK(r.m.findings[2].warning.message() ==
          "#define RED (...) is the older macro form; write [RED ...]");
    CHECK(r.m.findings[3].warning.message() ==
          "(shapes ...) is data; a file declares each shape with #shape (NAME ...)");
    CHECK(r.m.findings[3].warning.line() == 4);
    // Each finding's edits alone leave the rest as written.
    CHECK(one(text, r.m.findings[2]) ==
          "(pack core (level 1))\n(origin \"base game\")\n[RED 1 0 0]\n"
          "(shapes\n  (enemy (hp int))\n  (item (cost int)))\n");
    CHECK(one(text, r.m.findings[3]).find("#shape (enemy (hp int))\n#shape (item") !=
          std::string::npos);
}

TEST_CASE("both migrations in one pass: a shape's metadata and its split do not overlap") {
    const std::string_view text =
        "(shapes\n"
        "  (enemy (at /enemies/*)\n"
        "    (desc \"One enemy.\")\n"
        "    (hp int (range 1 100))\n"
        "    (taunt string (optional))))\n";
    const std::string want =
        "#shape (enemy (@at /enemies/*)\n"
        "  (@desc \"One enemy.\")\n"
        "  (hp int (@range 1 100))\n"
        "  (taunt string (@optional)))\n";
    const Run all = migrated(text);
    CHECK(all.text == want);
    // Applying every finding is the text written; the findings are in text order.
    std::string applied;
    apply_findings(text, {all.m.findings.begin(), all.m.findings.size()}, string_out(applied));
    CHECK(applied == want);
    for (size_t i = 1; i < all.m.findings.size(); ++i)
        CHECK(all.m.findings[i - 1].begin <= all.m.findings[i].begin);
    // Either order of the migrations alone gives the same text.
    const Run meta = migrated(text, {.migrations = only(Migration::Metadata)});
    CHECK(migrated(meta.text, {.migrations = only(Migration::Declarations)}).text == want);
    const Run decl = migrated(text, {.migrations = only(Migration::Declarations)});
    CHECK(migrated(decl.text, {.migrations = only(Migration::Metadata)}).text == want);
}

TEST_CASE("a second call finds nothing to rewrite") {
    const std::string_view text =
        "(pack p)\n(shapes (a (x int (range 0 1))))\n(a (x 0 (step 1)))\n"
        "{func f ($v) (doc \"Doubles.\") {* $v 2}}\n";
    const Run first = migrated(text);
    CHECK(first.text != text);
    const Run second = migrated(first.text);
    CHECK(second.text == first.text);
    CHECK(second.m.findings.empty());
}

TEST_CASE("what a migration will not decide is a finding without edits") {
    const Run r = migrated("(limits (min 0) (step 0.5))\n");
    CHECK(r.text == "(limits (min 0) (step 0.5))\n");
    REQUIRE(r.m.findings.size() == 1);
    CHECK(r.m.findings[0].edits.empty());
    CHECK(r.m.findings[0].warning.message() ==
          "(limits ...) has no value, so its (step ...) is a value under either rule; write "
          "(@step ...) if it is metadata");
    // In prose, metadata named alone.
    const Run prose = migrated("(range 0 10)", {.prose = true});
    REQUIRE(prose.m.findings.size() == 1);
    CHECK(prose.m.findings[0].edits.empty());
    CHECK(migrated("(range 0 10)").m.findings.empty());
}

TEST_CASE("text that does not parse still gets the findings that can be located") {
    const Run r = migrated("(a (b 1 (range 0 2))\n(c \"unterminated", {.file = Symbol("x.dta")});
    CHECK_FALSE(r.m.errors.empty());
    REQUIRE(r.m.findings.size() == 1);
    CHECK(r.m.findings[0].warning.file() == Symbol("x.dta"));
    CHECK(r.text.find("(@range 0 2)") != std::string::npos);
}

TEST_CASE("the shapes split keeps the text's line end, and text with fixed lines stays on one") {
    CHECK(migrated("(shapes\r\n  (a (x int))\r\n  (b (y int)))\r\n").text ==
          "#shape (a (x int))\r\n#shape (b (y int))\r\n");
    CHECK(migrated("(shapes (a (x int)) (b (y int)))", {.fixed_lines = true}).text ==
          "#shape (a (x int)) #shape (b (y int))");
}

TEST_CASE("migration names") {
    CHECK(std::string_view(migration_name(Migration::Metadata)) == "metadata");
    CHECK(std::string_view(migration_name(Migration::Declarations)) == "declarations");
}
