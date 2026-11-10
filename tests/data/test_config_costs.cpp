// Config costs: scratch's overlay in one pass, and a change rebinding only the tweaks
// under what it changed.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/tweak.hpp>
#include <baker/write.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "dev_only.hpp"
#include "ok.hpp"

using namespace baker;
using baker::test::ok;

namespace {

// What scratch holds, as the test sets it: each path once, in the order first set.
struct Model {
    std::vector<std::pair<std::string, std::vector<Node>>> entries;

    std::vector<Node>* find(const std::string& path) {
        for (auto& e : entries)
            if (e.first == path) return &e.second;
        return nullptr;
    }
    void put(const std::string& path, std::vector<Node> values) {
        if (std::vector<Node>* v = find(path))
            *v = std::move(values);
        else
            entries.emplace_back(path, std::move(values));
    }
};

// The overlay as scratch built it before the one-pass builder: each entry merged in turn.
Ref<Array> fold(const Model& m) {
    Ref<Array> overlay = Array::make(0);
    for (const auto& [text, values] : m.entries) {
        const Path path(text);
        std::vector<Node> leaf{Node(path[path.size() - 1].name)};
        leaf.insert(leaf.end(), values.begin(), values.end());
        Ref<Array> nest = Array::make_copy(leaf);
        for (size_t i = path.size() - 1; i-- > 0;) nest = array(path[i].name, Node(nest));
        overlay = merge(*overlay, *array(Node(nest)));
    }
    return overlay;
}

std::string text_of(const Array& a) {
    std::string s;
    write_dta(a, string_out(s));
    return s;
}

std::string scratch_text(const Config& cfg) {
    std::string s;
    REQUIRE(cfg.layer_text("scratch", string_out(s)));
    return s;
}

// The scratch layer and the applied root agree with the fold over the base.
void check_against_fold(Config& cfg, const Model& m, const char* base) {
    const Ref<Array> want = fold(m);
    CHECK(scratch_text(cfg) == text_of(*want));
    cfg.apply_edits();
    const Ref<Array> merged = merge(*ok(read_string(base, "base.dta")), *want);
    CHECK(structurally_equal(*cfg.root(), *merged));
}

struct Rng {
    uint64_t s;
    uint32_t operator()(uint32_t n) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>((s >> 33) % n);
    }
};

}  // namespace

BAKER_DEV_TEST_CASE("scratch's one-pass overlay is the entries' merges in turn") {
    static constexpr const char* kBase = "(a (b 1) (k (x 2))) (c 3 4) (d (e (f 5)))";
    static constexpr const char* kNames[] = {"a", "b", "c", "d", "e", "f", "k", "x"};
    for (uint64_t seed = 1; seed <= 4; ++seed) {
        CAPTURE(seed);
        Config cfg({.tweaks = nullptr});
        REQUIRE(cfg.push_layer_text("base.dta", kBase));
        Model m;
        Rng rng{seed};
        for (int step = 0; step < 300; ++step) {
            // A path of one to four segments over few names: values and keys at once (a.b
            // and a.b.c), set in either order.
            std::string path = kNames[rng(8)];
            for (uint32_t depth = rng(4); depth-- > 0;) path += std::string(".") + kNames[rng(8)];
            const uint32_t kind = rng(10);
            std::vector<Node>* known = m.find(path);
            if (kind < 2 && known && !known->empty()) {
                // One element: the whole value with it replaced.
                const uint32_t i = rng(static_cast<uint32_t>(known->size()));
                const Node v(static_cast<int64_t>(step));
                REQUIRE(cfg.set(path + "." + std::to_string(i), v));
                (*known)[i] = v;
                continue;
            }
            std::vector<Node> values;
            for (uint32_t n = kind == 2 ? 0 : 1 + rng(3); n-- > 0;) {
                switch (rng(3)) {
                    case 0: values.emplace_back(static_cast<int64_t>(rng(100))); break;
                    case 1: values.emplace_back(Symbol(kNames[rng(8)])); break;
                    default: values.emplace_back(array(1, 2.5)); break;  // a tuple: a value
                }
            }
            REQUIRE(cfg.set(Path(path), std::span<const Node>(values.data(), values.size())));
            m.put(path, std::move(values));
        }
        check_against_fold(cfg, m, kBase);
        // The fast path: a scalar entry changed in place, then the overlay again.
        for (auto& [path, values] : m.entries)
            if (values.size() == 1) {
                REQUIRE(cfg.set(path, Node(int64_t{-1})));
                values[0] = Node(int64_t{-1});
            }
        check_against_fold(cfg, m, kBase);
    }
}

