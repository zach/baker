// read_structure: what a text holds for tools, by the parser's own lexer, even when it does
// not parse.
#include <doctest.h>
#include <baker/parse.hpp>
#include <baker/tools/structure.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace baker;
using namespace baker::tools;

namespace {
std::string_view span(std::string_view text, uint32_t begin, uint32_t end) {
    return text.substr(begin, end - begin);
}
std::vector<TextTokenKind> kinds(const TextStructure& s) {
    std::vector<TextTokenKind> out;
    for (const TextToken& t : s.tokens) out.push_back(t.kind);
    return out;
}
}  // namespace

TEST_CASE("read_structure: tokens, brackets and comments of text that parses") {
    const std::string_view text =
        "; a tweak\n(kestrel (health 100) /* hp */ (speed 2.5) {spawn 'grunt one' \"x\" $n})\n"
        "#define PC\n";
    const TextStructure s = read_structure(text, Symbol("k.dta"));
    CHECK(s.errors.empty());
    using K = TextTokenKind;
    CHECK(kinds(s) == std::vector<K>{K::Open, K::Symbol, K::Open, K::Symbol, K::Int, K::Close,
                                     K::Open, K::Symbol, K::Float, K::Close, K::Open, K::Symbol,
                                     K::QuotedSymbol, K::String, K::Variable, K::Close, K::Close,
                                     K::Directive, K::Symbol});
    CHECK(span(text, s.tokens[1].begin, s.tokens[1].end) == "kestrel");
    CHECK(span(text, s.tokens[12].begin, s.tokens[12].end) == "'grunt one'");
    CHECK(s.tokens[1].line == 2);

    REQUIRE(s.brackets.size() == 4);
    CHECK(s.brackets[0].parent == -1);
    CHECK(s.brackets[0].tag == 1);  // kestrel
    CHECK(s.brackets[1].parent == 0);
    CHECK(s.brackets[3].open == '{');
    CHECK(s.brackets[3].closed);
    CHECK(span(text, s.brackets[1].begin, s.brackets[1].end) == "(health 100)");

    REQUIRE(s.comments.size() == 2);
    CHECK(span(text, s.comments[0].begin, s.comments[0].end) == "; a tweak");
    CHECK(s.comments[1].block);
    CHECK(span(text, s.comments[1].begin, s.comments[1].end) == "/* hp */");
}

TEST_CASE("read_structure: text being typed reads to its end, with the problems beside it") {
    const std::string_view typing = "(kestrel (hea";
    const TextStructure s = read_structure(typing);
    REQUIRE(s.brackets.size() == 2);
    CHECK_FALSE(s.brackets[0].closed);
    CHECK(s.brackets[0].end == typing.size());  // runs to the end
    CHECK(s.brackets[1].tag == 3);              // hea
    CHECK(s.errors.size() == 2);                // two unclosed
    CHECK(std::string(s.errors[0].message()).find("unclosed") != std::string::npos);

    const std::string_view string = "(name \"unfinished\n(more 1)";
    const TextStructure u = read_structure(string);
    REQUIRE(u.tokens.size() == 3);
    CHECK(u.tokens[2].kind == TextTokenKind::String);
    CHECK(u.tokens[2].end == string.size());  // the string runs to the end
    CHECK(u.errors.size() == 2);              // unterminated, and unclosed

    const TextStructure c = read_structure("(a 1) /* open");
    REQUIRE(c.comments.size() == 1);
    CHECK(c.comments[0].end == 13);
    CHECK(c.errors.size() == 1);

    const TextStructure stray = read_structure("(a 1)) (b [2)");
    CHECK(stray.errors.size() == 3);  // a stray ')', a ')' closing '[', and '(' unclosed
    CHECK(stray.brackets.size() == 3);

    const TextStructure bad = read_structure("(n 99999999999999999999 $)");
    CHECK(bad.tokens.size() == 5);  // the number and the lone '$' are tokens still
    CHECK(bad.errors.size() == 2);
}

