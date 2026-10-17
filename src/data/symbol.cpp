#include <baker/allocator.hpp>
#include <baker/symbol.hpp>

#include <cstring>
#include <mutex>

namespace baker {
namespace {

// Append-only arena + open-addressed hash table of entry pointers, from the default
// allocator. Never destroyed: symbols are immortal, and leaking the table keeps them valid
// during static destruction too.
class SymbolTable {
public:
    const detail::SymbolEntry* intern(std::string_view text) {
        const uint64_t hash = detail::hash_symbol_text(text);
        std::lock_guard lock(mutex_);
        size_t mask = capacity_ - 1;
        for (size_t i = static_cast<size_t>(hash) & mask;; i = (i + 1) & mask) {
            const detail::SymbolEntry* e = slots_[i];
            if (!e) break;
            if (e->hash == hash && e->size == text.size() &&
                std::memcmp(e->text, text.data(), text.size()) == 0)
                return e;
        }
        if ((count_ + 1) * 2 > capacity_) grow();
        const detail::SymbolEntry* e = make_entry(text, hash);
        insert(e);
        ++count_;
        return e;
    }

    size_t size() {
        std::lock_guard lock(mutex_);
        return count_;
    }

private:
    static constexpr size_t kBlockSize = size_t{64} * 1024;

    using Slot = const detail::SymbolEntry*;

    static Slot* new_slots(size_t n) {
        auto* s = static_cast<Slot*>(detail::default_alloc(n * sizeof(Slot), alignof(Slot)));
        for (size_t i = 0; i < n; ++i) s[i] = nullptr;
        return s;
    }

    void insert(const detail::SymbolEntry* e) {
        size_t mask = capacity_ - 1;
        size_t i = static_cast<size_t>(e->hash) & mask;
        while (slots_[i]) i = (i + 1) & mask;
        slots_[i] = e;
    }

    void grow() {
        Slot* old = slots_;
        const size_t old_capacity = capacity_;
        slots_ = new_slots(capacity_ * 2);
        capacity_ *= 2;
        for (size_t i = 0; i < old_capacity; ++i)
            if (old[i]) insert(old[i]);
        detail::default_free(old, old_capacity * sizeof(Slot));
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

    std::mutex mutex_;
    size_t capacity_ = 1024;
    Slot* slots_ = new_slots(capacity_);
    size_t count_ = 0;
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
