// Lexing and parsing: tokens, bounds checks, nesting, and located errors.
#include <doctest.h>
#include <baker/parse.hpp>
#include <baker/source_map.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

#include "text.hpp"

#include <string>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;

namespace {
Ref<Array> parse(std::string_view text) {
    auto r = read_string(text, "t.dta");
    REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : to_string(r.error())));
    return *r;
}
Error parse_error(std::string_view text) {
    auto r = read_string(text, "t.dta");
    REQUIRE_MESSAGE(!r.has_value(), "expected an error for: " << text);
    return r.error();
}
const Node& only(const Ref<Array>& root) {
    REQUIRE(root->size() == 1);
    return (*root)[0];
}
}  // namespace

TEST_CASE("menu example parses with file and line on every array") {
    auto root = parse(
        "; a comment\n"
        "\n"
        "(menu\n"
        "  ('bacon and eggs'\n"
        "    (price 12.75)\n"
        "    (calories 120))\n"
        "  ('fruit and cereal'\n"
        "    (price 11.75)\n"
        "    (calories 12)))\n");
    const Array& menu = *only(root).array();
    CHECK(menu.line() == 3);
    CHECK(menu.file() == Symbol("t.dta"));
    CHECK(menu[0].try_sym() == Symbol("menu"));
    const Array& bacon = *menu[1].array();
    CHECK(bacon.line() == 4);
    CHECK(bacon[0].try_sym() == Symbol("bacon and eggs"));
    CHECK(bacon[1].array()->line() == 5);
    CHECK((*bacon[1].array())[1].try_float() == 12.75f);
    CHECK((*bacon[2].array())[1].try_int() == 120);
    CHECK(root->line() == 1);
}

TEST_CASE("integers") {
    CHECK(only(parse("42")).try_int() == 42);
    CHECK(only(parse("-7")).try_int() == -7);
    CHECK(only(parse("+7")).try_int() == 7);
    CHECK(only(parse("007")).try_int() == 7);
    CHECK(only(parse("0x1F")).try_int() == 31);
    CHECK(only(parse("0XfF")).try_int() == 255);
    CHECK(only(parse("-0x10")).try_int() == -16);
    // Hex denotes its unsigned value: 0xFFFFFFFF is 4294967295, not -1.
    CHECK(only(parse("0xFFFFFFFF")).try_int() == 4294967295);
    CHECK(only(parse("0x80000000")).try_int() == 2147483648);
    CHECK(only(parse("0x7FFFFFFFFFFFFFFF")).try_int() == INT64_MAX);
    CHECK(only(parse("0x0000000000000000001")).try_int() == 1);
    CHECK(parse_error("0x8000000000000000").code() == ErrorCode::Range);
    CHECK(parse_error("0xFFFFFFFFFFFFFFFFF").code() == ErrorCode::Range);
    CHECK(only(parse("2147483648")).try_int() == 2147483648);
    CHECK(only(parse("9223372036854775807")).try_int() == INT64_MAX);
    CHECK(only(parse("-9223372036854775808")).try_int() == INT64_MIN);
    CHECK(parse_error("9223372036854775808").code() == ErrorCode::Range);
    CHECK(parse_error("-9223372036854775809").code() == ErrorCode::Range);
    CHECK(parse_error("99999999999999999999999").code() == ErrorCode::Range);
}

TEST_CASE("floats") {
    CHECK(only(parse("12.75")).try_float() == 12.75f);
    CHECK(only(parse("1e-3")).type() == Type::Float);
    CHECK(only(parse("1e-3")).try_float() == 1e-3);
    CHECK(only(parse("0.1")).try_float() == 0.1);
    CHECK(only(parse("1e39")).try_float() == 1e39);
    CHECK(only(parse("4.9e-324")).try_float() > 0.0);  // subnormals are fine
    CHECK(only(parse(".5")).try_float() == 0.5f);
    CHECK(only(parse("-.5")).try_float() == -0.5f);
    CHECK(only(parse("5.")).try_float() == 5.0f);
    CHECK(only(parse("2.5E2")).try_float() == 250.0f);
    CHECK(only(parse("-0.0")).float_bits() == 0x8000000000000000u);
    CHECK(parse_error("1e309").code() == ErrorCode::Range);
    CHECK(parse_error("-1e309").code() == ErrorCode::Range);
    CHECK(parse_error("1e-400").code() == ErrorCode::Range);  // rounds to zero
}

