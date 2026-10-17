// eval_request and the reply form baker.reply/1.1.
#include <doctest.h>
#include <baker/script.hpp>
#include <baker/std_interop.hpp>

#include <string>

using namespace baker;

namespace {

std::string reply(Context& ctx, std::string_view line) {
    std::string out;
    eval_request(ctx, line, string_out(out));
    REQUIRE(!out.empty());
    CHECK(out.back() == '\n');
    CHECK(out.find('\n') == out.size() - 1);  // one line
    out.pop_back();
    return out;
}

const std::string kHead = R"j({"schema":"baker.reply/1.1","id":)j";

}  // namespace

TEST_CASE("a request's id is echoed, and its result comes back as text and value") {
    Context ctx;
    CHECK(reply(ctx, "(7 {+ 1 2})") == kHead + R"j(7,"ok":true,"text":"3","value":3})j");
    CHECK(reply(ctx, "(req_8 {+ 1 2})") == kHead + R"j("req_8","ok":true,"text":"3","value":3})j");
    CHECK(reply(ctx, R"j(("a b" {* 2 1.25}))j") ==
          kHead + R"j("a b","ok":true,"text":"2.5","value":2.5})j");
    // Any other line is evaluated whole, with a null id.
    CHECK(reply(ctx, "{+ 1 2}") == kHead + R"j(null,"ok":true,"text":"3","value":3})j");
    CHECK(reply(ctx, "{set $x 4} {+ $x 1}") == kHead + R"j(null,"ok":true,"text":"5","value":5})j");
}

TEST_CASE("the projection: keyed arrays become objects, other arrays stay arrays") {
    Context ctx;
    // A tagged array of distinctly tagged children is an object, its own tag dropped.
    CHECK(reply(ctx, R"j((1 {if 1 (source (layer default) (file "d.dta") (line 3))}))j") ==
          kHead + R"j(1,"ok":true,"text":"(source (layer default) (file \"d.dta\") (line 3))",)j"
                  R"j("value":{"layer":"default","file":"d.dta","line":3}})j");
    // An empty rest is null, several values are an array, and keyed children nest.
    CHECK(reply(ctx, "(2 {if 1 ((a) (b 1 2) (c (d 1)))})") ==
          kHead + R"j(2,"ok":true,"text":"((a) (b 1 2) (c (d 1)))",)j"
                  R"j("value":{"a":null,"b":[1,2],"c":{"d":1}}})j");
    CHECK(reply(ctx, "(6 {if 1 (cfg (box (width 2) (height 1)) (name hero))})") ==
          kHead + R"j(6,"ok":true,"text":"(cfg (box (width 2) (height 1)) (name hero))",)j"
                  R"j("value":{"box":{"width":2,"height":1},"name":"hero"}})j");
    // Repeated tags, or a child that is not a tagged array, keep it an array.
    CHECK(reply(ctx, "(3 {if 1 ((a 1) (a 2))})") ==
          kHead + R"j(3,"ok":true,"text":"((a 1) (a 2))","value":[["a",1],["a",2]]})j");
    CHECK(reply(ctx, "(4 {if 1 (phase 10 20.5 x \"s\")})") ==
          kHead + R"j(4,"ok":true,"text":"(phase 10 20.5 x \"s\")",)j"
                  R"j("value":["phase",10,20.5,"x","s"]})j");
    // A list of entries keeps its order, as an array of objects.
    CHECK(reply(ctx, "(5 {if 1 (((layer a) (n 1)) ((layer b) (n 2)))})") ==
          kHead + R"j(5,"ok":true,"text":"(((layer a) (n 1)) ((layer b) (n 2)))",)j"
                  R"j("value":[{"layer":"a","n":1},{"layer":"b","n":2}]})j");
}

TEST_CASE("an error comes back located, with its script stack") {
    Context ctx;
    REQUIRE(ctx.eval("{func inner () {nosuch 1}} {func outer () {inner}}", "lib.dta"));
    const std::string r = reply(ctx, "(9 {outer})");
    CHECK(r.rfind(kHead + R"j(9,"ok":false,"error":{"code":)j", 0) == 0);
    CHECK(r.find(R"j("message":)j") != std::string::npos);
    CHECK(r.find(R"j("stack":[{"function":"inner")j") != std::string::npos);
    CHECK(r.find(R"j({"function":"outer")j") != std::string::npos);
    CHECK(r.find(R"j("file":"lib.dta")j") != std::string::npos);
}

TEST_CASE("a line that does not parse gets an error reply with a null id") {
    Context ctx;
    const std::string r = reply(ctx, "(3 {+ 1");
    CHECK(r.rfind(kHead + R"j(null,"ok":false,"error":{"code":)j", 0) == 0);
    CHECK(r.find(R"j("file":"<console>")j") != std::string::npos);
    // Text that needs escaping stays one line.
    const std::string s = reply(ctx, "(4 \"tab\there\nquote \\\" end\")");
    CHECK(s.find('\n') == std::string::npos);
}