BAKER_DEV_TEST_CASE("a value that is a key still merges as scratch's fold has it") {
    static constexpr const char* kBase = "(a (b 1) (k (x 2)))";
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base.dta", kBase));
    Model m;
    const auto put = [&](const char* path, std::vector<Node> values) {
        REQUIRE(cfg.set(Path(path), std::span<const Node>(values.data(), values.size())));
        m.put(path, std::move(values));
    };
    put("a.k.x", {Node(int64_t{7})});
    put("a.k", {Node(array(Symbol("x"), 8)), Node(int64_t{9})});  // (x 8) is a key
    put("a.b", {Node(int64_t{3})});
    put("a.k.y", {Node(int64_t{4})});
    check_against_fold(cfg, m, kBase);
}

BAKER_DEV_TEST_CASE("2,000 scratch entries build the fold's overlay") {
    static constexpr const char* kBase = "(g0 (k0 1))";
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base.dta", kBase));
    Model m;
    for (int i = 0; i < 2000; ++i) {
        // 40 groups of 50, visited out of order, some a level deeper.
        const int g = (i * 7) % 40;
        std::string path = "g" + std::to_string(g) + ".k" + std::to_string(i / 40);
        if (i % 9 == 0) path += ".v";
        const Node v(static_cast<int64_t>(i));
        REQUIRE(cfg.set(path, v));
        m.put(path, {v});
    }
    REQUIRE(m.entries.size() == 2000);
    check_against_fold(cfg, m, kBase);
}

// Every value of a tweak's path as text: a tweak of it changes whenever its nodes do.
struct Raw {
    std::string text;
    friend bool operator==(const Raw&, const Raw&) = default;
};
template <>
struct baker::Convert<Raw> {
    static bool from(const Values& v, Raw& out) {
        out.text = text_of(*Array::make_copy(v.nodes()));
        return true;
    }
};

namespace {

// A tweak and how many times it was told of a change.
template <class T>
struct Watched {
    std::string path;
    std::unique_ptr<Tweak<T>> tweak;
    int notified = 0;
    T last{};
    bool was_bound = false;

    Watched(std::string p, T fallback, TweakRegistry& reg) : path(std::move(p)) {
        tweak = std::make_unique<Tweak<T>>(Symbol(path), fallback, TweakMeta{}, reg);
        tweak->on_change([this] { ++notified; });
    }
    void before() {
        notified = 0;
        last = tweak->value();
        was_bound = tweak->bound();
    }
    // A commit told it of a change exactly when its value or its binding changed.
    [[nodiscard]] bool moved() const {
        return !(tweak->value() == last) || tweak->bound() != was_bound;
    }
};

std::string random_overlay(Rng& rng) {
    std::string s;
    for (uint32_t groups = 1 + rng(3); groups-- > 0;) {
        s += "(g" + std::to_string(rng(7));
        for (uint32_t keys = rng(13); keys-- > 0;) {
            s += " (k" + std::to_string(rng(22));
            switch (rng(6)) {
                case 0: s += " #remove"; break;
                case 1: s += " sym"; break;  // not an int: an int tweak warns and keeps -1
                case 2: s += " 1 2"; break;
                default: s += " " + std::to_string(rng(50)); break;
            }
            s += ")";
        }
        s += ")";
    }
    return s;
}

}  // namespace

