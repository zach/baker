// Node, Symbol, Ref, String, Array: layout, ownership, and checked access.
#include <doctest.h>
#include <baker/array.hpp>
#include <baker/callback.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace baker;

TEST_CASE("layout targets hold") {
    static_assert(sizeof(Node) == 16);
    static_assert(alignof(Node) == 8);
    static_assert(sizeof(Array) <= 24);
    CHECK(sizeof(Node) == 16);
    CHECK(sizeof(Array) == 24);
}

TEST_CASE("symbols are interned and compare by identity") {
    Symbol a("price");
    Symbol b(std::string("pri") + "ce");
    CHECK(a == b);
    CHECK(a.id() == b.id());
    CHECK(a.str() == "price");
    CHECK(a.hash() == detail::hash_symbol_text("price"));
    CHECK("price"_sym == a);
    CHECK_FALSE(Symbol("Price") == a);
    CHECK(Symbol() == Symbol(""));
    CHECK(Symbol().empty());
    CHECK(Symbol(std::string_view("with\0nul", 8)).str().size() == 8);
    CHECK(std::string(Symbol("abc").c_str()) == "abc");
}

TEST_CASE("symbol table grows and stays consistent") {
    std::vector<Symbol> syms;
    for (int i = 0; i < 20000; ++i) syms.emplace_back("grow_" + std::to_string(i));
    for (int i = 0; i < 20000; ++i)
        CHECK(syms[static_cast<size_t>(i)] == Symbol("grow_" + std::to_string(i)));
    CHECK(Symbol::table_size() >= 20000);
}

TEST_CASE("symbol interning is thread-safe") {
    std::vector<Symbol> a(2000), b(2000);
    std::thread t1([&] {
        for (size_t i = 0; i < a.size(); ++i) a[i] = Symbol("mt_" + std::to_string(i));
    });
    std::thread t2([&] {
        for (size_t i = 0; i < b.size(); ++i) b[i] = Symbol("mt_" + std::to_string(i));
    });
    t1.join();
    t2.join();
    CHECK(a == b);
}

TEST_CASE("node construction and types") {
    CHECK(Node().type() == Type::Int);
    CHECK(Node(7).type() == Type::Int);
    CHECK(Node(7L).type() == Type::Int);
    CHECK(Node(7u).type() == Type::Int);
    CHECK(Node(uint64_t{UINT64_MAX}).try_int() == -1);  // keeps its bit pattern
    CHECK(Node(INT64_MAX).try_int() == INT64_MAX);
    CHECK(Node(1.5f).type() == Type::Float);
    CHECK(Node(1.5).type() == Type::Float);
    CHECK(Node(Symbol("x")).type() == Type::Symbol);
    CHECK(Node(str("hi")).type() == Type::String);
    CHECK(Node(array(1, 2)).type() == Type::Array);
    CHECK(Node(array(1), Type::Command).type() == Type::Command);
    CHECK(Node(array(1), Type::Property).type() == Type::Property);
    CHECK(Node::var("v").type() == Type::Var);
    CHECK(Node::var("v").var_name() == Symbol("v"));
    CHECK(Node::object(ObjectHandle::make(3, 9)).object_handle().index() == 3);
    CHECK(Node::object(ObjectHandle::make(3, 9)).object_handle().salt() == 9);
    CHECK(ObjectHandle::make(0xFFFFFFFFu, 0x12345678u).index() == 0xFFFFFFFFu);
    CHECK(ObjectHandle::make(0xFFFFFFFFu, 0x12345678u).salt() == 0x12345678u);
    static const FuncRecord rec{"plus"};
    CHECK(Node::func(&rec).func_record() == &rec);
    CHECK(type_name(Type::Property) == "property");
}

