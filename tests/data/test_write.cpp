// The printer: spelling of every value, layout, and the round-trip guarantee.
#include <doctest.h>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

#include "text.hpp"

#include <bit>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <random>
#include <string>

using namespace baker;

namespace {
Ref<Array> parse(std::string_view text) {
    auto r = read_string(text, "t.dta");
    REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : to_string(r.error())));
    return *r;
}
void check_round_trip(const Array& root) {
    const std::string printed = to_dta(root);
    auto again = read_string(printed, "printed.dta");
    REQUIRE_MESSAGE(again.has_value(), printed);
    CHECK_MESSAGE(structurally_equal(root, **again), printed);
}
}  // namespace

TEST_CASE("atoms print in their reparseable spelling") {
    CHECK(to_string(Node(42)) == "42");
    CHECK(to_string(Node(-7)) == "-7");
    CHECK(to_string(Node(12.75f)) == "12.75");
    CHECK(to_string(Node(5.0f)) == "5.0");
    CHECK(to_string(Node(-0.0f)) == "-0.0");
    CHECK(to_string(Node(1e20)) == "1e+20");
    CHECK(to_string(Node(0.001)) == "0.001");
    CHECK(to_string(Node(0.1)) == "0.1");
    CHECK(to_string(Node(0.1f)) == "0.10000000149011612");  // a float widened exactly
    // A float a script function returns reads as its shortest decimal; zero keeps its sign.
    CHECK(baker::detail::float_to_decimal_double(0.1f) == 0.1);
    CHECK(Node(baker::detail::float_to_decimal_double(0.0f)).float_bits() == 0);
    CHECK(Node(baker::detail::float_to_decimal_double(-0.0f)).float_bits() == 0x8000000000000000u);
    CHECK(to_string(Node(int64_t{INT64_MIN})) == "-9223372036854775808");
    CHECK(to_string(Node(Symbol("price"))) == "price");
    CHECK(to_string(Node(Symbol("bacon and eggs"))) == "'bacon and eggs'");
    CHECK(to_string(Node(Symbol(""))) == "''");
    CHECK(to_string(Node(Symbol("12"))) == "'12'");
    CHECK(to_string(Node(Symbol("1e5"))) == "'1e5'");
    CHECK(to_string(Node(Symbol("99999999999"))) == "'99999999999'");
    CHECK(to_string(Node(Symbol("$x"))) == "'$x'");
    CHECK(to_string(Node(Symbol("#if"))) == "'#if'");
    CHECK(to_string(Node(Symbol("/*x"))) == "'/*x'");
    CHECK(to_string(Node(Symbol("a/*x"))) == "a/*x");
    CHECK(to_string(Node(Symbol("semi;colon"))) == "'semi;colon'");
    CHECK(to_string(Node(Symbol("don't"))) == "don't");
    CHECK(to_string(Node(Symbol("-"))) == "-");
    CHECK(to_string(Node(Symbol("\xEF\xBB\xBFze"))) == "\xEF\xBB\xBFze");
    CHECK(to_dta(*array(array("\xEF\xBB\xBFze''", 1))) == "(\xEF\xBB\xBFze'' 1)\n");
    CHECK(to_dta(*Array::make_copy(std::vector<Node>{Node(Symbol("\xEF\xBB\xBFze''"))})) ==
          " \xEF\xBB\xBFze''\n");
    CHECK(to_string(Node(str("say \"hi\"\nbye"))) == R"("say \qhi\q\nbye")");
    CHECK(to_string(Node(str(R"(C:\games)"))) == R"("C:\games")");
    CHECK(to_string(Node::var("game_time")) == "$game_time");
    static const FuncRecord rec{"+"};
    CHECK(to_string(Node::func(&rec)) == "+");
    CHECK(to_string(Node::object(ObjectHandle::make(2, 7))) == "'<object 2:7>'");
}

TEST_CASE("arrays print compactly and break when too wide") {
    auto root = parse("(menu ('bacon and eggs' (price 12.75) (calories 120)) (x {cmd $v} [prop]))");
    CHECK(to_string((*root)[0]) ==
          "(menu ('bacon and eggs' (price 12.75) (calories 120)) (x {cmd $v} [prop]))");
    CHECK(to_dta(*root, {.indent = 2, .width = 40}) ==
          "(menu\n"
          "  ('bacon and eggs'\n"
          "    (price 12.75)\n"
          "    (calories 120))\n"
          "  (x {cmd $v} [prop]))\n");
    CHECK(to_dta(*parse("(a 1) b 3")) == "(a 1)\nb\n3\n");
    CHECK(to_dta(*parse("")).empty());
}

TEST_CASE("printing round-trips hand-picked trees") {
    for (const char* src : {
             "(a 1 -2 3.5 -0.0 1e-30 3.4e38 sym 'q s' \"str\" $v {c 1} (n [p]))",
             "('' '\"' \"'\" \"\\q\" \"a\\\\nb\" \"\\\\\" 'x\ny' \"x\ny\")",
             "(0x7FFFFFFF 0xFFFFFFFF -2147483648 '-' '+' '.' '1.' '0x1' '-1' 'inf')",
             "({{nearest_object $p} suffer_damage} [a [b [c]]])",
         }) {
        CAPTURE(src);
        check_round_trip(*parse(src));
    }
}

