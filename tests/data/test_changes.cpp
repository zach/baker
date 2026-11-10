// The change log: transactions of change records beside the journal, opt-in; step
// 1 records live edits; step 2 a commit's records.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>

#include "alloc_counter.hpp"
#include "ok.hpp"
#include "temp_dir.hpp"

#include <string>
#include <vector>

using namespace baker;
using baker::test::ok;

namespace {

std::string text(const Array& a) {
    return to_string(a);
}

std::vector<Transaction> all(const Config& cfg) {
    std::vector<Transaction> out;
    cfg.transactions([&](const Transaction& t) { out.push_back(t); });
    return out;
}

}  // namespace

TEST_CASE("the change log is off until asked for, and then records each live edit whole") {
    if constexpr (kShipping) return;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base", "(player (speed 3.5) (tint 1 2 3 4 5 6))"));
    REQUIRE(cfg.set("player.speed", Node(4.0)));
    CHECK(cfg.transaction_count() == 0);
    CHECK(cfg.change_history() == 0);

    cfg.set_change_history(16);
    REQUIRE(cfg.set("player.speed", Node(5.0)));  // the fast path: speed is in scratch
    REQUIRE(cfg.set(Path("player.tint"), std::vector<Node>{Node(9), Node(8), Node(7), Node(6),
                                                           Node(5), Node(4), Node(3)}));
    REQUIRE(cfg.set("player.lives", Node(int64_t{3})));  // a path the stack lacks
    const auto ts = all(cfg);
    REQUIRE(ts.size() == 3);
    CHECK(ts[0].id == 1);
    CHECK(ts[0].name == Symbol("set"));
    REQUIRE(ts[0].records.size() == 1);
    const ChangeRecord& speed = ts[0].records[0];
    CHECK(speed.kind == ChangeKind::Set);
    CHECK(speed.path == Symbol("player.speed"));
    CHECK(text(*speed.before) == "(4.0)");
    CHECK(text(*speed.after) == "(5.0)");
    CHECK(speed.layer == Symbol("scratch"));
    CHECK(speed.source.layer == Symbol("scratch"));  // the old value was scratch's own
    // The whole value, past the journal's four nodes.
    const ChangeRecord& tint = ts[1].records[0];
    CHECK(text(*tint.before) == "(1 2 3 4 5 6)");
    CHECK(text(*tint.after) == "(9 8 7 6 5 4 3)");
    CHECK(tint.source.layer == Symbol("base"));
    // A value the stack lacked: no before.
    const ChangeRecord& lives = ts[2].records[0];
    CHECK_FALSE(lives.before);
    CHECK(text(*lives.after) == "(3)");
    CHECK(ts[2].time_ns >= ts[0].time_ns);
    CHECK(ts[2].id == 3);
}

TEST_CASE("a transaction groups the changes until its end, and nests") {
    if constexpr (kShipping) return;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base", "(a 1) (b 2) (c 3)"));
    cfg.set_change_history(8);
    cfg.begin_transaction("drag");
    REQUIRE(cfg.set("a", Node(10)));
    cfg.begin_transaction("inner");
    REQUIRE(cfg.set("b", Node(20)));
    cfg.end_transaction();
    REQUIRE(cfg.set("c", Node(30)));
    CHECK(cfg.transaction_count() == 0);  // still open
    cfg.end_transaction();
    const auto ts = all(cfg);
    REQUIRE(ts.size() == 1);
    CHECK(ts[0].name == Symbol("drag"));
    REQUIRE(ts[0].records.size() == 3);
    CHECK(ts[0].records[1].path == Symbol("b"));
    // An empty transaction is not kept; an unbalanced end is harmless.
    cfg.begin_transaction("nothing");
    cfg.end_transaction();
    cfg.end_transaction();
    CHECK(cfg.transaction_count() == 1);
}

