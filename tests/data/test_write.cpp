// The printer: spelling of every value, layout, and the round-trip guarantee.
#include <doctest.h>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

#include "temp_dir.hpp"
#include "text.hpp"

#include <bit>
#include <charconv>
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
    // A directory of the process's own: test binaries of other builds may run at once.
    const baker::test::TempDir dir;
    const std::string path = dir.path("write_test.dta");
    auto root = parse("(a 1) (b \"two\")");
    REQUIRE(write_file(path, *root).has_value());
    auto back = read_file(path);
    REQUIRE(back.has_value());
    CHECK(structurally_equal(*root, **back));
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

#if !(defined(__APPLE__) && defined(_LIBCPP_VERSION))  // Apple's lacks double to_chars
TEST_CASE("the fallback double formatter writes what std::to_chars writes, byte for byte") {
    // Where std::to_chars lacks doubles, format_double_fallback writes every number; a
    // write-back on one platform must read as on another (0020).
    auto to_chars = [](double v) {
        char buf[400];
        return std::string(buf, std::to_chars(buf, buf + sizeof buf, v).ptr);
    };
    std::vector<double> values;
    for (int i = -20000; i <= 20000; ++i)
        values.insert(values.end(), {double(i), i / 100.0, i / 8.0, i * 1000.0, i * 1e15});
    for (int e = -1074; e <= 1023; ++e) {  // the powers of two, where spacing changes
        const double p = std::ldexp(1.0, e);
        values.insert(values.end(), {p, std::nextafter(p, 0.0), std::nextafter(p, INFINITY)});
    }
    for (int e = -323; e <= 308; ++e) {
        const double p = std::pow(10.0, e);
        for (int k = 1; k <= 9; ++k) values.push_back(k * p);
        values.insert(values.end(), {std::nextafter(p, 0.0), std::nextafter(p, INFINITY)});
    }
    std::mt19937_64 rng(20);
    for (int i = 0; i < 100000; ++i) {
        const double v = std::bit_cast<double>(rng());
        if (std::isfinite(v)) values.push_back(v);
    }
    int differ = 0;
    for (double v : values) {
        if (std::string(detail::format_double_fallback(v).view()) == to_chars(v)) continue;
        if (++differ <= 10)
            CHECK(std::string(detail::format_double_fallback(v).view()) == to_chars(v));
    }
    CHECK(differ == 0);
}
#endif

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

    CHECK(detail::format_double_fallback(120.0) == "120");  // not 1.2e+02
    CHECK(detail::format_double_fallback(-0.25) == "-0.25");
    CHECK(detail::format_double_fallback(1e-5) == "1e-05");
    CHECK(detail::format_double_fallback(0.001) == "0.001");
    CHECK(detail::format_double_fallback(1e16) == "1e+16");
    CHECK(detail::format_double_fallback(1e-300) == "1e-300");
    CHECK(detail::format_double_fallback(123456789.125) == "123456789.125");
    CHECK(detail::format_double_fallback(1152921504606846976.0) == "1152921504606846976");

    // A locale's decimal point never reaches the text.
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "German_Germany.1252"}) {
        if (!std::setlocale(LC_NUMERIC, name)) continue;
        CHECK(detail::format_double_fallback(12.75) == "12.75");
        std::setlocale(LC_NUMERIC, "C");
        break;
    }
}

// --- Infix ---

namespace {
Node command_of(std::vector<Node> nodes) {
    return Node(Array::make_from(nodes), Type::Command);
}
Ref<Array> root_of(std::vector<Node> nodes) {
    return Array::make_from(nodes);
}
}  // namespace