TEST_CASE("atoms that are not numbers are symbols") {
    for (const char* s : {"-", "+", ".", "--", "1e", "0x", "0xZZ", "1.2.3", "inf", "nan", "-inf",
                          "1_000", "12abc", "a'b", "x/y", "../a.dta", "a/*b", "*"}) {
        CAPTURE(s);
        CHECK(only(parse(s)).try_sym() == Symbol(s));
    }
}

TEST_CASE("quoted symbols, strings, and variables") {
    CHECK(only(parse("'odd sym'")).try_sym() == Symbol("odd sym"));
    CHECK(only(parse("''")).try_sym() == Symbol());
    CHECK(only(parse("'12'")).try_sym() == Symbol("12"));
    CHECK(only(parse("'multi\nline'")).try_sym() == Symbol("multi\nline"));
    CHECK(only(parse("\"bacon\"")).try_str() == "bacon");
    CHECK(only(parse("\"\"")).try_str() == "");
    CHECK(only(parse(R"("say \qhi\q")")).try_str() == "say \"hi\"");
    CHECK(only(parse(R"("a\nb")")).try_str() == "a\nb");
    CHECK(only(parse(R"("C:\games\x")")).try_str() == R"(C:\games\x)");
    CHECK(only(parse(R"("end\")")).try_str() == "end\\");
    CHECK(only(parse("\"two\nlines\"")).try_str() == "two\nlines");
    CHECK(only(parse("$game_time")).var_name() == Symbol("game_time"));
    CHECK(parse_error("$").code() == ErrorCode::Syntax);
    CHECK(parse_error("\"open").message() == "unterminated string");
    CHECK(parse_error("'open").code() == ErrorCode::Syntax);
}

TEST_CASE("comments and whitespace") {
    auto root = parse("; line comment\n(a /* block\ncomment */ 1) ; trailing\n/**/(b)");
    REQUIRE(root->size() == 2);
    CHECK(to_string(*root) == "((a 1) (b))");
    CHECK((*root)[1].array()->line() == 4);
    CHECK(parse_error("(a) /* never closed\n\n").line() == 1);
    CHECK(to_string(*parse("\xEF\xBB\xBF(bom)")) == "((bom))");
    auto crlf = parse("(a)\r\n(b)\r\n\r\n(c)");
    CHECK((*crlf)[2].array()->line() == 4);
    CHECK(to_string(*parse("(\x01\x7f\t\v\f x)")) == "((\x7f x))");
    CHECK(parse("")->empty());
    CHECK(parse("   \n ; only a comment")->empty());
}

TEST_CASE("brackets") {
    auto root = parse("(a {b c} [d])");
    const Array& a = *only(root).array();
    CHECK(a[1].type() == Type::Command);
    CHECK(a[2].type() == Type::Property);
    CHECK(to_string(*parse("() {} (())")) == "(() {} (()))");

    Error e = parse_error("(a\n(b\n)");
    CHECK(e.message() == "'(' is never closed");
    CHECK(e.line() == 1);
    e = parse_error("(a\n {b)\n");
    CHECK(e.message() == "'{' from line 2 is closed by ')'");
    CHECK(e.line() == 2);
    CHECK(parse_error("(a))").message() == "unexpected ')'");
    CHECK(parse_error("]").message() == "unexpected ']'");
}

TEST_CASE("nesting is bounded") {
    std::string ok(256, '('), deep(257, '(');
    ok += std::string(256, ')');
    deep += std::string(257, ')');
    CHECK(parse(ok)->height() == 257);
    CHECK(parse_error(deep).code() == ErrorCode::Limit);
    // An unterminated pathological input fails fast rather than recursing.
    CHECK(parse_error(std::string(100000, '(')).code() == ErrorCode::Limit);
}