TEST_CASE("the log is a ring of the last transactions; clear_scratch records what it drops") {
    if constexpr (kShipping) return;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base", "(a 1)"));
    cfg.set_change_history(3);
    for (int i = 0; i < 5; ++i) REQUIRE(cfg.set("a", Node(int64_t{i + 2})));
    auto ts = all(cfg);
    REQUIRE(ts.size() == 3);
    CHECK(ts[0].id == 3);
    CHECK(text(*ts[0].records[0].after) == "(4)");
    CHECK(ts[2].id == 5);
    CHECK(text(*ts[2].records[0].after) == "(6)");
    // clear_scratch: the edit's value gives way to the file's.
    REQUIRE(cfg.clear_scratch("a"));
    ts = all(cfg);
    REQUIRE(ts.size() == 3);
    const Transaction& cleared = ts[2];
    CHECK(cleared.name == Symbol("clear_scratch"));
    REQUIRE(cleared.records.size() == 1);
    CHECK(text(*cleared.records[0].before) == "(6)");
    CHECK(text(*cleared.records[0].after) == "(1)");
    CHECK(cleared.records[0].source.layer == Symbol("scratch"));
    CHECK(cleared.records[0].layer == Symbol("base"));
    CHECK(ok(cfg.get<int>("a")) == 1);
    // Turning the log off clears it; set_change_history(0) keeps the fast path clean.
    cfg.set_change_history(0);
    CHECK(cfg.transaction_count() == 0);
    REQUIRE(cfg.set("a", Node(int64_t{7})));
    CHECK(cfg.transaction_count() == 0);
}

TEST_CASE("with the log off a scratch set allocates nothing; with it on, it keeps its trees") {
    if constexpr (kShipping) return;
    if (!test::counting_allocations()) return;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base", "(a 1)"));
    REQUIRE(cfg.set("a", Node(2)));
    size_t off = 0;
    {
        const test::AllocationScope scope;
        for (int i = 0; i < 100; ++i) (void)cfg.set("a", Node(i));
        off = scope.count();
    }
    CHECK(off == 0);
    cfg.set_change_history(8);
    for (int i = 0; i < 10; ++i) (void)cfg.set("a", Node(i));
    CHECK(cfg.transaction_count() == 8);  // the allocation gate counts the log's allocator
}

// Step 2: a commit's records from the walk of the two merged roots.
namespace {

std::vector<ChangeRecord> records_of(const Transaction& t) {
    return std::vector<ChangeRecord>(t.records.begin(), t.records.end());
}

const ChangeRecord* find(const std::vector<ChangeRecord>& rs, ChangeKind kind, const char* path) {
    for (const ChangeRecord& r : rs)
        if (r.kind == kind && r.path == Symbol(path)) return &r;
    return nullptr;
}

}  // namespace

TEST_CASE("a layer pushed or removed records what it added, removed, set and reordered") {
    if constexpr (kShipping) return;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base",
                                "(player (speed 3) (lives 2) (name \"p\")) (enemies "
                                "(grunt (hp 10)) (boss (hp 99)))"));
    cfg.set_change_history(8);
    REQUIRE(cfg.push_layer_text("patch",
                                "(player (speed 4) (armor 1) (lives #remove)) "
                                "(enemies (boss (hp 100)) (grunt (hp 11)))"));
    auto ts = all(cfg);
    REQUIRE(ts.size() == 1);
    CHECK(ts[0].name == Symbol("layer"));
    auto rs = records_of(ts[0]);
    const ChangeRecord* speed = find(rs, ChangeKind::Set, "player.speed");
    REQUIRE(speed);
    CHECK(text(*speed->before) == "(3)");
    CHECK(text(*speed->after) == "(4)");
    CHECK(speed->layer == Symbol("patch"));
    const ChangeRecord* armor = find(rs, ChangeKind::Add, "player.armor");
    REQUIRE(armor);
    CHECK_FALSE(armor->before);
    CHECK(text(*armor->after) == "(armor 1)");
    CHECK(armor->layer == Symbol("patch"));
    const ChangeRecord* lives = find(rs, ChangeKind::Remove, "player.lives");
    REQUIRE(lives);
    CHECK(text(*lives->before) == "(lives 2)");
    CHECK_FALSE(lives->after);
    CHECK(find(rs, ChangeKind::Set, "enemies.boss.hp"));
    CHECK(find(rs, ChangeKind::Set, "enemies.grunt.hp"));
    CHECK_FALSE(find(rs, ChangeKind::Set, "player.name"));  // shared: not walked
    CHECK_FALSE(find(rs, ChangeKind::Reorder, "enemies"));  // the merge keeps the lower order
    // Removing the layer reverses each record.
    REQUIRE(cfg.remove_layer("patch"));
    ts = all(cfg);
    REQUIRE(ts.size() == 2);
    rs = records_of(ts[1]);
    CHECK(find(rs, ChangeKind::Add, "player.lives"));
    CHECK(find(rs, ChangeKind::Remove, "player.armor"));
    CHECK(text(*find(rs, ChangeKind::Set, "player.speed")->after) == "(3)");
}

