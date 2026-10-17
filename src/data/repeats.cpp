// The repetition report (repeats.hpp).

#include <baker/repeats.hpp>

#include "common/containers.hpp"
#include "session.hpp"
#include "text.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace baker {
namespace {

uint64_t mix(uint64_t h, uint64_t v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

// A scalar's hash: its type and value. Arrays are hashed by what they hold (below).
uint64_t scalar_hash(const Node& n) noexcept {
    const uint64_t t = static_cast<uint64_t>(n.type());
    if (const auto s = n.try_str(); s && n.type() == Type::String)
        return mix(t, detail::content_hash(*s));
    if (n.type() == Type::Int) return mix(t, static_cast<uint64_t>(*n.try_int()));
    if (n.type() == Type::Float) return mix(t, std::bit_cast<uint64_t>(*n.try_float()));
    if (const auto s = n.try_sym()) return mix(t, reinterpret_cast<uintptr_t>(s->id()));
    if (n.type() == Type::Var) return mix(t, reinterpret_cast<uintptr_t>(n.var_name().id()));
    return t;  // functions and objects: structurally_equal decides
}

struct Finder {
    struct Info {
        uint64_t hash;
        uint32_t nodes;
    };
    struct Occurrence {
        const Array* array;
        Type kind;
        const Array* parent;
    };
    HashMap<const Array*, Info> memo;
    HashMap<const Array*, bool> seen;
    Vec<Occurrence> occurrences;  // each array once, at the first place it is seen

    // The hash and recursive size of `a`'s contents, once per array.
    Info info(const Array& a) {
        if (const auto it = memo.find(&a); it != memo.end()) return it->second;
        uint64_t h = 0x51ed2701c5f04b9bull;
        uint32_t nodes = 0;
        for (const Node& n : a.nodes()) {
            ++nodes;
            if (const Array* c = n.array()) {
                const Info ci = info(*c);
                nodes += ci.nodes;
                h = mix(h, mix(static_cast<uint64_t>(n.type()), ci.hash));
            } else {
                h = mix(h, scalar_hash(n));
            }
        }
        const Info r{h, nodes};
        memo.emplace(&a, r);
        return r;
    }

    void walk(const Array& parent) {
        for (const Node& n : parent.nodes()) {
            const Array* c = n.array();
            if (!c || !seen.emplace(c, true).second) continue;  // a shared array counts once
            info(*c);
            occurrences.push_back({c, n.type(), &parent});
            walk(*c);
        }
    }
};

bool site_less(const RepeatSite& a, const RepeatSite& b) noexcept {
    const int c = a.file.str().compare(b.file.str());
    return c != 0 ? c < 0 : a.line < b.line;
}

}  // namespace

List<Repeat> find_repeats(std::span<const Array* const> trees, const RepeatOptions& options) {
    if (options.min_repeats == 0) return {};
    Finder f;
    for (const Array* t : trees)
        if (t) f.walk(*t);

    // Group the large enough by hash and kind, then split each group into classes of
    // structurally equal subtrees.
    HashMap<uint64_t, Vec<uint32_t>> groups;
    for (uint32_t i = 0; i < f.occurrences.size(); ++i) {
        const Finder::Occurrence& o = f.occurrences[i];
        const Finder::Info in = f.memo.find(o.array)->second;
        if (in.nodes >= options.min_nodes)
            groups[mix(in.hash, static_cast<uint64_t>(o.kind))].push_back(i);
    }
    struct Class {
        Vec<uint32_t> members;
        uint32_t nodes;
    };
    Vec<Class> classes;
    for (auto& [hash, members] : groups) {
        if (members.size() < options.min_repeats) continue;
        Vec<Class> split;
        for (const uint32_t i : members) {
            const Finder::Occurrence& o = f.occurrences[i];
            auto same = std::find_if(split.begin(), split.end(), [&](const Class& c) {
                const Finder::Occurrence& r = f.occurrences[c.members[0]];
                return r.kind == o.kind && structurally_equal(*r.array, *o.array);
            });
            if (same == split.end())
                split.push_back({{i}, f.memo.find(o.array)->second.nodes});
            else
                same->members.push_back(i);
        }
        for (Class& c : split)
            if (c.members.size() >= options.min_repeats) classes.push_back(std::move(c));
    }

    // Largest first: a class whose every occurrence lies inside one already taken is the
    // same repetition seen again, and is not reported.
    std::sort(classes.begin(), classes.end(),
              [](const Class& a, const Class& b) { return a.nodes > b.nodes; });
    HashMap<const Array*, bool> covered;
    Vec<Repeat> out;
    for (const Class& c : classes) {
        const bool inside = std::all_of(c.members.begin(), c.members.end(), [&](uint32_t i) {
            return covered.count(f.occurrences[i].parent) != 0;
        });
        for (const uint32_t i : c.members) covered[f.occurrences[i].array] = true;
        if (inside) continue;
        Vec<RepeatSite> sites;
        for (const uint32_t i : c.members) {
            const Array* a = f.occurrences[i].array;
            sites.push_back({a->file(), a->line()});
        }
        std::sort(sites.begin(), sites.end(), site_less);
        const Finder::Occurrence& first = f.occurrences[c.members[0]];
        out.push_back({Node(Ref<Array>(const_cast<Array*>(first.array)), first.kind), c.nodes,
                       List<RepeatSite>(std::span<const RepeatSite>(sites.data(), sites.size()))});
    }
    // An order that depends only on the data: first site, then size, then text.
    std::sort(out.begin(), out.end(), [](const Repeat& a, const Repeat& b) {
        if (site_less(a.sites[0], b.sites[0])) return true;
        if (site_less(b.sites[0], a.sites[0])) return false;
        if (a.nodes != b.nodes) return a.nodes > b.nodes;
        return detail::text_of(a.form) < detail::text_of(b.form);
    });
    return List<Repeat>(std::span<const Repeat>(out.data(), out.size()));
}

List<Repeat> find_repeats(const Array& tree, const RepeatOptions& options) {
    const Array* trees[1] = {&tree};
    return find_repeats(std::span<const Array* const>(trees, 1), options);
}

}  // namespace baker
