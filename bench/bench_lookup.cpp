// Lookup cost. Target: find_float on a 20-child array <= 25 ns.
//
// Keys are interned Symbols, as a hot loop should hold them; the string-literal row
// shows what interning on every call costs instead. "avg" cycles through all 20 keys,
// "last" always asks for the 20th (the linear scan's worst case).
//
// Cold rows: 50,000 such objects, about 76 MB of tree, far beyond L2, visited
// in a shuffled order, each with a random key; loaded from text and from a binary cache,
// whose layouts differ (the parse allocates children before their parent; the cache lays
// arrays out breadth first).
#include <baker/array.hpp>
#include <baker/cache.hpp>
#include <baker/parse.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace baker::literals;

volatile double g_sink;

// Read through a volatile pointer, so the compiler cannot hoist a lookup with a constant
// key out of the timing loop.
template <class T>
const T& opaque(const T& v) {
    const T* volatile p = &v;
    return *p;
}

template <class F>
double ns_per_call(F&& f) {
    constexpr int kCalls = 2'000'000;
    double best = 1e30;
    for (int rep = 0; rep < 7; ++rep) {
        double sum = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kCalls; ++i) sum += f(i);
        const auto t1 = std::chrono::steady_clock::now();
        g_sink = sum;
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / kCalls);
    }
    return best;
}

void row(const char* name, double ns, double target = 0) {
    if (target > 0)
        std::printf("%-44s %7.2f ns   (target <= %.0f ns: %s)\n", name, ns, target,
                    ns <= target ? "ok" : "MISSED");
    else
        std::printf("%-44s %7.2f ns\n", name, ns);
}

}  // namespace

int main() {
    std::string text = "(obj\n";
    for (int i = 0; i < 20; ++i)
        text += "  (key" + std::to_string(i) + " " + std::to_string(i) + ".5)\n";
    text += ")\n(cfg (renderer (clear_color 0.1 0.2 0.3) (shadows (resolution 2048))))\n";
    auto root = baker::read_string(text, "bench.dta");
    if (!root) return 1;
    const baker::Array& obj = *(*root)->try_find("obj");

    std::vector<baker::Symbol> keys;
    for (int i = 0; i < 20; ++i) keys.emplace_back("key" + std::to_string(i));
    const baker::Symbol last = keys.back();
    const baker::Symbol absent("absent");

    auto sorted_ref = baker::Array::make_copy(obj.nodes());
    sorted_ref->sort_for_lookup();
    const baker::Array& sorted = *sorted_ref;

    const baker::Path path("cfg.renderer.shadows.resolution");
    const baker::Path path2("cfg.renderer.clear_color.1");

    std::printf("lookup on a 20-child array (best of 7, 2M calls each)\n");
    row("try_find (array), avg", ns_per_call([&](int i) {
            return static_cast<double>(
                obj.try_find(opaque(keys[static_cast<size_t>(i % 20)]))->size());
        }));
    row("find_float, avg", ns_per_call([&](int i) {
            return *obj.find_float(opaque(keys[static_cast<size_t>(i % 20)]));
        }),
        25);
    row("find_float, last", ns_per_call([&](int) { return *obj.find_float(opaque(last)); }), 25);
    row("find_float, sorted, avg", ns_per_call([&](int i) {
            return *sorted.find_float(opaque(keys[static_cast<size_t>(i % 20)]));
        }));
    row("find_float, sorted, last",
        ns_per_call([&](int) { return *sorted.find_float(opaque(last)); }));
    row("find<float>, avg", ns_per_call([&](int i) {
            return static_cast<double>(*obj.find<float>(opaque(keys[static_cast<size_t>(i % 20)])));
        }));
    row("find_float(\"key19\") (interns each call)",
        ns_per_call([&](int) { return *obj.find_float("key19"); }));
    row("find_float(\"key19\"_sym)",
        ns_per_call([&](int) { return *obj.find_float("key19"_sym); }));
    row("get<int>(Path), 4 segments",
        ns_per_call([&](int) { return static_cast<double>(*(*root)->get<int>(opaque(path))); }));
    row("get<float>(Path), 4 segments with index",
        ns_per_call([&](int) { return static_cast<double>(*(*root)->get<float>(opaque(path2))); }));
    row("get<int>(\"...\") (parses each call)", ns_per_call([&](int) {
            return static_cast<double>(*(*root)->get<int>("cfg.renderer.shadows.resolution"));
        }));
    // Where sorted mode starts to pay: a linear scan grows with the array, binary search
    // barely does.
    for (const int n : {50, 200, 1000}) {
        std::vector<baker::Node> big;
        std::vector<baker::Symbol> big_keys;
        for (int i = 0; i < n; ++i) {
            big_keys.emplace_back("big" + std::to_string(i));
            big.emplace_back(baker::array(big_keys.back(), i));
        }
        auto linear = baker::Array::make_from(big);
        auto sorted_big = baker::Array::make_copy(linear->nodes());
        sorted_big->sort_for_lookup();
        const auto un = static_cast<unsigned>(n);
        const double l = ns_per_call([&](int i) {
            return static_cast<double>(
                linear->try_find(opaque(big_keys[static_cast<unsigned>(i) % un]))->size());
        });
        const double s = ns_per_call([&](int i) {
            return static_cast<double>(
                sorted_big->try_find(opaque(big_keys[static_cast<unsigned>(i) % un]))->size());
        });
        std::printf("try_find, %4d children, avg: linear %7.2f ns, sorted %6.2f ns\n", n, l, s);
    }

    // Cold: a working set larger than L2.
    constexpr int kObjects = 50'000;
    std::string many;
    for (int o = 0; o < kObjects; ++o) {
        many += "(o" + std::to_string(o);
        for (int i = 0; i < 20; ++i)
            many += " (key" + std::to_string(i) + " " + std::to_string(i) + ".5)";
        many += ")\n";
    }
    auto text_tree = baker::read_string(many, "cold.dta");
    if (!text_tree) return 1;
    std::string bytes;
    if (!baker::write_cache(**text_tree, baker::TextSink{[](void* u, std::string_view t) {
                                                             static_cast<std::string*>(u)->append(
                                                                 t);
                                                         },
                                                         &bytes}))
        return 1;
    auto cache_tree = baker::read_cache_bytes(bytes);
    if (!cache_tree) return 1;
    std::mt19937 rng(42);
    std::vector<uint32_t> order(kObjects);
    for (uint32_t i = 0; i < kObjects; ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);
    std::vector<uint8_t> which(1 << 16);
    for (uint8_t& k : which) k = static_cast<uint8_t>(rng() % 20);
    std::printf("cold: %d objects of 20 keys, %.1f MB of tree, shuffled\n", kObjects,
                static_cast<double>(bytes.size()) / 1e6);
    for (const auto* tree : {&*text_tree, &*cache_tree}) {
        std::vector<const baker::Array*> objs;
        for (const baker::Node& n : **tree) objs.push_back(n.array());
        const double ns = ns_per_call([&](int i) {
            const auto u = static_cast<unsigned>(i);
            const baker::Array& o = *objs[order[u % kObjects]];
            return *o.find_float(keys[which[u & 0xFFFF]]);
        });
        row(tree == &*text_tree ? "find_float, cold, text-loaded"
                                : "find_float, cold, cache-loaded",
            ns);
    }
    return 0;
}