TEST_CASE("errors format with file and line") {
    Error e = parse_error("(a 1)\n(b 0x1FFFFFFFFFFFFFFFF)");
    CHECK(e.file() == Symbol("t.dta"));
    CHECK(e.line() == 2);
    CHECK(to_string(e) == "baker: integer 0x1FFFFFFFFFFFFFFFF is out of 64-bit range  (t.dta:2:4)");
}

TEST_CASE("reading text and files") {
    CHECK(to_string(*ok(read_string("(x 1)"))) == "((x 1))");
    CHECK_FALSE(read_string("(x"));
    CHECK_FALSE(read_file("/definitely/not/here.dta"));
}

TEST_CASE("missing file is an io error") {
    auto r = read_file("/definitely/not/here.dta");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code() == ErrorCode::Io);
}

TEST_CASE("include paths are handled lexically") {
    using detail::normalize_path;
    CHECK(normalize_path("a/./b/../c.dta") == "a/c.dta");
    CHECK(normalize_path("game/sub/part.dta") == "game/sub/part.dta");
    CHECK(normalize_path("a//b") == "a/b");
    CHECK(normalize_path("../x/../../y.dta") == "../../y.dta");
    CHECK(normalize_path("/a/../../b.dta") == "/b.dta");
    CHECK(normalize_path("C:\\games\\..\\cfg\\x.dta") == "C:/cfg/x.dta");
    CHECK(normalize_path("./") == ".");
    CHECK(normalize_path("<string>") == "<string>");
    CHECK(normalize_path("") == "");
    CHECK(detail::is_absolute_path("/a"));
    CHECK(detail::is_absolute_path("C:\\a"));
    CHECK_FALSE(detail::is_absolute_path("C:a"));
    CHECK_FALSE(detail::is_absolute_path("a/b"));
    CHECK(detail::parent_path("a/b/c.dta") == "a/b");
    CHECK(detail::parent_path("c.dta") == "");
    CHECK(detail::parent_path("/c.dta") == "/");
    CHECK(detail::parent_path("C:/c.dta") == "C:/");

    // A scheme names a virtual root: absolute, and its "//" kept as written.
    CHECK(normalize_path("user://baker/./local.dta") == "user://baker/local.dta");
    CHECK(normalize_path("user://../a.dta") == "user://a.dta");
    CHECK(normalize_path("my-pak+2.v1://x/y.dta") == "my-pak+2.v1://x/y.dta");
    CHECK(detail::is_absolute_path("user://a.dta"));
    CHECK(detail::parent_path("user://baker/local.dta") == "user://baker");
    CHECK(detail::parent_path("user://local.dta") == "user://");
    CHECK(detail::parent_path("user://") == "user://");
    CHECK(detail::join_path("user://baker", "inc.dta") == "user://baker/inc.dta");
    // Not schemes: a drive, a relative path with a colon, a scheme starting with a digit.
    CHECK(normalize_path("C://data//a.dta") == "C:/data/a.dta");
    CHECK_FALSE(detail::is_absolute_path("a b://x"));
    CHECK_FALSE(detail::is_absolute_path("1x://y"));
    CHECK(detail::join_path("", "x.dta") == "x.dta");
    CHECK(detail::join_path("dir", "x.dta") == "dir/x.dta");
    CHECK(detail::join_path("dir/", "x.dta") == "dir/x.dta");
}

