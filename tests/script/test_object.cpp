// Objects: binding, dispatch in both directions, handles, script classes (M5).
#include <doctest.h>
#include <baker/object.hpp>
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/std_script.hpp>

#include "alloc_counter.hpp"

#include <string>
#include <vector>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;

namespace {

int g_mom_calls = 0;

// The talk's example.
struct Person : Object<Person> {
    float height = 6.0f;

    void tell_mom() { ++g_mom_calls; }
    float on_grow(float amount) {
        height += amount;
        tell_mom();
        return height;
    }

    static void bind(Binder<Person>& b) {
        b.method("grow", &Person::on_grow).doc("Grows by `amount`.");
        b.property("height", &Person::height);
        b.method("shrink", [](Person& self, float a) { self.height -= a; });
    }
};

struct Creature : Object<Creature> {
    int hp = 10;
    int damage(int n) { return hp -= n; }
    static void bind(Binder<Creature>& b) {
        b.method("damage", &Creature::damage);
        b.method("roar", [](Creature&) { return Symbol("grr"); });
        b.property("hp", &Creature::hp);
    }
};

struct Hero : Object<Hero, Creature> {
    int armor = 3;
    static void bind(Binder<Hero>& b) {
        b.inherits<Creature>();  // already implied by Object<Hero, Creature>
        b.method("roar", [](Hero&) { return Symbol("for glory"); });
        b.method("damage", [](Hero& h, int n) { return h.damage(n > h.armor ? n - h.armor : 0); });
        b.property("armor", &Hero::armor);
    }
};

}  // namespace

namespace {

Error error_of(Context& ctx, std::string_view text) {
    auto r = ctx.eval(text, "obj.dta");
    REQUIRE_FALSE(r);
    return r.error();
}

}  // namespace

TEST_CASE("Person::grow runs from script and from C++") {
    Context ctx;
    Person bob;
    g_mom_calls = 0;
    ok(ctx.add(bob, "bob"));
    CHECK(bob.type_name() == "Person");
    CHECK(ok(ctx.eval("{bob grow 10}")).as_float() == 16.0);  // script -> C++
    CHECK(bob.height == 16.0f);
    CHECK(ok(ctx.call(bob, "grow", 1.5)).as_float() == 17.5);  // C++ -> the same dispatch
    CHECK(bob.on_grow(0.5f) == 18.0f);                         // and plain C++
    CHECK(g_mom_calls == 3);
    CHECK(ok(ctx.eval("{bob get height}")).as_float() == 18.0);
    CHECK(ok(ctx.eval("{bob set height 3}")).as_int() == 3);
    CHECK(bob.height == 3.0f);
    ok(ctx.eval("{bob shrink 1}"));
    CHECK(bob.height == 2.0f);
    CHECK(ok(ctx.eval("{bob type}")).as_sym() == Symbol("Person"));
    CHECK(ok(ctx.eval("{bob name}")).as_sym() == Symbol("bob"));
}

TEST_CASE("C++ calls a handler that script defined; script handlers come first") {
    Context ctx;
    Person bob;
    ok(ctx.add(bob, "bob"));
    ok(
        ctx.eval("{class Person\n"
                 "  (miss {set [height] {* [height] 2}} whoosh)\n"
                 "  (grow ($n) {set [height] {+ [height] {* $n 100}}})}"));
    CHECK(ok(ctx.call(bob, "miss")).as_sym() == Symbol("whoosh"));  // C++ -> script
    CHECK(bob.height == 12.0f);
    CHECK(ok(ctx.eval("{bob grow 1}")).as_float() == 112.0);  // the script handler wins
    CHECK_FALSE(ctx.var("this").is_set());                    // $this is restored afterwards
    CHECK(ok(ctx.eval("{bob shrink 2}")).as_int() == 0);      // the native method still serves
    CHECK(bob.height == 110.0f);
    // A handler's arity is checked.
    CHECK(error_of(ctx, "{bob grow}").message() == "'grow' takes 1 argument, given 0");
}

