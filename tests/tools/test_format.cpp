// Laying out text as Baker writes it, keeping its comments (requirement 0026).
#include <doctest.h>
#include <baker/parse.hpp>
#include <baker/project.hpp>
#include <baker/std_interop.hpp>
#include <baker/tools/format.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "ok.hpp"
#include "temp_dir.hpp"

using namespace baker;
using namespace baker::tools;
using baker::test::ok;

namespace {

std::string formatted(std::string_view text, int indent = 2, int width = 80, uint32_t begin = 0,
                      uint32_t end = UINT32_MAX) {
    std::string out;
    (void)format_text(text, string_out(out),
                      {.layout = {.indent = indent, .width = width}, .begin = begin, .end = end});
    return out;
}

// The old text with the edits applied.
std::string applied(std::string_view text, const Formatted& f) {
    std::string out;
    uint32_t from = 0;
    for (const FormatEdit& e : f.edits) {
        out.append(text.substr(from, e.begin - from));
        for (uint32_t k = 0; k < e.newlines; ++k) out.append(f.line_end);
        out.append(e.spaces, ' ');
        from = e.end;
    }
    out.append(text.substr(from));
    return out;
}

// What a text says, whitespace aside: each token and comment, in order.
std::vector<std::string> said(std::string_view text) {
    const TextStructure s = read_structure(text);
    std::vector<std::string> out;
    for (const TextToken& t : s.tokens) out.emplace_back(text.substr(t.begin, t.end - t.begin));
    for (const TextComment& c : s.comments) {  // a CRLF's \r aside
        std::string comment(text.substr(c.begin, c.end - c.begin));
        if (!comment.empty() && comment.back() == '\r') comment.pop_back();
        out.emplace_back("#c " + comment);
    }
    return out;
}

}  // namespace

TEST_CASE("the writer's layout: a form on one line if it fits, else one item a line") {
    const std::string text = "(shop (sword (price 12) (damage 3)) (shield (price 9)))\n";
    CHECK(formatted(text) == text);
    // At 30 the shop breaks, and the sword (29 wide, at indent 2) does not fit in 28.
    CHECK(formatted(text, 2, 30) ==
          "(shop\n"
          "  (sword\n"
          "    (price 12)\n"
          "    (damage 3))\n"
          "  (shield (price 9)))\n");
    CHECK(formatted(text, 2, 40) ==
          "(shop\n"
          "  (sword (price 12) (damage 3))\n"
          "  (shield (price 9)))\n");
    // The same as write_dta writes the tree, for text with nothing a tree loses.
    const std::string messy = "(shop\n(sword   (price 12)\n (damage 3))   (shield\n(price 9)))";
    std::string written;
    write_dta(*ok(read_string(messy, "t.dta")), string_out(written), {.indent = 2, .width = 30});
    CHECK(formatted(messy, 2, 30) == written);
}

TEST_CASE("comments stay: trailing, on their own line, and block; a ; comment breaks a form") {
    CHECK(formatted("(a 1) ; after\n; alone\n(b 2)\n") == "(a 1) ; after\n; alone\n(b 2)\n");
    CHECK(formatted("(shop ; the shop\n (sword 1) /* inline */ (bow 2))\n") ==
          "(shop ; the shop\n  (sword 1) /* inline */\n  (bow 2))\n");
    CHECK(formatted("(a 1 ; one\n 2)\n") == "(a\n  1 ; one\n  2)\n");
    CHECK(formatted("(a\n  1\n  ; last\n)\n") == "(a\n  1\n  ; last\n)\n");
    CHECK(formatted("(a /* x */ 1)\n") == "(a /* x */ 1)\n");
}

TEST_CASE("directives keep their arguments on their line; #remove is a value") {
    CHECK(
        formatted("#include   \"common.dta\"\n#define   FAST\n(speed\n#ifdef FAST\n 9\n#else\n 3\n"
                  "#endif\n)\n") ==
        "#include \"common.dta\"\n#define FAST\n(speed\n  #ifdef FAST\n  9\n  #else\n  3\n"
        "  #endif)\n");
    CHECK(formatted("#define RED (1\n 0   0)\n(a (b #remove))\n") ==
          "#define RED (1 0 0)\n(a (b #remove))\n");
    CHECK(formatted("#autorun   {print\n hi}\n") == "#autorun {print hi}\n");
}

TEST_CASE("what was written aligned stays aligned; nothing else is aligned") {
    const std::string table =
        "(kinds\n"
        "   (ball   (shape sphere) (size 0.4))\n"
        "   (crate  (shape box) (size 0.6))\n"
        "   (seesaw (shape box) (size 3)))\n";
    CHECK(formatted(table, 3, 80) == table);
    // A longer key added: the run is padded to it.
    CHECK(formatted("(kinds\n"
                    "   (ball   (shape sphere) (size 0.4))\n"
                    "   (catapult (shape box) (size 9))\n"
                    "   (crate  (shape box) (size 0.6)))\n",
                    3, 80) ==
          "(kinds\n"
          "   (ball     (shape sphere) (size 0.4))\n"
          "   (catapult (shape box) (size 9))\n"
          "   (crate    (shape box) (size 0.6)))\n");
    // Not aligned as written (the second items at different columns): single spaces.
    CHECK(formatted("(kinds\n   (ball  (shape sphere) (size 0.4))\n   (crate   (shape box)))\n", 3,
                    40) ==
          "(kinds\n   (ball (shape sphere) (size 0.4))\n   (crate (shape box)))\n");
    // Trailing comments written in a column stay in one, past the longest line.
    CHECK(formatted("(a 1)        ; one\n(bbbbb 2)    ; two\n(c 3)  ; three\n") ==
          "(a 1)     ; one\n(bbbbb 2) ; two\n(c 3) ; three\n");
}