BAKER_DEV_TEST_CASE("a change rebinds the tweaks under what it changed, as rebinding all") {
    // Duplicate keys, symbols, several values, and groups with no value of their own.
    static constexpr const char* kBase =
        "0 (g0 (k0 1) (k1 2) (k2 3) (k3 4 5)) (g1 (k0 sym) (k1 7) (k1 8)) "
        "(g2 (k0 9) (k5 10)) (g3 (k0 11) (k1 12) (k2 13) (k3 14) (k4 15) (k5 16) (k6 17) (k7 "
        "18) (k8 19) (k9 20) (k10 21) (k11 22) (k2 99))";
    TweakRegistry reg;
    std::vector<std::unique_ptr<Watched<int>>> ints;  // the ones that warn and keep defaults
    std::vector<std::unique_ptr<Watched<Raw>>> raws;  // the ones that move with every node
    Rng rng{7};
    const auto random_path = [&] {
        switch (rng(12)) {
            case 0: return std::string("g") + std::to_string(rng(7));  // a group
            case 1: return std::string("g1..k0");                      // malformed
            case 2: return std::string("0");                           // the root's value
            case 3: return "g" + std::to_string(rng(7)) + ".k" + std::to_string(rng(22)) + ".1";
            default: return "g" + std::to_string(rng(7)) + ".k" + std::to_string(rng(22));
        }
    };
    for (int i = 0; i < 80; ++i) {
        ints.push_back(std::make_unique<Watched<int>>(random_path(), -1, reg));
        raws.push_back(std::make_unique<Watched<Raw>>(random_path(), Raw{"none"}, reg));
    }
    Config cfg({.tweaks = &reg});
    int warnings = 0;
    int commits = 0;
    cfg.set_warning_handler([&](const Error&) { ++warnings; });
    (void)cfg.on_reload_complete([&](const ReloadRecord&) { ++commits; });
    REQUIRE(cfg.push_layer_text("base.dta", kBase));
    REQUIRE(cfg.push_layer_text("over.dta", "(g0 (k0 30))"));
    for (int step = 0; step < 600; ++step) {
        CAPTURE(step);
        for (auto& w : ints) w->before();
        for (auto& w : raws) w->before();
        warnings = 0;
        const int commits_before = commits;
        const uint32_t op = rng(20);
        if (op < 2) {  // most of a wide group at once: its keys are found by their tags
            std::string text = "(g3";
            for (int k = 0; k < 12; ++k)
                text += " (k" + std::to_string(k) + " " + std::to_string(rng(3)) + ")";
            REQUIRE(cfg.set_layer_text("over.dta", text + ")"));
        } else if (op < 9) {
            REQUIRE(cfg.set_layer_text("over.dta", random_overlay(rng)));
        } else if (op < 13) {  // a live edit retunes the tweaks bound to it
            const std::string path = "g" + std::to_string(rng(7)) + ".k" + std::to_string(rng(22));
            REQUIRE(cfg.set(path, Node(static_cast<int64_t>(100 + rng(50)))));
        } else if (op < 15) {
            REQUIRE(cfg.clear_scratch());
        } else if (op < 17) {
            REQUIRE(cfg.set_layer_enabled("over.dta", rng(2) == 0));
        } else if (op < 18) {
            cfg.apply_edits();
        } else {
            // A tweak goes and another comes, bound at once: the next change indexes them all
            // again.
            ints.erase(ints.begin() + rng(static_cast<uint32_t>(ints.size())));
            ints.push_back(std::make_unique<Watched<int>>(random_path(), -1, reg));
            ints.back()->before();
            warnings = 0;  // its own bind's
            REQUIRE(cfg.set_layer_text("over.dta", random_overlay(rng)));
        }
        // A live edit retunes only the tweaks of its path; after a commit, every tweak
        // holds what a full rebind gives: the current value, else its default.
        if (commits == commits_before) continue;
        int loud = 0;
        for (auto& w : ints) {
            CAPTURE(w->path);
            const Path path(w->path);
            const auto want = cfg.get<int>(path);
            CHECK(w->tweak->bound() == want.has_value());
            CHECK(int(*w->tweak) == (want ? *want : -1));
            const bool resolves = cfg.get<Raw>(path).has_value();
            loud += !path.valid() || (resolves && !want) ? 1 : 0;
            // (k 1) to (k 1 2) moves it too, at the same int.
            CHECK(w->notified == (w->moved() ? 1 : std::min(w->notified, 1)));
        }
        for (auto& w : raws) {
            CAPTURE(w->path);
            const Path path(w->path);
            const auto want = cfg.get<Raw>(path);
            CHECK(w->tweak->bound() == want.has_value());
            CHECK(w->tweak->get().text == (want ? want->text : "none"));
            loud += path.valid() ? 0 : 1;
            CHECK(w->notified == (w->moved() ? 1 : 0));
        }
        // A tweak that does not read warns at every commit, as rebinding all of them does.
        CHECK(warnings == loud);
    }
}
