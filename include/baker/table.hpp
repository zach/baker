#pragma once

// Baked tables (the schema layer's W2): a viewed (@keyed) shape's records
// as a packed array of its view struct, built from the merged tree once per change and
// read without walking it.
//
//   #include "tables.hpp"                       // generated: register_tables
//   game::register_tables(cfg);                 // or baker::add_table<game::Move>(cfg)
//   cfg.push_layer(...);
//   baker::Table<game::Move> moves = baker::table<game::Move>(cfg);
//   if (auto i = moves.index_of("kestrel.jab")) {          // the path below the table
//       const game::Move& jab = moves[*i];                 // one indexed load
//   }
//   for (uint32_t i : moves.children_of("kestrel"_sym)) ... // one parent's records
//
// A table is one block from the Config's allocator: the rows, a pool its strings, lists
// and curves point into, and three indices (keys and aliases, IDs, parents). It is
// refcounted: a Table<T> handle keeps its block while it is held, so a reload that
// rebuilds the table takes nothing from a reader. Rows are in the merged tree's order,
// grouped by parent; an index is stable across a reload while the table's set of records
// is unchanged. A record's field that is not a literal (a {command}, a $variable) reads
// as the member's default, with a located error to the Config's warning handler.
//
// The library never sees T: the generated struct's fixups(), kAt and kKeyed (view.hpp)
// and its reader are what add_table<T> hands over.

#include <baker/config.hpp>
#include <baker/path.hpp>
#include <baker/record_id.hpp>
#include <baker/ref.hpp>
#include <baker/symbol.hpp>
#include <baker/view.hpp>
#include <baker/view_read.hpp>

#include <atomic>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace baker {

// A run of row indices, [first, first + count): one parent's records.
struct IndexRange {
    uint32_t first = 0;
    uint32_t count = 0;

    class Iterator {
    public:
        constexpr explicit Iterator(uint32_t i) noexcept : i_(i) {}
        [[nodiscard]] constexpr uint32_t operator*() const noexcept { return i_; }
        constexpr Iterator& operator++() noexcept {
            ++i_;
            return *this;
        }
        [[nodiscard]] constexpr bool operator==(const Iterator& o) const noexcept {
            return i_ == o.i_;
        }

    private:
        uint32_t i_;
    };
    [[nodiscard]] constexpr Iterator begin() const noexcept { return Iterator(first); }
    [[nodiscard]] constexpr Iterator end() const noexcept { return Iterator(first + count); }
    [[nodiscard]] constexpr bool empty() const noexcept { return count == 0; }
    [[nodiscard]] constexpr uint32_t size() const noexcept { return count; }
};

// What add_table<T> registers: the view, type-erased.
struct TableSpec {
    uint64_t digest = 0;     // T::kShapeDigest: how the table is found
    std::string_view name;   // T::kShapeName: tool text, for errors; empty in shipping
    std::string_view at;     // T::kAt: where the records are
    uint32_t row_size = 0;   // sizeof(T)
    uint32_t row_align = 0;  // alignof(T)
    std::span<const ViewFixup> fixups;
    void (*init)(void* row) = nullptr;  // new (row) T{}
    ViewRead read = nullptr;            // ViewReader<T>::read
};

// A built table, type-erased: the block a Table<T> handle reads through. Public for the
// handle and the kit; a game reads through Table<T>.
class BakedTable {
public:
    void ref() const noexcept;
    void unref() const noexcept;

    [[nodiscard]] uint32_t size() const noexcept { return rows_; }
    [[nodiscard]] const void* row(uint32_t i) const noexcept {
        return static_cast<const char*>(rows_at_) + size_t{i} * row_size_;
    }
    [[nodiscard]] uint32_t row_size() const noexcept { return row_size_; }
    [[nodiscard]] uint64_t digest() const noexcept { return digest_; }
    // The record's key, and its (@id ...) or an empty one.
    [[nodiscard]] Symbol key_of(uint32_t i) const noexcept;
    [[nodiscard]] RecordId id_of(uint32_t i) const noexcept;
    // The row keyed `key` in a table whose shape's (@at ...) has no `*`; a key that is a
    // record's (@aka ...) resolves to it.
    [[nodiscard]] std::optional<uint32_t> index_of(Symbol key) const noexcept;
    // The row at `path` below the table: the tags the shape's `*` segments stand for, then
    // the key, as "kestrel.jab" or {"kestrel", "jab"}. The text form parses the path and
    // interns its names on every call (a tool's, or a one-off); a hot read holds a Path, a
    // Symbol or a RecordId and looks up by that.
    [[nodiscard]] std::optional<uint32_t> index_of(std::string_view path) const noexcept;
    [[nodiscard]] std::optional<uint32_t> index_of(const char* path) const noexcept {
        return index_of(std::string_view(path));
    }
    [[nodiscard]] std::optional<uint32_t> index_of(const Path& path) const noexcept;
    [[nodiscard]] std::optional<uint32_t> index_of(RecordId id) const noexcept;
    // The rows under the parent tagged `parent` (the shape's last `*`), contiguous; empty
    // for none, or for a table with no `*`.
    [[nodiscard]] IndexRange children_of(Symbol parent) const noexcept;
    // How many `*` the shape's (@at ...) has: the segments a path to a row has before its key.
    [[nodiscard]] uint32_t wildcards() const noexcept { return wildcards_; }
    // The pool and the indices, as bytes beside the rows: for the image writer (step 4).
    [[nodiscard]] size_t block_bytes() const noexcept { return block_bytes_; }