TEST_CASE("parse errors carry a column, in characters from the line's start") {
    struct Case {
        const char* text;
        int line, column;
    };
    const Case cases[] = {
        {"(a 1)\n  (b \"open", 2, 6},               // unterminated string: at its quote
        {"(a 'open", 1, 4},                         // unterminated quoted symbol
        {"(a 1) /* never", 1, 7},                   // unterminated comment: at its /*
        {"(a $)", 1, 4},                            // '$' without a name
        {"(a\n   99999999999999999999)", 2, 4},     // integer out of range
        {"(a 1))", 1, 6},                           // unexpected ')'
        {"(a (b 1]", 1, 8},                         // ( closed by ]
        {"(a 1)\n   (b 2", 2, 4},                   // never closed: at the bracket
        {"(a 1)\n #frobnicate", 2, 2},              // unknown directive
        {"(a 1) #else", 1, 7},                      // #else without #ifdef
        {"(name \"\xC3\xA9t\xC3\xA9\") $", 1, 14},  // two-byte characters count once
        {"\xEF\xBB\xBF(a \"open", 1, 4},            // a byte-order mark is not a character
    };
    for (const Case& c : cases) {
        CAPTURE(c.text);
        const Error e = parse_error(c.text);
        CHECK(e.line() == c.line);
        CHECK(e.column() == c.column);
    }
    const Error e = parse_error("(a 1)\n  (b \"open");
    CHECK(to_string(e) == "baker: unterminated string  (t.dta:2:6)");
}

TEST_CASE("an error located by an array's line has no column") {
    const auto root = read_string("(speed fast)", "t.dta");
    REQUIRE(root);
    const auto v = (*root)->get<int>("speed");
    REQUIRE_FALSE(v);
    CHECK(v.error().line() == 1);
    CHECK(v.error().column() == 0);
    CHECK(to_string(v.error()) == "baker: 'speed' is symbol, not int  (t.dta:1)");
    // A pack declaration's problem names the declaration's line only.
    const Error pack = parse_error("#pack (p (level 3))");
    CHECK(pack.column() == 0);
}

// --- The infix rule ---

namespace {
const Array& command_at(const Ref<Array>& root, size_t i) {
    REQUIRE(i < root->size());
    REQUIRE((*root)[i].type() == Type::Command);
    return *(*root)[i].array();
}
}  // namespace

TEST_CASE("a brace whose odd slots are one operator reads head-first, its spans in order") {
    const std::string text = "(pass {0.3 <= $value <= 0.7})";
    LoadSession s;
    SourceMap map;
    s.source_map = &map;
    auto root = read_string(text, "t.dta", s);
    REQUIRE(root);
    const Array& pass = *(**root)[0].array();
    REQUIRE(pass.size() == 2);
    REQUIRE(pass[1].type() == Type::Command);
    const Array& band = *pass[1].array();
    REQUIRE(band.size() == 4);
    CHECK(band[0].is("<="));
    CHECK(band[1].try_float() == 0.3);
    CHECK(band[2].type() == Type::Var);
    CHECK(band[2].var_name() == Symbol("value"));
    CHECK(band[3].try_float() == 0.7);
    CHECK(band.written_infix());
    CHECK(band.line() == 1);
    CHECK(to_string(pass[1]) == "{0.3 <= $value <= 0.7}");  // printed back as written
    // Each node's span is the token it came from; the head's is the first operator's.
    auto span_text = [&](size_t i) {
        const SourceSpan* sp = map.span(band, i);
        REQUIRE(sp);
        return text.substr(sp->begin, sp->end - sp->begin);
    };
    CHECK(span_text(0) == "<=");
    CHECK(span_text(1) == "0.3");
    CHECK(span_text(2) == "$value");
    CHECK(span_text(3) == "0.7");
    REQUIRE(map.extent(band));
    CHECK(map.extent(band)->begin == 6);
    CHECK(map.extent(band)->end == text.size() - 1);
    // Longer chains, and a chain of commands.
    root = read_string("{1 + 2 + 3 + 4} {{a} - {b} - {c}}", "t.dta");
    REQUIRE(root);
    const Array& sum = command_at(*root, 0);
    REQUIRE(sum.size() == 5);
    CHECK(sum[0].is("+"));
    for (size_t i = 1; i < 5; ++i) CHECK(sum[i].try_int() == static_cast<int64_t>(i));
    const Array& diff = command_at(*root, 1);
    REQUIRE(diff.size() == 4);
    CHECK(diff[0].is("-"));
    CHECK((*diff[1].array())[0].is("a"));
    CHECK((*diff[3].array())[0].is("c"));
}