TEST_CASE("a prefix command quotes an operator at an odd slot, so every tree round-trips") {
    // Built in C++, never flagged: the reader must not take any of these infix.
    const Node plus = command_of({Node(Symbol("+")), Node(Symbol("-")), Node(1)});
    const Node minus = command_of({Node(Symbol("-")), Node(Symbol("-")), Node(Symbol("-"))});
    const Node tern = command_of({Node(Symbol("a")), Node(Symbol("?")), Node(Symbol("b")),
                                  Node(Symbol(":")), Node(Symbol("c"))});
    const Node tern_num =
        command_of({Node(1), Node(Symbol("?")), Node(2), Node(Symbol(":")), Node(3)});
    const Node call = command_of({Node(Symbol("f")), Node(Symbol("+")), Node(1), Node(2)});
    const Node prefix =
        command_of({Node(Symbol("<=")), Node(0.3), Node::var(Symbol("v")), Node(0.7)});
    const Node f_minus = command_of({Node(Symbol("f")), Node(Symbol("-")), Node(1)});
    // An even brace whose head is an operand would read as malformed infix: quoted too.
    const Node handler =
        command_of({Node::var(Symbol("obj")), Node(Symbol("<=")), Node(1), Node(2)});
    const Node bare = command_of({Node(0.3), Node(Symbol("-"))});
    CHECK(to_string(plus) == "{+ '-' 1}");
    CHECK(to_string(minus) == "{- - -}");  // bare symbols only: the guard keeps it a command
    CHECK(to_string(tern) == "{a ? b : c}");
    CHECK(to_string(tern_num) == "{1 ? 2 : 3}");  // `?` is no operator
    CHECK(to_string(call) == "{f + 1 2}");        // an even count under a call's head: today's text
    CHECK(to_string(prefix) == "{<= 0.3 $v 0.7}");
    CHECK(to_string(f_minus) == "{f '-' 1}");
    CHECK(to_string(handler) == "{$obj '<=' 1 2}");
    CHECK(to_string(bare) == "{0.3 '-'}");
    const auto root = root_of({plus, minus, tern, tern_num, call, prefix, f_minus, handler, bare});
    check_round_trip(*root);
    const auto again = parse(to_dta(*root));
    for (const Node& n : *again) CHECK_FALSE(n.array()->written_infix());
}

TEST_CASE("a command flagged written infix prints infix and reads back flagged") {
    const std::string text =
        "(pass {0.3 <= $value <= 0.7})\n{$i += 1}\n{- - 5}\n{$kind == 'bacon and eggs'}\n"
        "{1 + - + 2}\n";
    auto root = parse(text);
    CHECK(to_dta(*root) == text);
    auto again = parse(to_dta(*root));
    CHECK(structurally_equal(*root, *again));
    for (size_t i = 1; i < again->size(); ++i) CHECK((*again)[i].array()->written_infix());
    // A C++-built chain a tool flags prints infix; flagged but unfit (symbols only), prefix.
    auto sum = root_of({Node(Symbol("+")), Node(1), Node(2), Node(3)});
    sum->set_written_infix(true);
    CHECK(to_string(Node(sum, Type::Command)) == "{1 + 2 + 3}");
    auto syms = root_of({Node(Symbol("+")), Node(Symbol("a")), Node(Symbol("b"))});
    syms->set_written_infix(true);
    CHECK(to_string(Node(syms, Type::Command)) == "{+ a b}");
    auto one = root_of({Node(Symbol("-")), Node(5)});
    one->set_written_infix(true);
    CHECK(to_string(Node(one, Type::Command)) == "{- 5}");
    auto four = root_of({Node(Symbol("if")), Node(1), Node(2), Node(3), Node(4)});
    four->set_written_infix(true);
    CHECK(to_string(Node(four, Type::Command)) == "{if 1 2 3 4}");
    // An operator that would open a comment, `/*`, never prints between operands.
    auto comment = root_of({Node(Symbol("/*")), Node(1), Node(2)});
    comment->set_written_infix(true);
    CHECK(to_string(Node(comment, Type::Command)) == "{'/*' 1 2}");
    // The flag is not equality, and set() on a flagged command keeps the printer honest.
    CHECK(structurally_equal(*sum, *parse("{+ 1 2 3}")->begin()->array()));
    const_cast<Array&>(*sum).set(1, Node(Symbol("a")));
    CHECK(to_string(Node(sum, Type::Command)) == "{a + 2 + 3}");
    const_cast<Array&>(*sum).set(2, Node(Symbol("b")));
    const_cast<Array&>(*sum).set(3, Node(Symbol("c")));
    CHECK(to_string(Node(sum, Type::Command)) == "{+ a b c}");
}

TEST_CASE("a long infix command wraps one operand a line, the operator ending each line") {
    auto root = parse(
        "(measure {{hearth.events stuck} + {hearth.events oscillating} + "
        "{hearth.events starving} + {hearth.events script_errors}})");
    CHECK(to_dta(*root, {.indent = 3, .width = 60}) ==
          "(measure\n"
          "   {{hearth.events stuck} +\n"
          "      {hearth.events oscillating} +\n"
          "      {hearth.events starving} +\n"
          "      {hearth.events script_errors}})\n");
    check_round_trip(*root);
}