TEST_CASE("numeric access coerces only where lossless") {
    CHECK(Node(3).try_float() == 3.0f);
    CHECK(Node(4.0f).try_int() == 4);
    CHECK_FALSE(Node(4.5f).try_int().has_value());
    CHECK(Node(3e9).try_int() == 3000000000);
    CHECK_FALSE(Node(1e19).try_int().has_value());
    CHECK_FALSE(Node(-1e19).try_int().has_value());
    CHECK(Node(9007199254740993LL).try_float() == 9007199254740992.0);  // int reads round past 2^53
    CHECK_FALSE(Node(Symbol("x")).try_int().has_value());
    CHECK_FALSE(Node(Symbol("x")).try_float().has_value());
    CHECK(Node(Symbol("x")).try_sym() == Symbol("x"));
    CHECK_FALSE(Node(str("x")).try_sym().has_value());
    CHECK(Node(str("x")).try_str() == "x");
    CHECK_FALSE(Node(Symbol("x")).try_str().has_value());
    CHECK(Node(array(1)).try_array() != nullptr);
    CHECK(Node(array(1), Type::Command).try_array() == nullptr);
    CHECK(Node(array(1), Type::Command).array() != nullptr);
}

TEST_CASE("checked accessors return located type errors") {
    CHECK(Node(12).as_int() == 12);
    CHECK(Node(12).as_float() == 12.0f);
    CHECK(Node(12).as<int64_t>() == 12);
    CHECK(Node(2.5f).as<double>() == 2.5);
    CHECK(Node(2.5).as<float>() == 2.5f);
    CHECK(Node(int64_t{3000000000}).as<int64_t>() == 3000000000);
    CHECK(Node(int64_t{3000000000}).as<uint32_t>() == 3000000000u);
    CHECK(Node(int64_t{3000000000}).as<int32_t>().error().code() == ErrorCode::Range);
    CHECK(Node(1e300).as<float>().error().code() == ErrorCode::Range);
    CHECK(Node(1e-300).as<float>() == 0.0f);  // precision may round; only range is checked
    CHECK(Node(200).as<uint8_t>() == 200);
    CHECK_FALSE(Node(300).as<uint8_t>());
    CHECK_FALSE(Node(-1).as<uint32_t>());
    CHECK_FALSE(Node(Symbol("a")).as_str());
    CHECK_FALSE(Node(str("a")).as_sym());
    const Error e = Node(12.75f).as_int().error();
    CHECK(e.code() == ErrorCode::Type);
    CHECK(e.message() == "12.75 is float, not int");
    auto a = Array::make(1, "where.dta", 12);
    CHECK(to_string(Node(a).as_int().error()) == "baker: (0) is array, not int  (where.dta:12)");
    CHECK(a->try_at(1) == nullptr);
    CHECK(a->try_at(0)->as_int() == 0);
}

TEST_CASE("refcounts follow nodes and arrays") {
    Ref<Array> inner = array(1, 2);
    CHECK(inner->ref_count() == 1);
    {
        Node n(inner);
        CHECK(inner->ref_count() == 2);
        Node copy = n;
        CHECK(inner->ref_count() == 3);
        Node moved = std::move(copy);
        CHECK(inner->ref_count() == 3);
        CHECK(copy.type() == Type::Int);
        Ref<Array> outer = array("outer", n, moved);
        CHECK(inner->ref_count() == 5);
    }
    CHECK(inner->ref_count() == 1);

    Ref<String> s = str("shared");
    {
        Node a(s), b(s);
        CHECK(s->ref_count() == 3);
        a = b;
        CHECK(s->ref_count() == 3);
        a = Node(1);
        CHECK(s->ref_count() == 2);
    }
    CHECK(s->ref_count() == 1);
    CHECK(s->view() == "shared");
}

TEST_CASE("array construction helpers") {
    auto price = array("price", 12.75f);
    auto eggs = array("eggs", price);
    CHECK(to_string(*eggs) == "(eggs (price 12.75))");
    CHECK(eggs->height() == 2);
    CHECK(price->height() == 1);
    CHECK(array()->empty());
    CHECK(to_string(*array("name", str("Bob"), 3, -1.5, Node::var("x"))) ==
          "(name \"Bob\" 3 -1.5 $x)");
    auto made = Array::make(3, "f.dta", 9);
    CHECK(made->size() == 3);
    CHECK(made->file() == Symbol("f.dta"));
    CHECK(made->line() == 9);
    made->set(1, Node(array(array(1))));
    CHECK(made->height() == 3);
    CHECK(made->try_at(3) == nullptr);
    CHECK((*made)[1].array()->height() == 2);
}