TEST_CASE("read_structure: a macro this text defines is a macro where it is used") {
    const TextStructure s =
        read_structure("[GREETING hello there]\n(say GREETING)\n(say 'GREETING')");
    CHECK(s.errors.empty());
    CHECK(s.tokens[1].kind == TextTokenKind::Symbol);         // the definition's name
    CHECK(s.tokens[7].kind == TextTokenKind::Macro);          // a use
    CHECK(s.tokens[11].kind == TextTokenKind::QuotedSymbol);  // quoted: never a macro
}

TEST_CASE("read_structure agrees with the parser on the corpus: what parses reads clean") {
    for (const auto& entry : std::filesystem::directory_iterator(BAKER_CORPUS_DIR)) {
        if (entry.path().extension() != ".dta") continue;
        CAPTURE(entry.path().string());
        std::ifstream in(entry.path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        const TextStructure s = read_structure(text);
        if (read_string(text, entry.path().string())) {
            CHECK(s.errors.empty());
            for (const TextBracket& b : s.brackets) CHECK(b.closed);
        }
    }
}

TEST_CASE("a declaration's form is marked with its directive") {
    const std::string text =
        "#pack (core (level game))\n#shape (unit (hp int))\n(unit (hp 1))\n"
        "(a #append (b 1))\n";
    const TextStructure s = read_structure(text);
    REQUIRE(s.brackets.size() >= 5);
    auto directive = [&](const TextBracket& b) {
        if (b.declares < 0) return std::string();
        const TextToken& t = s.tokens[static_cast<size_t>(b.declares)];
        return text.substr(t.begin, t.end - t.begin);
    };
    CHECK(directive(s.brackets[0]) == "#pack");
    CHECK(s.brackets[1].declares < 0);  // (level game), inside it
    CHECK(directive(s.brackets[2]) == "#shape");
    CHECK(s.brackets[4].declares < 0);  // (unit (hp 1)) is data
    for (size_t i = 5; i < s.brackets.size(); ++i) CHECK(s.brackets[i].declares < 0);
}

TEST_CASE("a brace that reads infix names its operator, and malformed infix is an error") {
    const std::string text =
        "(pass {0.3 <= $value <= 0.7})\n{wheat -> flour}\n{+ '-' 1}\n{f + 1 2}\n"
        "{{econ.count mill} >= 1}\n";
    const TextStructure s = read_structure(text);
    CHECK(s.errors.empty());
    auto op = [&](size_t i) {
        const int32_t t = s.brackets[i].infix;
        return t < 0 ? std::string() : std::string(span(text, s.tokens[t].begin, s.tokens[t].end));
    };
    REQUIRE(s.brackets.size() == 7);
    CHECK(op(0).empty());  // (pass ...)
    CHECK(op(1) == "<=");  // {0.3 <= $value <= 0.7}
    CHECK(op(2).empty());  // bare symbols: the guard
    CHECK(op(3).empty());  // a quoted operator: a prefix call
    CHECK(op(4).empty());  // a call's head before an operator argument
    CHECK(op(5) == ">=");  // a command operand
    CHECK(op(6).empty());  // {econ.count mill}
    // The parser's errors for malformed infix, at the brace, beside the rest.
    for (const auto& [bad, message] : std::vector<std::pair<std::string, std::string>>{
             {"(pass\n   {0 < $value <= 10})",
              "mixed operators in {...}: nest braces to say which applies first"},
             {"{0.3 <= $value 0.7}", "infix {...} needs '<=' between every two operands"},
             {"{$value <=}", "infix {...} ends with '<=': write an operand after it"}}) {
        const TextStructure m = read_structure(bad);
        REQUIRE(m.errors.size() == 1);
        CHECK(m.errors[0].message() == message);
        auto parsed = read_string(bad, "t.dta");
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().message() == message);
        CHECK(m.errors[0].line() == parsed.error().line());
        CHECK(m.errors[0].column() == parsed.error().column());
    }
}