TEST_CASE("mixed operators in one brace are an error at the brace, with its column") {
    const Error e = parse_error("(pass\n   {0 < $value <= 10})");
    CHECK(e.code() == ErrorCode::Syntax);
    CHECK(e.message() == "mixed operators in {...}: nest braces to say which applies first");
    CHECK(e.line() == 2);
    CHECK(e.column() == 4);
    CHECK(to_string(e) ==
          "baker: mixed operators in {...}: nest braces to say which applies first  (t.dta:2:4)");
    // Nested braces say which applies first.
    auto root = parse("{and {0 < $value} {$value <= 10}}");
    const Array& both = command_at(root, 0);
    CHECK(both[0].is("and"));
    CHECK((*both[1].array())[0].is("<"));
    CHECK((*both[2].array())[0].is("<="));
    CHECK(both[1].array()->written_infix());
    CHECK_FALSE(both.written_infix());
}

TEST_CASE("malformed infix is an error at the brace that names the fix") {
    // An operand at an operator's slot, or a brace that ends with its operator: the shapes
    // that read as a call of their first operand before, which check let pass.
    const Error band = parse_error("(pass\n   {0.3 <= $value 0.7})");
    CHECK(band.code() == ErrorCode::Syntax);
    CHECK(band.message() == "infix {...} needs '<=' between every two operands");
    CHECK(band.line() == 2);
    CHECK(band.column() == 4);
    CHECK(to_string(band) ==
          "baker: infix {...} needs '<=' between every two operands  (t.dta:2:4)");
    CHECK(parse_error("{$value <= 0.7 0.3}").message() ==
          "infix {...} needs '<=' between every two operands");
    const Error open = parse_error("(pass {$value <=})");
    CHECK(open.message() == "infix {...} ends with '<=': write an operand after it");
    CHECK(open.column() == 7);
    // Their siblings: the operator at the end of a longer brace, an operand where the
    // operator should be in an odd brace (not "mixed"), and heads that are a property, a
    // command, a number.
    CHECK(parse_error("{0.3 <= $value <=}").message() ==
          "infix {...} ends with '<=': write an operand after it");
    CHECK(parse_error("{$value <= 0.7 0.3 1}").message() ==
          "infix {...} needs '<=' between every two operands");
    CHECK(parse_error("{[hp] -= 5 1}").message() ==
          "infix {...} needs '-=' between every two operands");
    CHECK(parse_error("{{econ.count mill} >=}").message() ==
          "infix {...} ends with '>=': write an operand after it");
    CHECK(parse_error("{1 + 2 +}").message() ==
          "infix {...} ends with '+': write an operand after it");
    CHECK(parse_error("#autorun {$i +=}").message() ==
          "infix {...} ends with '+=': write an operand after it");
    // What stays a command: a call's head (a symbol) before an operator argument, an
    // operator quoted (an object's handler named by one), a prefix call, a head-first call
    // through a command or variable, `?`, which is no operator, and a chain.
    auto root = parse(
        "{f + 1 2} {f <=} {$obj '<=' 1 2} {$obj '-'} {<= 0.3 $v 0.7} {- 5 2 1} "
        "{{find enemy} hit 10 2} {$obj move 1 2} {$c ? 1} {0.3 <= $v <= 0.7}");
    CHECK(command_at(root, 0)[0].is("f"));
    CHECK(command_at(root, 1).size() == 2);
    CHECK(command_at(root, 2)[0].type() == Type::Var);
    CHECK(command_at(root, 2)[1].is("<="));
    CHECK(command_at(root, 3).size() == 2);
    CHECK(command_at(root, 4)[0].is("<="));
    CHECK(command_at(root, 5).size() == 4);
    CHECK(command_at(root, 6)[1].is("hit"));
    CHECK(command_at(root, 7)[1].is("move"));
    CHECK(command_at(root, 8)[1].is("?"));
    CHECK(command_at(root, 9).written_infix());
}