TEST_CASE("inheritance: methods and properties are flattened, a derived method replaces") {
    Context ctx;
    Hero hero;
    Creature rat;
    const Node h = ok(ctx.add(hero, "hero"));
    ok(ctx.add(rat, "rat"));
    CHECK(ok(ctx.eval("{rat roar}")).as_sym() == Symbol("grr"));
    CHECK(ok(ctx.eval("{hero roar}")).as_sym() == Symbol("for glory"));
    CHECK(ok(ctx.eval("{hero damage 5}")).as_int() == 8);  // the armor absorbs 3
    CHECK(ok(ctx.eval("{hero get hp}")).as_int() == 8);    // inherited property
    CHECK(ok(ctx.eval("{hero get armor}")).as_int() == 3);
    CHECK(ctx.try_get<Creature>(h) == &hero);
    CHECK(ctx.try_get<Hero>(h) == &hero);
    CHECK(ctx.try_get<Person>(h) == nullptr);
    CHECK(ctx.get<Creature>(h) == &hero);
    CHECK(to_string(ctx.get<Person>(h).error()) == "baker: the object is a Hero, not a Person");
    // Script handlers attached to a base type apply to derived objects.
    ok(ctx.eval("{class Creature (heal ($n) {set [hp] {+ [hp] $n}})}"));
    CHECK(ok(ctx.eval("{hero heal 2}")).as_int() == 10);
}

TEST_CASE("a destroyed object leaves a stale handle, never a dangling pointer") {
    Context ctx;
    Node old;
    {
        Person p;
        old = ok(ctx.add(p, "temp"));
        ctx.var("p") = old;
        CHECK(ok(ctx.eval("{$p grow 1}")).as_float() == 7.0);
    }  // p removes itself
    CHECK(ctx.try_resolve(old) == nullptr);
    CHECK(ctx.object("temp").type() == Type::Int);
    CHECK(error_of(ctx, "\n{$p grow 1}").message() ==
          "the object this command is sent to was removed (a stale handle)");
    CHECK(error_of(ctx, "{temp grow 1}").message() == "'temp' is not a function");
    Person q;
    const Node fresh = ok(ctx.add(q));  // likely reuses the slot, with a new salt
    CHECK(ctx.try_resolve(fresh) == &q);
    CHECK(ctx.try_resolve(old) == nullptr);
    CHECK(to_string(ctx.get<Person>(old).error()) == "baker: the Person object was removed");
}

TEST_CASE("object commands evaluate slot 0") {
    Context ctx;
    Person bob;
    ok(ctx.add(bob, "bob"));
    ok(ctx.func("nearest", [&](Context& c) { return c.object("bob"); }));
    CHECK(ok(ctx.eval("{{nearest} grow 1}")).as_float() == 7.0);
    ctx.var("who") = bob.node();
    CHECK(ok(ctx.eval("{$who grow 1}")).as_float() == 8.0);
    CHECK(error_of(ctx, "{{+ 1 2} grow 1}").message() ==
          "a command starts with a function or an object, not a int");
}

TEST_CASE("errors name the type, the method, and the call site") {
    Context ctx;
    Person bob;
    ok(ctx.add(bob, "bob"));
    Error e = error_of(ctx, "\n\n{bob fly}");
    CHECK(e.message() == "Person has no method 'fly'");
    CHECK(e.line() == 3);
    CHECK(error_of(ctx, "{bob grow tall}").message() ==
          "argument 1 of 'grow' is symbol, not float32");
    CHECK(error_of(ctx, "{bob set height tall}").message() ==
          "property 'height' of Person is float32, not symbol");
    CHECK(error_of(ctx, "{bob get weight}").message() ==
          "Person has no property or parameter 'weight'");
    CHECK(error_of(ctx, "{bob}").message() ==
          "an object command names a method: {object method ...}");
    CHECK_FALSE(ctx.add(bob, "bob2"));  // already in this Context
    Person other;
    CHECK(ctx.add(other, "bob").error().message() == "an object is already named 'bob'");
    CHECK(ctx.add(other, "if").error().message() ==
          "'if' is a function, so it cannot name an object");
}

TEST_CASE("loose parameters and property paths") {
    Context ctx;
    Person bob;
    ok(ctx.add(bob, "bob"));
    ok(ctx.eval("{bob set mood happy}"));
    CHECK(ok(ctx.eval("{bob get mood}")).as_sym() == Symbol("happy"));
    ok(ctx.eval("{bob set pos ((x 3) (y 4))}"));
    ok(ctx.eval("{class Person (where [pos x]) (feeling [mood])}"));
    CHECK(ok(ctx.eval("{bob where}")).as_int() == 3);
    CHECK(ok(ctx.eval("{bob feeling}")).as_sym() == Symbol("happy"));
    CHECK(error_of(ctx, "{+ [height] 1}").message() ==
          "[...] reads a property of $this, which is not set");
}