TEST_CASE("structural equality ignores locations") {
    auto a = Array::make_copy(std::vector<Node>{Node(1), Node(Symbol("x"))}, "a.dta", 1);
    auto b = Array::make_copy(std::vector<Node>{Node(1), Node(Symbol("x"))}, "b.dta", 99);
    CHECK(structurally_equal(*a, *b));
    CHECK(structurally_equal(Node(5), Node(5)));
    CHECK_FALSE(structurally_equal(Node(1), Node(1.0f)));
    CHECK_FALSE(structurally_equal(Node(0.0f), Node(-0.0f)));
    CHECK_FALSE(structurally_equal(Node(array(1)), Node(array(1), Type::Command)));
    CHECK(structurally_equal(Node(str("s")), Node(str("s"))));
    CHECK_FALSE(structurally_equal(Node(str("s")), Node(Symbol("s"))));
}

#if BAKER_ATOMIC_REFCOUNT_ENABLED
TEST_CASE("atomic refcounts survive concurrent copies") {
    Ref<Array> shared = array("shared", 1, 2, 3);
    auto work = [&] {
        for (int i = 0; i < 10000; ++i) {
            Node n(shared);
            Node m = n;
        }
    };
    std::thread t1(work), t2(work);
    t1.join();
    t2.join();
    CHECK(shared->ref_count() == 1);
}
#endif

TEST_CASE("Expected is std::expected when the library has it") {
    // The choice comes from the library's configure step, and these tests are built
    // at the library's own standard, so it must match what this translation unit sees.
    static_assert(BAKER_STD_EXPECTED == BAKER_LIB_HAS_STD_EXPECTED);
#if BAKER_HAS_STD_EXPECTED
    static_assert(std::is_same_v<Expected<int>, std::expected<int, Error>>);
#endif
    MESSAGE("std::expected in use: " << BAKER_HAS_STD_EXPECTED);
    Expected<int> ok = 3;
    Expected<int> bad = Unexpected(make_error(ErrorCode::NotFound, "nope"));
    CHECK(ok.has_value());
    CHECK(*ok == 3);
    CHECK_FALSE(bad.has_value());
    CHECK(bad.error().message() == "nope");
    CHECK(bad.value_or(7) == 7);
    Expected<void> v;
    CHECK(v.has_value());
}

TEST_CASE("Callback copies, moves, and frees what it holds") {
    static int alive = 0;
    struct Counted {
        int n;
        explicit Counted(int v) : n(v) { ++alive; }
        Counted(const Counted& o) : n(o.n) { ++alive; }
        ~Counted() { --alive; }
        int operator()(int x) const { return n + x; }
    };
    {
        Callback<int(int)> a = Counted(10);
        CHECK(alive == 1);
        CHECK(a(5) == 15);
        Callback<int(int)> b = a;  // a separate copy
        CHECK(alive == 2);
        Callback<int(int)> c = std::move(a);
        CHECK(alive == 2);
        CHECK_FALSE(a);
        CHECK(c(1) == 11);
        b = nullptr;
        CHECK(alive == 1);
        Callback<int(int)> plain = [](int x) { return x * 2; };  // no captures: no allocation
        CHECK(plain(4) == 8);
        CHECK(alive == 1);
    }
    CHECK(alive == 0);
}

TEST_CASE("symbols intern the same text to one entry from several threads") {
    // Four threads intern 2,000 names, half shared by all and half their own, while the
    // table grows under them. Every name reads back as itself, and shared names are one
    // symbol wherever they were interned (the read path takes no lock).
    std::vector<std::vector<Symbol>> per_thread(4);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            auto& mine = per_thread[static_cast<size_t>(t)];
            for (int i = 0; i < 2000; ++i) {
                const std::string name =
                    (i % 2 ? "shared_" : "t" + std::to_string(t) + "_") + std::to_string(i);
                mine.push_back(Symbol(name));
                CHECK(mine.back().str() == name);
            }
        });
    for (auto& th : threads) th.join();
    for (int i = 1; i < 2000; i += 2)
        for (int t = 1; t < 4; ++t)
            CHECK(per_thread[0][static_cast<size_t>(i)] ==
                  per_thread[static_cast<size_t>(t)][static_cast<size_t>(i)]);
    const Symbol again("shared_1");
    CHECK(again == per_thread[2][1]);
}