TEST_CASE("the guard, quoting and macros keep today's braces as they are") {
    // Bare symbols only: a command as today, whatever the symbol at slot 1.
    auto root = parse("{wheat -> flour} {a - b} {$a -> b} {a ? b : c}");
    const Array& raw = command_at(root, 0);
    CHECK(raw[0].is("wheat"));
    CHECK(raw[1].is("->"));
    CHECK_FALSE(raw.written_infix());
    CHECK(command_at(root, 1)[0].is("a"));
    CHECK(command_at(root, 3)[0].is("a"));
    CHECK(command_at(root, 3).size() == 5);
    // A variable among the operands fires it.
    const Array& fired = command_at(root, 2);
    CHECK(fired[0].is("->"));
    CHECK(fired[1].type() == Type::Var);
    CHECK(fired.written_infix());
    // A quoted symbol at an odd slot exempts the brace; at an even slot it is an operand.
    root = parse("{+ '-' 1} {1 + '-'} {1 '+' 2}");
    const Array& q = command_at(root, 0);
    CHECK(q[0].is("+"));
    CHECK(q[1].is("-"));
    CHECK(q[2].try_int() == 1);
    CHECK_FALSE(q.written_infix());
    const Array& e = command_at(root, 1);
    CHECK(e[0].is("+"));
    CHECK(e[1].try_int() == 1);
    CHECK(e[2].is("-"));
    CHECK(e.written_infix());
    CHECK(command_at(root, 2)[0].try_int() == 1);
    // An even count under a call's head, a unary minus, a call with an operator argument,
    // and a prefix call are commands as today; `{<= 0.3 $v 0.7}` written prefix is not
    // flagged.
    root = parse("{f + 2 +} {- 5} {f + 1 2} {<= 0.3 $v 0.7} {$x = 1}");
    CHECK(command_at(root, 0).size() == 4);
    CHECK(command_at(root, 0)[0].is("f"));
    CHECK(command_at(root, 1)[0].is("-"));
    CHECK(command_at(root, 1).size() == 2);
    CHECK(command_at(root, 2)[0].is("f"));
    CHECK(command_at(root, 3)[0].is("<="));
    CHECK_FALSE(command_at(root, 3).written_infix());
    CHECK(command_at(root, 4)[0].is("="));  // `=` is a call of `=`, which nothing defines
    // A macro used at an odd slot, or expanding to several nodes, exempts its brace; a
    // one-node macro at an even slot is an operand.
    root = parse(
        "[OP +]\n[K 5]\n[TRIPLE 2 3 4]\n"
        "{1 OP 2} {$x + K} {1 + TRIPLE}");
    CHECK(command_at(root, 0)[0].try_int() == 1);
    CHECK(command_at(root, 0)[1].is("+"));
    CHECK(command_at(root, 1)[0].is("+"));
    CHECK(command_at(root, 1)[2].try_int() == 5);
    CHECK(command_at(root, 1).written_infix());
    CHECK(command_at(root, 2).size() == 5);
    CHECK(command_at(root, 2)[0].try_int() == 1);
    // Nothing in a parenthesized array or a property changes.
    root = parse("(a 1 + 2) [x + 1]");
    CHECK((*(*root)[0].array())[0].is("a"));
    CHECK((*(*root)[0].array()).size() == 4);
}

TEST_CASE("an #autorun brace reads infix too") {
    LoadSession s;
    auto r = read_string("#autorun {$count += 1}", "t.dta", s);
    REQUIRE(r);
    REQUIRE(s.autorun().size() == 1);
    const Array& a = *s.autorun()[0].array();
    REQUIRE(a.size() == 3);
    CHECK(a[0].is("+="));
    CHECK(a[1].type() == Type::Var);
    CHECK(a[2].try_int() == 1);
    CHECK(a.written_infix());
}
