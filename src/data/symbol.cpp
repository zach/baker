#include <baker/allocator.hpp>
#include <baker/symbol.hpp>

#include <atomic>
#include <cstring>
#include <mutex>

namespace baker {
namespace {

// Append-only arena + open-addressed hash table of entry pointers, from the default
// allocator. Never destroyed: symbols are immortal, and leaking the table keeps them valid
// during static destruction too.
//
// Reads take no lock. Entries are immutable once published, slots only ever go from
// empty to an entry, and a grown table replaces the old one whole, so a reader probes the
// table it loaded and either finds its symbol or reaches an empty slot. Only then does it
// take the mutex, probe the current table again, and insert. A grown table keeps the one
// it replaced: a reader may still be probing it, and geometric growth bounds the total.
class SymbolTable {
public:
    const detail::SymbolEntry* intern(std::string_view text) {
        const uint64_t hash = detail::hash_symbol_text(text);
        if (const auto* e = find(*table_.load(std::memory_order_acquire), text, hash)) return e;
        std::lock_guard lock(mutex_);
        Table* t = table_.load(std::memory_order_relaxed);
        if (const auto* e = find(*t, text, hash)) return e;  // interned since the probe
        if ((count_ + 1) * 2 > t->capacity) t = grow(*t);
        const detail::SymbolEntry* e = make_entry(text, hash);
        insert(*t, e);
        ++count_;
        return e;
    }

    size_t size() {
        std::lock_guard lock(mutex_);
        return count_;
    }

private:
    static constexpr size_t kBlockSize = size_t{64} * 1024;

    using Slot = std::atomic<const detail::SymbolEntry*>;
    struct Table {
        Slot* slots;
        size_t capacity;
        Table* older;  // the table this one replaced, kept: a reader may still be probing it
    };

    static Table* new_table(size_t n) {
        auto* t = static_cast<Table*>(detail::default_alloc(sizeof(Table), alignof(Table)));
        auto* s = static_cast<Slot*>(detail::default_alloc(n * sizeof(Slot), alignof(Slot)));
        for (size_t i = 0; i < n; ++i) new (s + i) Slot(nullptr);
        t->slots = s;
        t->capacity = n;
        t->older = nullptr;
        return t;
    }

    // The entry for `text` in `t`, or null at the first empty slot of its probe sequence.
    static const detail::SymbolEntry* find(const Table& t, std::string_view text,
                                           uint64_t hash) noexcept {
        const size_t mask = t.capacity - 1;
        for (size_t i = static_cast<size_t>(hash) & mask;; i = (i + 1) & mask) {
            const detail::SymbolEntry* e = t.slots[i].load(std::memory_order_acquire);
            if (!e) return nullptr;
            if (e->hash == hash && e->size == text.size() &&
                std::memcmp(e->text, text.data(), text.size()) == 0)
                return e;
        }
    }

    // Under the lock. The entry is complete before the store publishes it.
    static void insert(Table& t, const detail::SymbolEntry* e) noexcept {
        const size_t mask = t.capacity - 1;
        size_t i = static_cast<size_t>(e->hash) & mask;
        while (t.slots[i].load(std::memory_order_relaxed)) i = (i + 1) & mask;
        t.slots[i].store(e, std::memory_order_release);
    }

    // Under the lock: a table of twice the capacity, published once it holds every entry.
    Table* grow(Table& old) {
        Table* t = new_table(old.capacity * 2);
        for (size_t i = 0; i < old.capacity; ++i)
            if (const auto* e = old.slots[i].load(std::memory_order_relaxed)) insert(*t, e);
        t->older = &old;  // never freed: a reader may still be probing it
        table_.store(t, std::memory_order_release);
        return t;
    }

    const detail::SymbolEntry* make_entry(std::string_view text, uint64_t hash) {
        const size_t need = sizeof(detail::SymbolEntry) + text.size() + 1;
        char* mem = alloc(need);
        auto* chars = mem + sizeof(detail::SymbolEntry);
        if (!text.empty()) std::memcpy(chars, text.data(), text.size());
        chars[text.size()] = '\0';
        return new (mem) detail::SymbolEntry{chars, static_cast<uint32_t>(text.size()),
                                             static_cast<uint32_t>(count_ + 1), hash};
    }

    char* alloc(size_t n) {
        n = (n + alignof(detail::SymbolEntry) - 1) & ~(alignof(detail::SymbolEntry) - 1);
        constexpr size_t align = alignof(detail::SymbolEntry);
        if (n > kBlockSize / 4) return static_cast<char*>(detail::default_alloc(n, align));
        if (!cur_ || used_ + n > kBlockSize) {
            cur_ = static_cast<char*>(detail::default_alloc(kBlockSize, align));
            used_ = 0;
        }
        char* p = cur_ + used_;
        used_ += n;
        return p;
    }

    std::mutex mutex_;  // held to insert; never to read
    std::atomic<Table*> table_{new_table(1024)};
    size_t count_ = 0;     // under the lock
    char* cur_ = nullptr;  // blocks are never freed
    size_t used_ = 0;
};

SymbolTable& table() {
    static SymbolTable* t = detail::create<SymbolTable>(default_allocator());  // leaked; see above
    return *t;
}

}  // namespace

Symbol::Symbol(std::string_view text)
    : e_(text.empty() ? &detail::kEmptySymbolEntry : table().intern(text)) {}

size_t Symbol::table_size() noexcept {
    return table().size();
}

}  // namespace baker