TEST_CASE("printing round-trips random trees") {
    std::mt19937 rng(12345);
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    const char alphabet[] = "abz09 _-+.'\"$#;()[]{}\\/*\nq";
    auto random_text = [&] {
        std::string s;
        for (int i = pick(6); i > 0; --i)
            s += alphabet[pick(static_cast<int>(sizeof alphabet - 1))];
        return s;
    };
    std::function<Node(int)> gen = [&](int depth) -> Node {
        switch (pick(depth > 4 ? 6 : 9)) {
            case 0: return Node(static_cast<int64_t>((uint64_t{rng()} << 32) | rng()));
            case 1: {
                const double f = std::bit_cast<double>((uint64_t{rng()} << 32) | rng());
                return std::isfinite(f) ? Node(f) : Node(1.5);
            }
            case 2:
            case 3: {
                // A symbol that needs quotes cannot also contain a quote.
                std::string s = random_text();
                for (char& c : s)
                    if (c == '\'') c = '_';
                return Node(Symbol(s));
            }
            case 4: {
                // Strings cannot hold a literal backslash followed by n or q.
                std::string s = random_text();
                for (size_t i = 0; i + 1 < s.size(); ++i)
                    if (s[i] == '\\' && (s[i + 1] == 'n' || s[i + 1] == 'q')) s[i] = '/';
                return Node(str(s));
            }
            case 5: {
                std::string name = "v" + random_text();
                for (char& c : name)
                    if (static_cast<unsigned char>(c) <= 0x20 ||
                        std::string_view("()[]{}\";").find(c) != std::string_view::npos)
                        c = '_';
                return Node::var(Symbol(name));
            }
            default: {
                std::vector<Node> kids;
                for (int i = pick(5); i > 0; --i) kids.push_back(gen(depth + 1));
                const Type kinds[] = {Type::Array, Type::Command, Type::Property};
                return Node(Array::make_from(kids), kinds[pick(3)]);
            }
        }
    };
    for (int round = 0; round < 300; ++round) {
        std::vector<Node> top;
        for (int i = pick(6); i > 0; --i) {
            Node n = gen(0);
            if (n.type() == Type::Property) n = Node(Ref<Array>(n.mutable_array()));
            top.push_back(n);
        }
        check_round_trip(*Array::make_from(top));
    }
}

TEST_CASE("write_file writes the file form") {
    const auto path = (std::filesystem::temp_directory_path() / "baker_write_test.dta").string();
    auto root = parse("(a 1) (b \"two\")");
    REQUIRE(write_file(path, *root).has_value());
    auto back = read_file(path);
    REQUIRE(back.has_value());
    CHECK(structurally_equal(*root, **back));
    std::remove(path.c_str());
    CHECK(write_file("/definitely/not/a/dir/x.dta", *root).error().code() == ErrorCode::Io);
}

TEST_CASE("preview is bounded on shared trees") {
    std::string src = "[M0 (x x)]\n";
    for (int i = 1; i <= 60; ++i)
        src += "[M" + std::to_string(i) + " (M" + std::to_string(i - 1) + " M" +
               std::to_string(i - 1) + ")]\n";
    src += "(top M60)";
    auto root = parse(src);
    const std::string p(detail::preview((*root)[0], 40).view());
    CHECK(p.size() == 40);
    CHECK(p.ends_with("..."));
}

TEST_CASE("both double formatters round-trip exactly") {
    // format_double uses std::to_chars except on Apple, where format_double_fallback
    // (shortest %g that reads back) stands in. Both must give back the same bits.
    auto reads_back = [](std::string text, double v) {
        if (text.find_first_of(".eEni") == std::string::npos) text += ".0";
        const detail::Number n = detail::classify_number(text);
        return n.kind == detail::NumberKind::Float &&
               std::bit_cast<uint64_t>(n.f) == std::bit_cast<uint64_t>(v);
    };
    std::vector<double> values = {0.0,
                                  -0.0,
                                  0.1,
                                  1.0 / 3,
                                  12.75,
                                  5.0,
                                  1e20,
                                  1e-5,
                                  4.9e-324,
                                  1.7976931348623157e308,
                                  -2.2250738585072014e-308,
                                  123456789.125};
    std::mt19937_64 rng(7);
    while (values.size() < 20000) {
        const double v = std::bit_cast<double>(rng());
        if (std::isfinite(v)) values.push_back(v);
    }
    for (double v : values) {
        CAPTURE(v);
        CHECK(reads_back(std::string(detail::format_double(v).view()), v));
        CHECK(reads_back(std::string(detail::format_double_fallback(v).view()), v));
    }
    CHECK(detail::format_double_fallback(0.1) == "0.1");
    CHECK(detail::format_double_fallback(-0.0) == "-0");
    CHECK(detail::format_double_fallback(1e20) == "1e+20");

    // %g follows the C locale's decimal point; the fallback must normalize it.
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "German_Germany.1252"}) {
        if (!std::setlocale(LC_NUMERIC, name)) continue;
        CHECK(detail::format_double_fallback(12.75) == "12.75");
        std::setlocale(LC_NUMERIC, "C");
        break;
    }
}