TEST_CASE("one blank line where any was written; none at the ends") {
    CHECK(formatted("\n\n(a 1)\n\n\n\n(b 2)\n(c 3)\n\n\n") == "(a 1)\n\n(b 2)\n(c 3)\n");
    CHECK(formatted("(top\n  (a 1)\n\n\n  (b 2))\n", 2, 10) == "(top\n  (a 1)\n\n  (b 2))\n");
}

TEST_CASE("text that does not parse is unchanged; a range lays out only what it touches") {
    std::string out;
    const Formatted f = format_text("(a  (b 1)\n", string_out(out));
    CHECK(out == "(a  (b 1)\n");
    CHECK_FALSE(f.errors.empty());
    CHECK_FALSE(f.changed());

    const std::string text = "(a   1)\n(b   2)\n(c   3)\n";
    CHECK(formatted(text, 2, 80, 9, 10) == "(a   1)\n(b 2)\n(c   3)\n");
}

TEST_CASE("the edits turn the old text into the new; CRLF and a byte order mark stay") {
    for (const std::string text :
         {"(shop\n(sword   (price 12)\n (damage 3))   (shield (price 9)))",
          "\xEF\xBB\xBF(a   1)\r\n\r\n\r\n(b\r\n 2)\r\n", "(a 1)\n", "", "   \n"}) {
        CAPTURE(text);
        std::string out;
        const Formatted f = format_text(text, string_out(out), {.layout = {.width = 20}});
        CHECK(applied(text, f) == out);
    }
    std::string out;
    const Formatted crlf = format_text("(a   1)\r\n(b 2)", string_out(out));
    CHECK(out == "(a 1)\r\n(b 2)\r\n");
    CHECK(crlf.line_end == "\r\n");
}

TEST_CASE("over the corpus: formatting twice changes nothing, and nothing but whitespace") {
    namespace fs = std::filesystem;
    size_t files = 0;
    // The game stacks are in the development tree only: a released tree has the rest.
    for (const char* root : {"tests", "examples", "stacks"}) {
        const fs::path dir = fs::path(BAKER_SOURCE_DIR) / root;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            if (e.path().extension() != ".dta" ||
                e.path().string().find("/build") != std::string::npos)
                continue;
            std::ifstream in(e.path(), std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string text = ss.str();
            if (!read_structure(text).errors.empty()) continue;  // fixtures that do not read
            CAPTURE(e.path().string());
            for (const auto& [indent, width] :
                 {std::pair{2, 80}, std::pair{3, 100}, std::pair{1, 20}}) {
                const std::string once = formatted(text, indent, width);
                CHECK(formatted(once, indent, width) == once);
                CHECK(said(once) == said(text));
            }
            ++files;
        }
    }
    CHECK(files > 20);
}

TEST_CASE("formatting is linear in the text") {
    // Linear: four times the text takes about four times as long, where a layout that
    // looked back over the text would take sixteen. A ratio, so that a sanitizer's or a
    // debug build's slowdown, which falls on both, does not matter; the 100 ms for 2 MB
    // is a Release measurement.
    auto text_of_size = [](size_t bytes) {
        std::string text;
        for (int i = 0; text.size() < bytes; ++i)
            text += "(item" + std::to_string(i) + "   (price " + std::to_string(i % 97) +
                    ") ; its price\n (name \"Item\")  (tags fast   loud))\n";
        return text;
    };
    auto best_ms = [](const std::string& text) {
        double best = 1e30;
        for (int run = 0; run < 3; ++run) {
            const auto t0 = std::chrono::steady_clock::now();
            std::string out;
            (void)format_text(text, string_out(out));
            best = std::min(best, std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count());
        }
        return best;
    };
    const double one = best_ms(text_of_size(500'000));
    const double four = best_ms(text_of_size(2'000'000));
    MESSAGE("0.5 MB in " << one << " ms, 2 MB in " << four << " ms");
    CHECK(four < 8 * one);
}

TEST_CASE("a project names its layout with (format ...)") {
    test::TempDir dir;
    const std::string file = dir.write(
        "baker-project.dta", "(project (format (indent 3) (width 100)) (layers (d \"d.dta\")))\n");
    const Project p = ok(read_project(file));
    CHECK(p.format.indent == 3);
    CHECK(p.format.width == 100);
    CHECK(ok(read_project(dir.write("b/baker-project.dta", "(project (layers (d \"d.dta\")))\n")))
              .format.indent == WriteOptions{}.indent);
    CHECK_FALSE(read_project(
        dir.write("c/baker-project.dta", "(project (format (depth 3)) (layers (d \"d.dta\")))\n")));
}