    struct Layout;  // table.cpp: builds one
    // The key index: (hash of the row's tags and key, or an alias in the key's place) and
    // the row, sorted by hash; the ID index, sorted; the parents: (tag, first, count).
    struct KeyEntry {
        uint64_t hash = 0;
        Symbol key;  // an alias entry's alias, which the hash is of; else empty
        uint32_t row = 0;
        uint32_t alias = 0;  // 1: an alias entry
    };
    struct IdEntry {
        RecordId id;
        uint32_t row = 0;
    };
    struct ParentEntry {
        Symbol tag;
        uint32_t first = 0, count = 0;
    };

private:
    friend struct Layout;
    BakedTable() = default;
    [[nodiscard]] std::optional<uint32_t> find(const Symbol* tags, size_t n) const noexcept;

    const Allocator* alloc_ = nullptr;
    mutable std::atomic<uint32_t> refs_{1};
    uint32_t rows_ = 0;
    uint32_t row_size_ = 0;
    uint32_t wildcards_ = 0;
    uint64_t digest_ = 0;
    size_t block_bytes_ = 0;
    const void* rows_at_ = nullptr;
    // Per row: its wildcards() ancestor tags, then its key; and its ID.
    const Symbol* tags_ = nullptr;
    const RecordId* ids_ = nullptr;
    const KeyEntry* keys_ = nullptr;
    uint32_t key_count_ = 0;
    const IdEntry* id_index_ = nullptr;
    uint32_t id_count_ = 0;
    const ParentEntry* parents_ = nullptr;
    uint32_t parent_count_ = 0;
};

// The typed handle: a refcounted block, or empty when the Config has no such table.
template <class T>
class Table {
public:
    Table() = default;
    explicit Table(Ref<const BakedTable> block) noexcept : block_(std::move(block)) {}

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(block_); }
    [[nodiscard]] uint32_t size() const noexcept { return block_ ? block_->size() : 0; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    // Row i: an indexed load, unchecked. at() checks.
    [[nodiscard]] const T& operator[](uint32_t i) const noexcept {
        return *static_cast<const T*>(block_->row(i));
    }
    [[nodiscard]] const T* at(uint32_t i) const noexcept {
        return i < size() ? &(*this)[i] : nullptr;
    }
    [[nodiscard]] std::span<const T> rows() const noexcept {
        return block_ ? std::span<const T>(static_cast<const T*>(block_->row(0)), size())
                      : std::span<const T>();
    }
    [[nodiscard]] Symbol key_of(uint32_t i) const noexcept { return block_->key_of(i); }
    [[nodiscard]] RecordId id_of(uint32_t i) const noexcept { return block_->id_of(i); }
    [[nodiscard]] std::optional<uint32_t> index_of(Symbol key) const noexcept {
        return block_ ? block_->index_of(key) : std::nullopt;
    }
    [[nodiscard]] std::optional<uint32_t> index_of(std::string_view path) const noexcept {
        return block_ ? block_->index_of(path) : std::nullopt;
    }
    [[nodiscard]] std::optional<uint32_t> index_of(const char* path) const noexcept {
        return index_of(std::string_view(path));
    }
    [[nodiscard]] std::optional<uint32_t> index_of(const Path& path) const noexcept {
        return block_ ? block_->index_of(path) : std::nullopt;
    }
    [[nodiscard]] std::optional<uint32_t> index_of(RecordId id) const noexcept {
        return block_ ? block_->index_of(id) : std::nullopt;
    }
    [[nodiscard]] IndexRange children_of(Symbol parent) const noexcept {
        return block_ ? block_->children_of(parent) : IndexRange{};
    }
    [[nodiscard]] const BakedTable* block() const noexcept { return block_.get(); }

private:
    Ref<const BakedTable> block_;
};

// Registers T's table with the Config: built from the merged tree now if it has one, and
// again at every change that touches its records. T is a generated view of a (@keyed)
// shape; registering a shape that is not keyed is refused, as is one registered twice.
template <class T>
Expected<void> add_table(Config& cfg) {
    static_assert(T::kKeyed, "a table is a (@keyed) shape's: mark the shape (@keyed)");
    static_assert(std::is_trivially_destructible_v<T>, "a view holds only trivial members");
    return cfg.add_table(TableSpec{
        .digest = T::kShapeDigest,
        .name = T::kShapeName,
        .at = T::kAt,
        .row_size = sizeof(T),
        .row_align = alignof(T),
        .fixups = T::fixups(),
        .init = [](void* row) { new (row) T{}; },
        .read = &view_detail::read_erased<T>,
    });
}

// T's table as of the last change, or an empty handle when none is registered.
template <class T>
[[nodiscard]] Table<T> table(const Config& cfg) noexcept {
    return Table<T>(cfg.table_block(T::kShapeDigest));
}

}  // namespace baker