TEST_CASE("a commit records renames by ID or alias, reorders, and list verbs") {
    if constexpr (kShipping) return;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base",
                                "(items (sword (@id a0000000000000000000000000000001) "
                                "(dmg 5)) (shield (hp 3)) (bow (dmg 2)) (list 1 2 3))"));
    cfg.set_change_history(8);
    // The layer's text again with a key renamed, another aliased, and the order changed.
    REQUIRE(cfg.set_layer_text("base",
                               "(items (bow (dmg 2)) (blade (@id "
                               "a0000000000000000000000000000001) (dmg 5)) "
                               "(buckler (@aka shield) (hp 3)) (list 1 2 3))"));
    auto rs = records_of(all(cfg).back());
    const ChangeRecord* blade = find(rs, ChangeKind::Rename, "items.blade");
    REQUIRE(blade);
    CHECK(blade->renamed_from == Symbol("sword"));
    CHECK(text(*blade->before) == "(sword (@id a0000000000000000000000000000001) (dmg 5))");
    CHECK(!blade->id.empty());
    const ChangeRecord* buckler = find(rs, ChangeKind::Rename, "items.buckler");
    REQUIRE(buckler);
    CHECK(buckler->renamed_from == Symbol("shield"));
    CHECK_FALSE(find(rs, ChangeKind::Remove, "items.sword"));
    CHECK_FALSE(find(rs, ChangeKind::Remove, "items.shield"));
    CHECK_FALSE(find(rs, ChangeKind::Add, "items.blade"));
    CHECK(find(rs, ChangeKind::Reorder, "items"));
    // #append and #drop in a layer: the verb with its operand.
    REQUIRE(cfg.push_layer_text("more", "(items (list #append 4 5))"));
    rs = records_of(all(cfg).back());
    const ChangeRecord* app = find(rs, ChangeKind::Append, "items.list");
    REQUIRE(app);
    CHECK(text(*app->before) == "(1 2 3)");
    CHECK(text(*app->after) == "(4 5)");
    CHECK(app->layer == Symbol("more"));
    REQUIRE(cfg.push_layer_text("less", "(items (list #drop 2))"));
    rs = records_of(all(cfg).back());
    const ChangeRecord* drop = find(rs, ChangeKind::Drop, "items.list");
    REQUIRE(drop);
    CHECK(text(*drop->before) == "(1 2 3 4 5)");
    CHECK(text(*drop->after) == "(2)");
}

TEST_CASE("a promotion and a write-back are one transaction each, with the lines written") {
    if constexpr (kShipping) return;
    const test::TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("base", dir.write("base.dta", "(player (speed 3) (lives 2))\n")));
    cfg.set_change_history(8);
    REQUIRE(cfg.set("player.speed", Node(4)));
    REQUIRE(cfg.set("player.lives", Node(5)));
    const WriteReport w = ok(cfg.write_back("player.lives"));
    REQUIRE(w.written.size() == 1);
    auto ts = all(cfg);
    REQUIRE(ts.size() == 3);
    CHECK(ts[2].name == Symbol("write_back"));
    REQUIRE(ts[2].written.size() == 1);
    CHECK(ts[2].written[0].line == w.written[0].line);
    CHECK(ts[2].written[0].file == w.written[0].file);
    // The stack reads the same value from the file now; the record is the layer's own.
    REQUIRE(ts[2].records.size() == 1);
    CHECK(ts[2].records[0].kind == ChangeKind::Set);
    CHECK(ts[2].records[0].path == Symbol("player.lives"));
    CHECK(text(*ts[2].records[0].before) == "(2)");
    CHECK(text(*ts[2].records[0].after) == "(5)");
    CHECK(ts[2].records[0].layer == Symbol("base"));
    CHECK(ts[2].records[0].source.layer == Symbol("base"));
    REQUIRE(cfg.promote("scratch", "base"));
    ts = all(cfg);
    REQUIRE(ts.size() == 4);
    CHECK(ts[3].name == Symbol("promote"));
    REQUIRE(ts[3].written.size() == 1);
    CHECK(ts[3].written[0].file == Symbol(dir.path("base.dta")));
    REQUIRE(ts[3].records.size() == 1);
    CHECK(ts[3].records[0].path == Symbol("player.speed"));
    CHECK(text(*ts[3].records[0].before) == "(3)");
    CHECK(text(*ts[3].records[0].after) == "(4)");
    CHECK(ts[3].records[0].layer == Symbol("base"));
    CHECK(ok(cfg.get<int>("player.speed")) == 4);
}