TEST_CASE("script classes: new, parameters, and delete") {
    Context ctx;
    ok(
        ctx.eval("{class Goblin\n"
                 "  (hit ($n) {set [hp] {- [hp] $n}})\n"
                 "  (alive {> [hp] 0})}\n"
                 "{new Goblin g1 (hp 5)}"));
    CHECK(ok(ctx.eval("{g1 hit 3}")).as_int() == 2);
    CHECK(ok(ctx.eval("{g1 alive}")).as_int() == 1);
    CHECK(ok(ctx.eval("{g1 hit 2} {g1 alive}")).as_int() == 0);
    CHECK(ok(ctx.eval("{g1 type}")).as_sym() == Symbol("Goblin"));
    CHECK(error_of(ctx, "{g1 fly}").message() == "Goblin has no method 'fly'");
    const Node g = ctx.object("g1");
    ok(ctx.eval("{delete g1}"));
    CHECK(ctx.try_resolve(g) == nullptr);
    CHECK(error_of(ctx, "{new Orc o}").message() == "'Orc' is not a class");
    // A default-constructible bound type can be created from script too.
    ctx.add_type<Person>();
    const Node p = ok(ctx.eval("{new Person tall (height 9)}"));
    REQUIRE(ctx.try_get<Person>(p));
    CHECK(ctx.try_get<Person>(p)->height == 9.0f);
    CHECK(ok(ctx.eval("{tall grow 1}")).as_float() == 10.0);
}  // the Context deletes `tall`, which it created

TEST_CASE("extend adds handlers to a class from elsewhere, replacing only those it names") {
    Context ctx;
    ok(
        ctx.eval("{class Goblin (hit ($n) {set [hp] {- [hp] $n}}) (alive {> [hp] 0})}"
                 "{new Goblin g1 (hp 5)}"));
    // A second file adds one handler and replaces one; the others stay.
    ok(ctx.eval("{extend Goblin (taunt \"come here\") (alive {> [hp] 1})}"));
    CHECK(ok(ctx.eval("{g1 taunt}")).as_str() == "come here");
    CHECK(ok(ctx.eval("{g1 hit 4}")).as_int() == 1);
    CHECK(ok(ctx.eval("{g1 alive}")).as_int() == 0);  // the replaced handler
    // {class} again (a reloaded script) replaces every handler, extensions included: an
    // extension runs after its class.
    ok(ctx.eval("{class Goblin (hit ($n) 0)}"));
    CHECK(error_of(ctx, "{g1 taunt}").message() == "Goblin has no method 'taunt'");
    // A bound type with no class yet gets one; a name that is neither is an error.
    ctx.add_type<Person>();
    Person bob;
    ok(ctx.add(bob, "bob"));
    ok(ctx.eval("{extend Person (greet \"hello\")}"));
    CHECK(ok(ctx.eval("{bob greet}")).as_str() == "hello");
    CHECK(ok(ctx.eval("{bob grow 1}")).as_float() == 7.0);  // native methods still answer
    CHECK(error_of(ctx, "{extend Orc (x 1)}").message() ==
          "'Orc' is not a class: {class Orc ...} it first");
    CHECK(error_of(ctx, "{extend Goblin oops}").message() ==
          "a handler is (method ($params...) body...)");
}

TEST_CASE("check knows the handlers an extension adds") {
    Context ctx;
    const auto tree =
        ok(read_string("{class Goblin (hit ($n) 0)}\n"
                       "{extend Goblin (taunt 0)}\n"
                       "{new Goblin g1}\n"
                       "{g1 taunt}\n"
                       "{g1 dance}\n",
                       "level.dta"));
    const CheckReport report = check(*tree, ctx);
    REQUIRE(report.demand.size() == 1);
    CHECK(to_string(report).find("'dance' on Goblin") != std::string::npos);
}

TEST_CASE("a Context destroyed before its objects leaves them unregistered") {
    Person bob;
    {
        Context ctx;
        ok(ctx.add(bob, "bob"));
        CHECK(bob.context() == &ctx);
    }
    CHECK(bob.context() == nullptr);
    CHECK(bob.node().type() == Type::Int);
    Context again;
    CHECK(again.add(bob, "bob"));  // can join another
}

