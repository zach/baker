#include <baker/allocator.hpp>
#include <baker/error.hpp>
#include <baker/symbol.hpp>

#include "intern_site.hpp"
#include "text.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_MSC_VER)
#define BAKER_NOINLINE __declspec(noinline)
#define BAKER_INLINE __forceinline
#else
#define BAKER_NOINLINE [[gnu::noinline]]
#define BAKER_INLINE [[gnu::always_inline]] inline
#endif

namespace baker {

// The hash is a promise (symbol_hash): these golden values are FNV-1a 64's, and a change of the
// algorithm fails the library's own build.
static_assert(symbol_hash("") == 0xcbf29ce484222325ull);
static_assert(symbol_hash("a") == 0xaf63dc4c8601ec8cull);  // FNV-1a 64's own test vector
static_assert(symbol_hash("jump") == 0xeb1776ddf832a4cdull);
static_assert(symbol_hash("\xC3\xA9p\xC3\xA9"
                          "e") == 0x3ae05f984f97964aull);  // "épée", UTF-8

namespace {

thread_local detail::InternSite this_site;
thread_local detail::NewSymbols* this_new_symbols = nullptr;

void report_by_default(const Error& report) {
    detail::fatal(to_string(report));
}

std::atomic<SymbolCollisionHandler> collision_handler{report_by_default};

// A name whose hash an earlier name has: reported outside the table's lock, so a handler may
// intern. Cold: it needs two names among 2^64 hashes.
void report_collision(const detail::SymbolEntry& first, const detail::SymbolEntry& second) {
    static constexpr char kDigits[] = "0123456789abcdef";
    char hex[19] = "0x";
    for (int i = 0; i < 16; ++i) hex[2 + i] = kDigits[(first.hash >> (60 - 4 * i)) & 15];
    hex[18] = '\0';
    const Str message =
        "the names '" + Str(std::string_view(first.text, first.size & detail::kSymbolSizeMask)) +
        "' and '" + Str(std::string_view(second.text, second.size & detail::kSymbolSizeMask)) +
        "' share the hash " + hex +
        ": code that keys on Symbol::hash() cannot tell them apart; as Symbols "
        "they stay distinct";
    const Error report = make_error(ErrorCode::Collision, message, this_site.file,
                                    this_site.line ? *this_site.line : 0);
    collision_handler.load(std::memory_order_acquire)(report);
}

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
    const detail::SymbolEntry* intern(std::string_view text, uint64_t hash) {
        if (const auto* e = find(*table_.load(std::memory_order_acquire), text, hash)) return e;
        return add(text, hash);
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
        auto* t = static_cast<Table*>(
            detail::default_alloc(sizeof(Table), alignof(Table), MemoryKind::Symbols));
        auto* s = static_cast<Slot*>(
            detail::default_alloc(n * sizeof(Slot), alignof(Slot), MemoryKind::Symbols));
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
            if (e->hash == hash && (e->size & detail::kSymbolSizeMask) == text.size() &&
                std::memcmp(e->text, text.data(), text.size()) == 0)
                return e;
        }
    }

    // A name the lock-free probe did not find: out of line, so the probe compiles as it would
    // alone.
    BAKER_NOINLINE const detail::SymbolEntry* add(std::string_view text, uint64_t hash) {
        const detail::SymbolEntry* e = nullptr;
        const detail::SymbolEntry* clash = nullptr;  // an earlier name with this hash
        {
            std::lock_guard lock(mutex_);
            Table* t = table_.load(std::memory_order_relaxed);
            // Interned since the probe? A build that is not shipping also notes, on this probe
            // alone, a name it passes with the same hash: every new name passes them all.
            if constexpr (kShipping) {
                if (const auto* found = find(*t, text, hash)) return found;
            } else {
                if (const auto* found = find_noting(*t, text, hash, clash)) return found;
            }
            if ((count_ + 1) * 2 > t->capacity) t = grow(*t);
            e = make_entry(text, hash);
            insert(*t, e);
            ++count_;
        }
        if (detail::NewSymbols* n = this_new_symbols; n && ++n->count > n->limit)
            *n->trip = n->trip_at;
        if constexpr (!kShipping)
            if (clash) report_collision(*clash, *e);
        return e;
    }

    // find(), noting in `clash` the first entry it passes with the same hash and other text.
    // Under the lock, and only to insert: the lock-free probe above stays as it is.
    static const detail::SymbolEntry* find_noting(const Table& t, std::string_view text,
                                                  uint64_t hash,
                                                  const detail::SymbolEntry*& clash) noexcept {
        const size_t mask = t.capacity - 1;
        for (size_t i = static_cast<size_t>(hash) & mask;; i = (i + 1) & mask) {
            const detail::SymbolEntry* e = t.slots[i].load(std::memory_order_relaxed);
            if (!e) return nullptr;
            if (e->hash != hash) continue;
            if ((e->size & detail::kSymbolSizeMask) == text.size() &&
                std::memcmp(e->text, text.data(), text.size()) == 0)
                return e;
            if (!clash) clash = e;
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
        uint32_t size = static_cast<uint32_t>(text.size());
        if (size > 1 && text[0] == '@') size |= detail::kSymbolMetaBit;  // the metadata sigil
        return new (mem) detail::SymbolEntry{chars, size, static_cast<uint32_t>(count_ + 1), hash};
    }

    char* alloc(size_t n) {
        n = (n + alignof(detail::SymbolEntry) - 1) & ~(alignof(detail::SymbolEntry) - 1);
        constexpr size_t align = alignof(detail::SymbolEntry);
        if (n > kBlockSize / 4)
            return static_cast<char*>(detail::default_alloc(n, align, MemoryKind::Symbols));
        if (!cur_ || used_ + n > kBlockSize) {
            cur_ =
                static_cast<char*>(detail::default_alloc(kBlockSize, align, MemoryKind::Symbols));
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

// Inlined into the constructor, as the probe it guards is.
BAKER_INLINE SymbolTable& table() {
    static SymbolTable* t =
        detail::create<SymbolTable>(default_allocator(MemoryKind::Symbols));  // leaked; see above
    return *t;
}

}  // namespace

Symbol::Symbol(std::string_view text)
    : e_(text.empty() ? &detail::kEmptySymbolEntry : table().intern(text, symbol_hash(text))) {}

const detail::SymbolEntry* detail::intern_hashed(std::string_view text, uint64_t hash) {
    return text.empty() ? &kEmptySymbolEntry : table().intern(text, hash);
}

size_t Symbol::table_size() noexcept {
    return table().size();
}

SymbolCollisionHandler set_symbol_collision_handler(SymbolCollisionHandler handler) noexcept {
    return collision_handler.exchange(handler ? handler : report_by_default,
                                      std::memory_order_acq_rel);
}

detail::NewSymbols* detail::count_new_symbols(NewSymbols* counter) noexcept {
    NewSymbols* const was = this_new_symbols;
    this_new_symbols = counter;
    return was;
}

detail::InternSite detail::set_intern_site(InternSite site) noexcept {
    const InternSite was = this_site;
    this_site = site;
    return was;
}

}  // namespace baker