// Step 3: undo and redo per transaction.
TEST_CASE("undo returns to before the latest transaction, redo applies it again") {
    if constexpr (kShipping) return;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base", "(player (speed 3) (lives 2))"));
    CHECK(cfg.undo().error().code() == ErrorCode::Refused);  // the log is off
    cfg.set_change_history(8);
    CHECK(cfg.undo().error().code() == ErrorCode::NotFound);  // nothing yet
    REQUIRE(cfg.set("player.speed", Node(4)));
    cfg.begin_transaction("drag");
    REQUIRE(cfg.set("player.speed", Node(5)));
    REQUIRE(cfg.set("player.lives", Node(9)));
    cfg.end_transaction();
    REQUIRE(cfg.push_layer_text("patch", "(player (speed 7) (armor 1))"));
    CHECK(ok(cfg.get<int>("player.speed")) == 5);  // scratch over the patch
    CHECK(ok(cfg.get<int>("player.armor")) == 1);
    REQUIRE(cfg.transaction_count() == 3);

    REQUIRE(cfg.undo());  // the push
    CHECK_FALSE(cfg.get<int>("player.armor"));
    CHECK(ok(cfg.get<int>("player.speed")) == 5);
    REQUIRE(cfg.undo());  // the drag, as one step
    CHECK(ok(cfg.get<int>("player.speed")) == 4);
    CHECK(ok(cfg.get<int>("player.lives")) == 2);
    CHECK(cfg.transaction_count() == 3);  // kept, marked
    CHECK(cfg.transaction_at(1).undone);
    CHECK(cfg.transaction_at(2).undone);
    CHECK_FALSE(cfg.transaction_at(0).undone);
    REQUIRE(cfg.redo());  // the drag again
    CHECK(ok(cfg.get<int>("player.speed")) == 5);
    CHECK(ok(cfg.get<int>("player.lives")) == 9);
    CHECK_FALSE(cfg.transaction_at(1).undone);
    REQUIRE(cfg.undo());
    REQUIRE(cfg.undo());  // the first set
    CHECK(ok(cfg.get<int>("player.speed")) == 3);
    CHECK(cfg.undo().error().code() == ErrorCode::NotFound);
    CHECK(cfg.unsaved().empty());
    // A new change drops the undone chain.
    REQUIRE(cfg.set("player.speed", Node(11)));
    CHECK(cfg.transaction_count() == 1);
    CHECK(cfg.transaction_at(0).name == Symbol("set"));
    CHECK(cfg.redo().error().code() == ErrorCode::NotFound);
    REQUIRE(cfg.undo());
    CHECK(ok(cfg.get<int>("player.speed")) == 3);
    REQUIRE(cfg.redo());
    CHECK(ok(cfg.get<int>("player.speed")) == 11);
    // The ring keeps the last n, and undo reaches no further.
    for (int i = 0; i < 10; ++i) REQUIRE(cfg.set("player.speed", Node(20 + i)));
    CHECK(cfg.transaction_count() == 8);
    for (int i = 0; i < 8; ++i) REQUIRE(cfg.undo());
    CHECK(ok(cfg.get<int>("player.speed")) == 21);  // before the oldest kept
    CHECK(cfg.undo().error().code() == ErrorCode::NotFound);
}

TEST_CASE("undoing a promotion returns the data in memory; the file stays written") {
    if constexpr (kShipping) return;
    const test::TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("base", dir.write("base.dta", "(player (speed 3))\n")));
    cfg.set_change_history(8);
    REQUIRE(cfg.set("player.speed", Node(4)));
    REQUIRE(cfg.promote("scratch", "base"));
    CHECK(dir.read("base.dta") == "(player (speed 4))\n");
    CHECK(ok(cfg.layer_root("base")->get<int>("player.speed")) == 4);
    REQUIRE(cfg.undo());  // the promotion: the old tree is back under scratch's edit
    CHECK(ok(cfg.get<int>("player.speed")) == 4);
    CHECK(ok(cfg.layer_root("base")->get<int>("player.speed")) == 3);
    REQUIRE(cfg.undo());  // the edit
    CHECK(ok(cfg.get<int>("player.speed")) == 3);
    CHECK(dir.read("base.dta") == "(player (speed 4))\n");  // as written; named by the log
    CHECK(cfg.transaction_at(1).written.size() == 1);
    CHECK(cfg.transaction_at(1).undone);
    // Nothing of the undo itself is recorded.
    CHECK(cfg.transaction_count() == 2);
    REQUIRE(cfg.redo());
    REQUIRE(cfg.redo());
    CHECK(ok(cfg.get<int>("player.speed")) == 4);
    CHECK(ok(cfg.layer_root("base")->get<int>("player.speed")) == 4);
    // A reload reads the file as written.
    REQUIRE(cfg.undo());
    REQUIRE(cfg.undo());
    REQUIRE(cfg.reload());
    CHECK(ok(cfg.get<int>("player.speed")) == 4);
}