TEST_CASE("calling a native method allocates nothing") {
    if (!test::counting_allocations()) return;
    Context ctx;
    Person bob;
    ok(ctx.add(bob, "bob"));
    auto tree = ok(read_string("{bob grow 0} {bob get height} {bob set height 6}"));
    for (int i = 0; i < 3; ++i) REQUIRE(ctx.run(tree->nodes()));
    test::AllocationScope scope;
    for (int i = 0; i < 100; ++i) REQUIRE(ctx.run(tree->nodes()));
    CHECK(scope.count() == 0);
}

TEST_CASE("a call from C++ into script allocates nothing once warm") {
    if (!test::counting_allocations()) return;
    Context ctx;
    Person bob;
    ok(ctx.add(bob, "bob"));
    REQUIRE(ctx.eval("{class Person (wave ($n) $n)}"));
    for (int i = 0; i < 3; ++i) REQUIRE(ctx.call(bob, "wave", 1));
    test::AllocationScope scope;
    for (int i = 0; i < 100; ++i) {
        REQUIRE(ctx.call(bob, "wave", i));     // a script handler
        REQUIRE(ctx.call(bob, "grow", 0.0f));  // a native method, the same size
    }
    CHECK(scope.count() == 0);
}

TEST_CASE("a command C++ built and the script kept is never reused") {
    Context ctx;
    Person bob;
    ok(ctx.add(bob, "bob"));
    // {func f () 7}, built by Context::call: the definition keeps the command array.
    const Node args[] = {Node(Array::make(0)), Node(7)};
    REQUIRE(ctx.call(Node(Symbol("func")), "f", args));
    CHECK(ok(ctx.call(bob, "grow", 1.0f)).try_float() == doctest::Approx(7.0));
    CHECK(ok(ctx.call(bob, "set", Symbol("height"), 2.0f)).try_float() == doctest::Approx(2.0));
    CHECK(ok(ctx.eval("{f}")).try_int() == 7);
}

TEST_CASE("check knows the methods of named objects and of objects the tree creates") {
    Context ctx;
    Person bob;
    ok(ctx.add(bob, "bob"));
    ctx.add_type<Person>();
    const auto tree =
        ok(read_string("{class Goblin (hit ($n) 0)}\n"
                       "{new Goblin g1}\n"
                       "{bob grow 1}\n"
                       "{bob grow}\n"
                       "{bob fly}\n"
                       "{g1 hit 1}\n"
                       "{g1 dance}\n"
                       "{bob get height}\n",
                       "level.dta"));
    const CheckReport report = check(*tree, ctx);
    REQUIRE(report.errors.size() == 1);
    CHECK(to_string(report.errors[0]) == "baker: 'grow' takes 1 argument, given 0  (level.dta:4)");
    REQUIRE(report.demand.size() == 2);
    CHECK(to_string(report).find("demand: 'fly' on Person is not defined, called at level.dta:5") !=
          std::string::npos);
    CHECK(
        to_string(report).find("demand: 'dance' on Goblin is not defined, called at level.dta:7") !=
        std::string::npos);
}

TEST_CASE("the reference lists types, their methods and properties, and script classes") {
    Context ctx;
    ctx.add_type<Person>();
    ctx.add_type<Hero>();
    ok(ctx.eval("{class Person (miss whoosh)} {class Goblin (hit ($n) 0)}"));
    const std::string ref = reference(ctx);
    CHECK(ref.find("### `Person`\n\n"
                   "- `{<Person> grow float32}` → float32: Grows by `amount`.\n"
                   "- `{<Person> shrink float32}` → nothing\n"
                   "- property `height`: float32\n"
                   "- `{<Person> miss}` (script)\n") != std::string::npos);
    CHECK(ref.find("### `Hero`\n\nDerives from `Creature`.\n") != std::string::npos);
    CHECK(ref.find("### `Goblin` (script class)\n\n- `{<Goblin> hit $n}` (script)\n") !=
          std::string::npos);
}

