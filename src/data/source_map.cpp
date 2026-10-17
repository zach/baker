#include <baker/source_map.hpp>

#include "common/containers.hpp"

namespace baker {

// Flat: every array's spans lie in one run, and an open-addressed index finds an
// array's entry by address. Recording an array costs no allocation of its own; the run and
// the tables grow by doubling.
struct SourceMap::Tables {
    explicit Tables(const Allocator& a)
        : alloc(a), entries(alloc), run(alloc), keys(alloc), slots(alloc), texts(alloc) {}
    struct Entry {
        Ref<Array> keep;  // keeps the address from being reused while it is a key
        SourceSpan extent;
        uint32_t first = 0;  // its spans: run[first, first + count)
        uint32_t count = 0;
    };
    const Allocator alloc;
    Vec<Entry> entries;
    Vec<SourceSpan> run;
    Vec<const Array*> keys;  // power-of-two sized; null: free
    Vec<uint32_t> slots;     // the entry each key's array has
    HashMap<Symbol, Str> texts;

    [[nodiscard]] size_t probe(const Array* a) const noexcept {
        const size_t mask = keys.size() - 1;
        size_t i = static_cast<size_t>(
                       (reinterpret_cast<uintptr_t>(a) >> 4) * 0x9E3779B97F4A7C15ull >> 32) &
                   mask;
        while (keys[i] && keys[i] != a) i = (i + 1) & mask;
        return i;
    }
    [[nodiscard]] const Entry* find(const Array& a) const noexcept {
        if (keys.empty()) return nullptr;
        const size_t i = probe(&a);
        return keys[i] ? &entries[slots[i]] : nullptr;
    }
    void grow() {
        Vec<const Array*> old_keys(keys);
        Vec<uint32_t> old_slots(slots);
        keys.assign(keys.empty() ? 1024 : keys.size() * 2, nullptr);
        slots.resize(keys.size());
        for (size_t i = 0; i < old_keys.size(); ++i)
            if (old_keys[i]) {
                const size_t j = probe(old_keys[i]);
                keys[j] = old_keys[i];
                slots[j] = old_slots[i];
            }
    }
};

// From the default allocator unless an owner passes its own: a host-made map has no owner,
// and snapshots share a Config's.
SourceMap::SourceMap() : SourceMap(default_allocator()) {}
SourceMap::SourceMap(const Allocator& alloc) : t_(detail::create<Tables>(alloc, alloc)) {}
SourceMap::~SourceMap() {
    const Allocator alloc = t_->alloc;
    detail::destroy(alloc, t_);
}

std::optional<std::span<const SourceSpan>> SourceMap::spans(const Array& a) const {
    const Tables::Entry* e = t_->find(a);
    if (!e || e->count != a.size()) return std::nullopt;
    return std::span<const SourceSpan>(t_->run.data() + e->first, e->count);
}

const SourceSpan* SourceMap::span(const Array& a, size_t i) const {
    const auto s = spans(a);
    return s && i < s->size() ? &(*s)[i] : nullptr;
}

const SourceSpan* SourceMap::extent(const Array& a) const {
    const Tables::Entry* e = t_->find(a);
    return e ? &e->extent : nullptr;
}

std::optional<std::string_view> SourceMap::text(Symbol file) const {
    auto it = t_->texts.find(file);
    if (it == t_->texts.end()) return std::nullopt;
    return std::string_view(it->second);
}

bool SourceMap::empty() const noexcept {
    return t_->entries.empty();
}

void SourceMap::record(const Ref<Array>& a, const SourceSpan& extent,
                       std::span<const SourceSpan> spans) {
    Tables& t = *t_;
    if ((t.entries.size() + 1) * 2 > t.keys.size()) t.grow();
    const auto first = static_cast<uint32_t>(t.run.size());
    t.run.insert(t.run.end(), spans.data(), spans.data() + spans.size());
    const size_t i = t.probe(a.get());
    Tables::Entry e{a, extent, first, static_cast<uint32_t>(spans.size())};
    if (t.keys[i]) {
        t.entries[t.slots[i]] = std::move(e);  // recorded again (a #merge rebuild): replaced
        return;
    }
    t.keys[i] = a.get();
    t.slots[i] = static_cast<uint32_t>(t.entries.size());
    t.entries.push_back(std::move(e));
}

void SourceMap::record_text(Symbol file, std::string_view text) {
    Tables& t = *t_;
    // A map read again keeps the text as it is now.
    if (auto [it, fresh] = t.texts.emplace(file, Str(text, t.alloc)); !fresh) {
        it->second.assign(text);
        return;
    }
    // The file is read next: room for its spans and arrays up front, at config's typical
    // density (a node every 4 to 5 bytes, an array every 14), so the run is not regrown.
    t.run.reserve(t.run.size() + text.size() / 4);
    t.entries.reserve(t.entries.size() + text.size() / 14);
    while ((t.entries.capacity() + 1) * 2 > t.keys.size()) t.grow();
}

}  // namespace baker