struct Robot : Object<Robot> {
    static void bind(Binder<Robot>& b) {
        b.method("walk", [](Robot&, int n) { return n; })
            .doc("Walks `n` steps.")
            .status(Status::Deprecated, "stride");
        b.method("stride", [](Robot&, int n) { return n; }).status(Status::Provisional);
        b.method("fly", [](Robot&) {}).status(Status::Stub);
    }
};

TEST_CASE("the reference shows a method's status as it does a function's") {
    Context ctx;
    ctx.add_type<Robot>();
    const std::string ref = reference(ctx);
    CHECK(ref.find("- `{<Robot> fly}` → nothing *(stub)*\n"
                   "- `{<Robot> stride int32}` → int32 *(provisional)*\n"
                   "- `{<Robot> walk int32}` → int32 *(deprecated: use `stride`)*: Walks `n` "
                   "steps.\n") != std::string::npos);
}

TEST_CASE("an object deleted during a call stays until the host's eval returns") {
    Context ctx;
    ctx.add_type<Person>();
    // During a method's arguments, a native method's and an object's own.
    auto r = ctx.eval("{do {new Person q} {q grow {do {delete q} 1}}}");
    REQUIRE(r);
    CHECK(*r->try_float() == doctest::Approx(7.0));
    CHECK(ctx.object("q").type() != Type::Object);
    REQUIRE(ctx.eval("{new Person q}"));
    (void)ok(ctx.eval("{q set mood {delete q}}"));
    CHECK(ctx.object("q").type() != Type::Object);
    // The host's eval is the outermost call: later commands still reach the object.
    REQUIRE(ctx.eval("{new Person q}"));
    CHECK(*ok(ctx.eval("{delete q} {q grow 1}")).try_float() == doctest::Approx(7.0));
    auto stale = ctx.eval("{q grow 1}");
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code() == ErrorCode::NotFound);
    // C++ removing an object from inside a call waits the same way.
    Person p;
    REQUIRE(ctx.add(p, "p"));
    REQUIRE(ctx.func("drop", [&](Context& c) { c.remove(p); }));
    (void)ok(ctx.eval("{p grow {do {drop} 1}}"));
    CHECK(p.height == doctest::Approx(7.0f));
    CHECK_FALSE(ctx.try_resolve(ctx.object("p")));
}

int g_counted_destroyed = 0;
struct Counted : Object<Counted> {
    ~Counted() override { ++g_counted_destroyed; }
    static void bind(Binder<Counted>&) {}
};

TEST_CASE("the host removing an object {new} made destroys it") {
    Context ctx;
    ctx.add_type<Counted>();
    auto made = ok(ctx.eval("{new Counted c}"));
    ObjectBase* c = ctx.try_resolve(made);
    REQUIRE(c);
    g_counted_destroyed = 0;
    // Outside any call or tick the removal is at once, and the Context owned the object:
    // it is destroyed, as {delete} and a deferred removal destroy it. It once leaked.
    ctx.remove(*c);
    CHECK(g_counted_destroyed == 1);
    CHECK_FALSE(ctx.try_resolve(made));
}

TEST_CASE("objects made while a method's arguments are evaluated do not move its receiver") {
    Context ctx;
    ctx.add_type<Person>();
    Person p;
    REQUIRE(ctx.add(p, "p"));
    std::string text = "{p get {do";
    for (int i = 0; i < 40; ++i) text += " {new Person}";
    text += " nope}}";
    auto r = ctx.eval(text);
    REQUIRE_FALSE(r);
    CHECK(std::string(r.error().message()).find("Person has no property or parameter") !=
          std::string::npos);
}

TEST_CASE("a class redefined or extended by its own running handler") {
    Context ctx;
    Person p;
    REQUIRE(ctx.add(p, "p"));
    ctx.set_profiling(true);  // the handler's counters are written after it returns
    REQUIRE(ctx.eval("{class Person (ping () {class Person (ping () 5)} 7)}"));
    CHECK(*ctx.eval("{p ping}")->try_int() == 7);
    CHECK(*ctx.eval("{p ping}")->try_int() == 5);
    REQUIRE(ctx.eval(
        "{class Person (ping () {extend Person (a () 1) (b () 2) (c () 3) (ping () 6)} 8)}"));
    CHECK(*ctx.eval("{p ping}")->try_int() == 8);
    CHECK(*ctx.eval("{p ping}")->try_int() == 6);
    CHECK(*ctx.eval("{p c}")->try_int() == 3);
}
