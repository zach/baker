// Baked tables (table.hpp): the block, and the builder that makes one from the merged
// tree through a view's fix-up list, never seeing the view's type.

#include <baker/table.hpp>

#include "common/containers.hpp"
#include "curve_access.hpp"
#include "markers.hpp"
#include "table_build.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>

namespace baker {

namespace {

constexpr size_t kPoolAlign = 16;

constexpr size_t aligned(size_t n, size_t align) noexcept {
    return (n + align - 1) & ~(align - 1);
}

// FNV-1a over the tags' hashes: a row's place and key as one number.
uint64_t hash_tags(const Symbol* tags, size_t n) noexcept {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= tags[i].hash();
        h *= 1099511628211ULL;
    }
    return h;
}

const Symbol& any_tag() {
    static const Symbol s("*");
    return s;
}

// ListView's bytes, which the builder reads and writes through a fix-up: the layout
// view.hpp fixes at 16 bytes.
struct RawList {
    const void* data;
    uint32_t size;
    uint32_t packed;
};
static_assert(sizeof(RawList) == sizeof(ListView<float>));

// The governing arrays: each array at the `at` path, `*` matching any tag, with the tags
// its `*` segments stood for.
struct Governed {
    const Array* parent;
    uint32_t first_tag;  // into `tags`: the wildcard tags, wildcards() of them
};

struct Walker {
    std::span<const Symbol> at;
    Vec<Governed>& found;
    Vec<Symbol>& tags;
    Vec<Symbol> chain;  // the wildcard tags so far

    void walk(const Array& a, size_t depth) {
        for (const Node& n : a) {
            const Array* c = n.try_array();
            if (!c || c->tag().empty() || c->tag().is_meta()) continue;
            const bool wild = at[depth] == any_tag();
            if (!wild && at[depth] != c->tag()) continue;
            if (wild) chain.push_back(c->tag());
            if (depth + 1 == at.size()) {
                found.push_back({c, static_cast<uint32_t>(tags.size())});
                for (const Symbol t : chain) tags.push_back(t);
            } else if (c->height() > 1) {
                walk(*c, depth + 1);
            }
            if (wild) chain.pop_back();
        }
    }
};

bool is_record(const Node& n) noexcept {
    const Array* c = n.try_array();
    return c && !c->tag().empty() && !c->tag().is_meta();
}

// The alignment a list's elements need: the largest power of two dividing their size, up
// to the pool's 16 (a type's alignment divides its size).
constexpr size_t element_align(size_t size) noexcept {
    const size_t low = size & (~size + 1);
    return low == 0 || low > kPoolAlign ? kPoolAlign : low;
}

// The pool's two passes: measured, then written. `at` is null while measuring.
class Pool {
public:
    explicit Pool(char* base) noexcept : base_(base) {}
    [[nodiscard]] size_t used() const noexcept { return used_; }
    [[nodiscard]] char* base() const noexcept { return base_; }
    // `bytes` of room at `align`: where they are when writing, null when measuring.
    char* take(size_t bytes, size_t align) noexcept {
        used_ = aligned(used_, align);
        char* p = base_ ? base_ + used_ : nullptr;
        used_ += bytes;
        return p;
    }

private:
    char* base_;
    size_t used_ = 0;
};

// The pool's strings, each distinct text once. Measuring finds each string's place by its
// text, first against the last text of the same member, which a column of repeated values
// hits; writing replays those places in the same order, so no text is looked up twice.
class PoolStrings {
public:
    PoolStrings(const Allocator& alloc, size_t expected)
        : seen_(Seen::allocator_type(alloc)), places_(alloc) {
        places_.reserve(expected);
    }

    // Where `s`, a string at offset `member` of its struct, goes in the pool (or, measuring,
    // will go), taking room for its first copy.
    char* place(Pool& pool, std::string_view s, uint32_t member) {
        if (!pool.base()) {
            Recent& r = recent_[(member / alignof(std::string_view)) % kRecent];
            if (r.at != kNone && r.text == s) {
                places_.push_back(r.at);
                return nullptr;
            }
            const auto it = seen_.find(s);
            size_t at = 0;
            if (it != seen_.end()) {
                at = it->second;
                places_.push_back(at);
            } else {
                (void)pool.take(s.size() + 1, 1);
                at = pool.used() - s.size() - 1;
                seen_.emplace(s, at);
                places_.push_back(at | kFirst);
            }
            r = {s, at};
            return nullptr;
        }
        const size_t place = places_[next_++];
        if ((place & kFirst) == 0) return pool.base() + place;
        char* p = pool.take(s.size() + 1, 1);
        if (!s.empty()) std::memcpy(p, s.data(), s.size());  // an empty view may point nowhere
        p[s.size()] = '\0';
        return p;
    }

private:
    using Seen = HashMap<std::string_view, size_t>;
    static constexpr size_t kFirst = ~(~size_t{0} >> 1);  // the top bit: the first copy
    static constexpr size_t kNone = ~size_t{0};
    struct Recent {
        std::string_view text;
        size_t at = kNone;  // its offset in the pool; kNone: an empty slot
    };
    static constexpr size_t kRecent = 16;  // by the member's offset
    Seen seen_;
    Vec<size_t> places_;  // each string's offset in the pool, in the order placed
    size_t next_ = 0;     // writing: the next of places_
    Recent recent_[kRecent];
};

// Relocates what `row`'s fix-ups point at into the pool (or measures it). `temp` is room
// for one list element of any nested view, for reading elements that are views.
struct Relocator {
    Pool& pool;
    PoolStrings& strings;
    Vec<char>& temp;

    void run(char* row, std::span<const ViewFixup> fixups) {
        for (const ViewFixup& f : fixups) {
            char* at = row + f.offset;
            switch (f.kind) {
                case ViewFixupKind::Symbol: break;
                case ViewFixupKind::String: string(at, f.offset); break;
                case ViewFixupKind::Curve: curve(at); break;
                case ViewFixupKind::Struct: run(at, f.nested_fixups()); break;
                case ViewFixupKind::List: list(at, f); break;
            }
        }
    }

    void string(char* at, uint32_t member) {
        std::string_view s;
        std::memcpy(&s, at, sizeof(std::string_view));
        const char* p = strings.place(pool, s, member);
        if (!p) return;
        const std::string_view moved(p, s.size());
        std::memcpy(at, &moved, sizeof(std::string_view));
    }

    void curve(char* at) {
        Curve c;
        std::memcpy(&c, at, sizeof(Curve));
        const auto n = static_cast<uint32_t>(c.size());
        char* p = pool.take(size_t{n} * sizeof(CurvePoint), alignof(CurvePoint));
        if (!p) return;
        auto* points = reinterpret_cast<CurvePoint*>(p);
        for (uint32_t i = 0; i < n; ++i) points[i] = c.point(i);
        // Made in zeroed bytes, so the block's copy has zero padding, not the stack's.
        alignas(Curve) unsigned char moved[sizeof(Curve)] = {};
        static_assert(std::is_trivially_destructible_v<Curve>);
        new (moved) Curve(std::span<const CurvePoint>(points, n), c.mode(), c.extend());
        std::memcpy(at, moved, sizeof(Curve));
    }

    void list(char* at, const ViewFixup& f) {
        RawList raw{};
        std::memcpy(&raw, at, sizeof(RawList));
        if (raw.packed || !raw.data) return;  // packed already, or empty
        const auto* list = static_cast<const Array*>(raw.data);
        const Values v = list->values();
        const uint32_t n = raw.size;
        char* elements = pool.take(size_t{n} * f.element_size, element_align(f.element_size));
        for (uint32_t i = 0; i < n; ++i) {
            char* e = elements ? elements + size_t{i} * f.element_size : nullptr;
            // Each element into its slot (or into temp while measuring), then relocated.
            char* into =
                e ? e : (temp.resize(std::max(temp.size(), size_t{f.element_size})), temp.data());
            switch (f.element) {
                case ViewFixup::Element::Int32: {
                    const auto x = static_cast<int32_t>(v[i].try_int().value_or(0));
                    if (e) std::memcpy(e, &x, sizeof x);
                    break;
                }
                case ViewFixup::Element::Float: {
                    const auto x = static_cast<float>(v[i].try_float().value_or(0));
                    if (e) std::memcpy(e, &x, sizeof x);
                    break;
                }
                case ViewFixup::Element::Bool: {
                    bool x = false;
                    (void)v.read(i, x);
                    if (e) std::memcpy(e, &x, sizeof x);
                    break;
                }
                case ViewFixup::Element::Symbol: {
                    const Symbol x = v[i].try_sym().value_or(Symbol());
                    if (e) std::memcpy(e, &x, sizeof x);
                    break;
                }
                case ViewFixup::Element::String: {
                    const std::string_view x = v[i].try_str().value_or(std::string_view());
                    std::memcpy(into, &x, sizeof(std::string_view));
                    string(into, f.offset);
                    break;
                }
                case ViewFixup::Element::Curve: {
                    Curve x;
                    if (const Array* ea = v[i].try_array()) (void)read_curve(ea->values(), x);
                    std::memcpy(into, &x, sizeof(Curve));
                    curve(into);
                    break;
                }
                case ViewFixup::Element::Struct: {
                    if (f.init) f.init(into);
                    if (const Array* ea = v[i].try_array(); ea && f.read)
                        (void)f.read(*ea, into, nullptr);
                    run(into, f.nested_fixups());
                    break;
                }
            }
        }
        if (!elements) return;
        const RawList moved{elements, n, 1};
        std::memcpy(at, &moved, sizeof(RawList));
    }
};

Str record_path(std::span<const Symbol> at, const Symbol* wild, Symbol key) {
    Str p;
    size_t w = 0;
    for (const Symbol seg : at) {
        const Symbol s = seg == any_tag() ? wild[w++] : seg;
        p += (p.empty() ? "" : ".") + Str(s.str());
    }
    p += "." + Str(key.str());
    return p;
}

}  // namespace

// --- BakedTable ---

struct BakedTable::Layout {
    struct Fields {
        const Symbol* tags;
        const RecordId* ids;
        const KeyEntry* keys;
        uint32_t key_count;
        const IdEntry* idx;
        uint32_t id_count;
        const ParentEntry* parents;
        uint32_t parent_count;
    };
    static Fields fields(const BakedTable& t) noexcept {
        return {t.tags_,     t.ids_,      t.keys_,    t.key_count_,
                t.id_index_, t.id_count_, t.parents_, t.parent_count_};
    }
    static BakedTable* place(char* block, size_t bytes, const Allocator& alloc) {
        auto* t = new (block) BakedTable();
        t->alloc_ = &alloc;
        t->block_bytes_ = bytes;
        return t;
    }
    static void set(BakedTable& t, uint32_t rows, uint32_t row_size, uint32_t wildcards,
                    uint64_t digest, const void* rows_at, const Symbol* tags, const RecordId* ids,
                    const KeyEntry* keys, uint32_t key_count, const IdEntry* id_index,
                    uint32_t id_count, const ParentEntry* parents, uint32_t parent_count) {
        t.rows_ = rows;
        t.row_size_ = row_size;
        t.wildcards_ = wildcards;
        t.digest_ = digest;
        t.rows_at_ = rows_at;
        t.tags_ = tags;
        t.ids_ = ids;
        t.keys_ = keys;
        t.key_count_ = key_count;
        t.id_index_ = id_index;
        t.id_count_ = id_count;
        t.parents_ = parents;
        t.parent_count_ = parent_count;
    }
};

void BakedTable::ref() const noexcept {
    refs_.fetch_add(1, std::memory_order_relaxed);
}

void BakedTable::unref() const noexcept {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        const Allocator& a = *alloc_;
        const size_t bytes = block_bytes_;
        this->~BakedTable();
        detail::deallocate(a, const_cast<BakedTable*>(this), bytes);
    }
}

Symbol BakedTable::key_of(uint32_t i) const noexcept {
    return i < rows_ ? tags_[size_t{i} * (wildcards_ + 1) + wildcards_] : Symbol();
}

RecordId BakedTable::id_of(uint32_t i) const noexcept {
    return i < rows_ && ids_ ? ids_[i] : RecordId{};
}

std::optional<uint32_t> BakedTable::find(const Symbol* tags, size_t n) const noexcept {
    if (n != size_t{wildcards_} + 1) return std::nullopt;
    const uint64_t h = hash_tags(tags, n);
    const KeyEntry* lo = keys_;
    const KeyEntry* hi = keys_ + key_count_;
    lo = std::lower_bound(lo, hi, h, [](const KeyEntry& e, uint64_t x) { return e.hash < x; });
    for (; lo < hi && lo->hash == h; ++lo) {
        const Symbol* row = tags_ + size_t{lo->row} * n;
        bool same = true;
        for (size_t i = 0; i + 1 < n && same; ++i) same = row[i] == tags[i];
        if (same && (lo->alias ? lo->key == tags[n - 1] : row[n - 1] == tags[n - 1]))
            return lo->row;
    }
    return std::nullopt;
}

std::optional<uint32_t> BakedTable::index_of(Symbol key) const noexcept {
    return find(&key, 1);
}

std::optional<uint32_t> BakedTable::index_of(const Path& path) const noexcept {
    Symbol tags[Path::kMaxSegments];
    if (path.size() == 0 || path.size() > Path::kMaxSegments) return std::nullopt;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i].is_index) return std::nullopt;
        tags[i] = path[i].name;
    }
    return find(tags, path.size());
}

std::optional<uint32_t> BakedTable::index_of(std::string_view path) const noexcept {
    return index_of(Path(path));
}

std::optional<uint32_t> BakedTable::index_of(RecordId id) const noexcept {
    if (id.empty()) return std::nullopt;
    const IdEntry* e = std::lower_bound(id_index_, id_index_ + id_count_, id,
                                        [](const IdEntry& x, RecordId y) { return x.id < y; });
    if (e < id_index_ + id_count_ && e->id == id) return e->row;
    return std::nullopt;
}

IndexRange BakedTable::children_of(Symbol parent) const noexcept {
    for (uint32_t i = 0; i < parent_count_; ++i)
        if (parents_[i].tag == parent) return {parents_[i].first, parents_[i].count};
    return {};
}

// --- The builder ---

namespace detail {

Expected<void> parse_table_at(const TableSpec& spec, TableSlot& slot) {
    slot.at.clear();
    std::string_view at = spec.at;
    while (!at.empty()) {
        const size_t sp = at.find(' ');
        const std::string_view seg = at.substr(0, sp);
        if (!seg.empty()) slot.at.emplace_back(seg);
        at = sp == std::string_view::npos ? std::string_view() : at.substr(sp + 1);
    }
    if (slot.at.empty())
        return Unexpected(make_error(
            ErrorCode::Refused, "a table of shape " + Str(spec.name.empty() ? "?" : spec.name) +
                                    " has no (@at ...): a keyed shape is at a place"));
    return {};
}

namespace {
// What a walk found, kept in the slot: the arrays, held, and the tags of each one's `*`s.
void keep_sources(TableSlot& slot, const Vec<Governed>& found, const Vec<Symbol>& tags) {
    slot.sources.clear();
    for (const Governed& g : found) slot.sources.emplace_back(const_cast<Array*>(g.parent));
    slot.source_tags.assign(tags.begin(), tags.end());
}
}  // namespace

bool table_sources_unchanged(TableSlot& slot, const Array& root, const Allocator& alloc) {
    Vec<Governed> found(alloc);
    Vec<Symbol> tags(alloc);
    Walker w{{slot.at.data(), slot.at.size()}, found, tags, Vec<Symbol>(alloc)};
    w.walk(root, 0);
    if (found.size() != slot.sources.size() || tags.size() != slot.source_tags.size()) return false;
    if (!std::equal(tags.begin(), tags.end(), slot.source_tags.begin())) return false;
    // A merge builds anew an array a layer overrides, so an edit elsewhere hands back an
    // equal copy: compared by content, its records shared with the last tree's are pointer
    // tests.
    for (size_t i = 0; i < found.size(); ++i)
        if (found[i].parent != slot.sources[i].get() &&
            !structurally_equal(*found[i].parent, *slot.sources[i]))
            return false;
    keep_sources(slot, found, tags);
    return true;
}

Ref<const BakedTable> build_table(TableSlot& slot, const Array& root, const Allocator& alloc,
                                  const Callback<void(const Error&)>& warn) {
    const TableSpec& spec = slot.spec;
    const std::span<const Symbol> at(slot.at.data(), slot.at.size());
    uint32_t wildcards = 0;
    for (const Symbol s : at) wildcards += s == any_tag() ? 1 : 0;

    // The governing arrays and their records, in tree order.
    Vec<Governed> found(alloc);
    Vec<Symbol> wild(alloc);
    Walker w{at, found, wild, Vec<Symbol>(alloc)};
    w.walk(root, 0);
    keep_sources(slot, found, wild);
    slot.errors.clear();
    struct Record {
        const Array* array;
        uint32_t governed;
    };
    Vec<Record> records(alloc);
    for (uint32_t g = 0; g < found.size(); ++g) {
        const Array& p = *found[g].parent;
        for (uint32_t i = p.tag().empty() ? 0 : 1; i < p.size(); ++i)
            if (is_record(p[i])) records.push_back({p[i].try_array(), g});
    }
    const auto rows = static_cast<uint32_t>(records.size());

    // Each row read once, into a zeroed buffer laid out as the block's rows, so its
    // bytes, padding included, are what reading it in place would leave; its pool bytes
    // measured from the row as read. The rows move into the block once it is sized.
    const size_t rows_bytes = size_t{rows} * spec.row_size;
    const size_t row_align = std::max<size_t>(spec.row_align, kPoolAlign);
    struct RowsBuffer {
        const Allocator& alloc;
        size_t bytes;
        char* data;
        ~RowsBuffer() {
            if (data) detail::deallocate(alloc, data, bytes);
        }
    } read_rows{
        alloc, rows_bytes,
        rows_bytes ? static_cast<char*>(detail::allocate(alloc, rows_bytes, row_align)) : nullptr};
    if (rows_bytes) std::memset(read_rows.data, 0, rows_bytes);
    Vec<char> elem_temp(alloc);
    size_t string_members = 0;
    for (const ViewFixup& f : spec.fixups)
        string_members += f.kind == ViewFixupKind::String ? 1 : 0;
    PoolStrings strings(alloc, size_t{rows} * string_members);
    size_t pool_bytes = 0;
    {
        Pool measure(nullptr);
        Relocator r{measure, strings, elem_temp};
        for (uint32_t i = 0; i < rows; ++i) {
            const Record& rec = records[i];
            char* row = read_rows.data + size_t{i} * spec.row_size;
            spec.init(row);
            Error err;
            if (!spec.read(*rec.array, row, &err)) {
                const Symbol* wild_tags = wild.data() + found[rec.governed].first_tag;
                err.set_path(record_path(at, wild_tags, rec.array->tag()).view());
                slot.errors.push_back(err);
                if (warn) warn(err);
            }
            r.run(row, spec.fixups);
        }
        pool_bytes = measure.used();
    }
    // Aliases: an index entry each.
    uint32_t alias_count = 0;
    for (const Record& rec : records)
        if (rec.array->meta_first())
            if (const Array* aka = rec.array->meta(aka_tag()))
                alias_count += static_cast<uint32_t>(aka->values().size());

    // The block: the header, the rows, the pool, the tags, the keys, the parents, and, for a
    // view that looks records up by ID, the ids and their index.
    const size_t ids_n = spec.by_id ? rows : 0;
    const size_t rows_at = aligned(sizeof(BakedTable), row_align);
    const size_t pool_at = aligned(rows_at + rows_bytes, kPoolAlign);
    const size_t tags_at = aligned(pool_at + pool_bytes, alignof(Symbol));
    const size_t tags_n = size_t{rows} * (wildcards + 1);
    const size_t keys_at =
        aligned(tags_at + tags_n * sizeof(Symbol), alignof(BakedTable::KeyEntry));
    const size_t keys_n = size_t{rows} + alias_count;
    const size_t parents_at =
        aligned(keys_at + keys_n * sizeof(BakedTable::KeyEntry), alignof(BakedTable::ParentEntry));
    const size_t parents_n = wildcards ? found.size() : 0;
    const size_t ids_at =
        aligned(parents_at + parents_n * sizeof(BakedTable::ParentEntry), alignof(RecordId));
    const size_t idx_at = aligned(ids_at + ids_n * sizeof(RecordId), alignof(BakedTable::IdEntry));
    const size_t bytes = idx_at + ids_n * sizeof(BakedTable::IdEntry);
    char* block = static_cast<char*>(detail::allocate(alloc, bytes, row_align));
    std::memset(block, 0, bytes);
    if (rows_bytes) std::memcpy(block + rows_at, read_rows.data, rows_bytes);
    BakedTable* table = BakedTable::Layout::place(block, bytes, alloc);
    auto* tags = reinterpret_cast<Symbol*>(block + tags_at);
    auto* ids = spec.by_id ? reinterpret_cast<RecordId*>(block + ids_at) : nullptr;
    auto* keys = reinterpret_cast<BakedTable::KeyEntry*>(block + keys_at);
    auto* idx = reinterpret_cast<BakedTable::IdEntry*>(block + idx_at);
    auto* parents = reinterpret_cast<BakedTable::ParentEntry*>(block + parents_at);

    // The rows, relocated into the pool in place; the tags, ids and keys.
    Pool pool(block + pool_at);
    Relocator r{pool, strings, elem_temp};
    uint32_t key_count = 0, id_count = 0;
    for (uint32_t i = 0; i < rows; ++i) {
        const Record& rec = records[i];
        r.run(block + rows_at + size_t{i} * spec.row_size, spec.fixups);
        Symbol* row_tags = tags + size_t{i} * (wildcards + 1);
        for (uint32_t k = 0; k < wildcards; ++k)
            row_tags[k] = wild[found[rec.governed].first_tag + k];
        row_tags[wildcards] = rec.array->tag();
        if (ids) {
            new (ids + i) RecordId(rec.array->record_id());
            if (!ids[i].empty() && ids[i].valid()) idx[id_count++] = {ids[i], i, 0};
        }
        keys[key_count++] = {hash_tags(row_tags, wildcards + 1), Symbol(), i, 0};
        if (rec.array->meta_first())
            if (const Array* aka = rec.array->meta(aka_tag()))
                for (const Node& old : aka->values())
                    if (const auto s = old.try_sym()) {
                        row_tags[wildcards] = *s;
                        keys[key_count++] = {hash_tags(row_tags, wildcards + 1), *s, i, 1};
                        row_tags[wildcards] = rec.array->tag();
                    }
    }
    std::sort(keys, keys + key_count,
              [](const BakedTable::KeyEntry& a, const BakedTable::KeyEntry& b) {
                  return a.hash != b.hash ? a.hash < b.hash : a.row < b.row;
              });
    std::sort(idx, idx + id_count, [](const BakedTable::IdEntry& a, const BakedTable::IdEntry& b) {
        return a.id < b.id;
    });
    uint32_t parent_count = 0;
    if (wildcards) {
        uint32_t first = 0;
        for (uint32_t g = 0; g < found.size(); ++g) {
            uint32_t count = 0;
            while (first + count < rows && records[first + count].governed == g) ++count;
            parents[parent_count++] = {wild[found[g].first_tag + wildcards - 1], first, count};
            first += count;
        }
    }
    BakedTable::Layout::set(*table, rows, spec.row_size, wildcards, spec.digest, block + rows_at,
                            tags, ids, keys, key_count, idx, id_count, parents, parent_count);
    return Ref<const BakedTable>::adopt(table);
}

}  // namespace detail
}  // namespace baker

// --- The image's table section ---

namespace baker {

namespace {

// A section: digest u64, fixups u64, then u32 each: row_size, rows, block,
// wildcards, by_id, and the block's layout as offsets (rows, tags, ids, keys, key_count,
// idx, id_count, parents, parent_count); then the names (u32 count; u32 length + bytes,
// each padded to 8); then the block's bytes, padded to 8. In the block, the header is zero
// and every slot that holds a pointer or a symbol holds its file form instead: an offset
// into the block, or a name's index. The reader finds those slots as the writer did,
// through the view's fix-up list and the indices, so the section carries no list of them.
constexpr uintptr_t kNowhere = UINTPTR_MAX;  // a pointer that was null
struct FileString {                          // a std::string_view
    uintptr_t offset, size;
};
struct FileList {  // a ListView: RawList's layout
    uintptr_t offset;
    uint32_t size, packed;
};
struct FileCurve {  // a Curve's points, count and modes; no padding, so no stray bytes
    uintptr_t offset;
    uint32_t size;
    uint8_t mode, extend;
    uint16_t zero;
};
// A symbol is a uintptr_t: 0 for the empty one, else 1 + its index in the names.
static_assert(sizeof(FileString) == sizeof(std::string_view));
static_assert(sizeof(FileList) == sizeof(RawList));
static_assert(sizeof(FileCurve) <= sizeof(Curve));
static_assert(sizeof(FileCurve) == sizeof(uintptr_t) + 8);
static_assert(sizeof(uintptr_t) == sizeof(Symbol));

template <class T>
void put(Str& out, const T& v) {
    out.append(std::string_view(reinterpret_cast<const char*>(&v), sizeof v));
}
template <class T>
bool get(std::string_view in, size_t& at, T& v) {
    if (in.size() - at < sizeof v) return false;
    std::memcpy(&v, in.data() + at, sizeof v);
    at += sizeof v;
    return true;
}

uint64_t fixups_hash(std::span<const ViewFixup> fixups, uint64_t h = 1469598103934665603ULL) {
    for (const ViewFixup& f : fixups) {
        const uint64_t words[] = {f.offset, static_cast<uint64_t>(f.kind),
                                  static_cast<uint64_t>(f.element), f.element_size};
        for (const uint64_t w : words) {
            h ^= w;
            h *= 1099511628211ULL;
        }
        h = fixups_hash(f.nested_fixups(), h);
    }
    return h;
}

// A fix-up of one list element, as a member at offset 0: what a list of strings or curves
// walks per element.
constexpr ViewFixup element_fixup(ViewFixupKind kind) noexcept {
    return {0, kind, ViewFixup::Element::Int32, 0, nullptr, 0, nullptr, nullptr};
}
constexpr ViewFixup kStringElement = element_fixup(ViewFixupKind::String);
constexpr ViewFixup kCurveElement = element_fixup(ViewFixupKind::Curve);

// The writer's walk: each slot of the block in memory, written in its file form into the
// block's copy in the section.
struct Encoder {
    const char* base;  // the block in memory
    char* out;         // its copy in the section
    HashMap<Symbol, uint32_t>& index;
    Vec<Symbol>& names;

    [[nodiscard]] uintptr_t offset_of(const void* p) const noexcept {
        return p ? static_cast<uintptr_t>(static_cast<const char*>(p) - base) : kNowhere;
    }
    template <class T>
    void put_at(const char* at, const T& v, size_t slot) {
        char* o = out + (at - base);
        std::memset(o, 0, slot);
        std::memcpy(o, &v, sizeof v);
    }
    void symbol(const char* at) {
        Symbol s;
        std::memcpy(&s, at, sizeof(Symbol));
        uintptr_t v = 0;
        if (!s.empty()) {
            const auto it = index.find(s);
            if (it != index.end()) {
                v = it->second + 1;
            } else {
                index.emplace(s, static_cast<uint32_t>(names.size()));
                names.push_back(s);
                v = names.size();
            }
        }
        put_at(at, v, sizeof(Symbol));
    }
    void run(const char* row, std::span<const ViewFixup> fixups) {
        for (const ViewFixup& f : fixups) {
            const char* at = row + f.offset;
            switch (f.kind) {
                case ViewFixupKind::Symbol: symbol(at); break;
                case ViewFixupKind::String: {
                    std::string_view s;
                    std::memcpy(&s, at, sizeof(std::string_view));
                    put_at(at, FileString{offset_of(s.data()), s.size()}, sizeof(std::string_view));
                    break;
                }
                case ViewFixupKind::Curve: {
                    Curve c;
                    std::memcpy(&c, at, sizeof(Curve));
                    put_at(at,
                           FileCurve{
                               offset_of(CurveReader::points(c)), static_cast<uint32_t>(c.size()),
                               static_cast<uint8_t>(c.mode()), static_cast<uint8_t>(c.extend()), 0},
                           sizeof(Curve));
                    break;
                }
                case ViewFixupKind::Struct: run(at, f.nested_fixups()); break;
                case ViewFixupKind::List: list(at, f); break;
            }
        }
    }
    void list(const char* at, const ViewFixup& f) {
        RawList raw{};
        std::memcpy(&raw, at, sizeof(RawList));
        if (!raw.packed || !raw.data) {
            put_at(at, FileList{kNowhere, 0, 0}, sizeof(RawList));
            return;
        }
        put_at(at, FileList{offset_of(raw.data), raw.size, 1}, sizeof(RawList));
        const auto* elements = static_cast<const char*>(raw.data);
        for (uint32_t i = 0; i < raw.size; ++i) {
            const char* e = elements + size_t{i} * f.element_size;
            switch (f.element) {
                case ViewFixup::Element::Symbol: symbol(e); break;
                case ViewFixup::Element::String: run(e, {&kStringElement, 1}); break;
                case ViewFixup::Element::Curve: run(e, {&kCurveElement, 1}); break;
                case ViewFixup::Element::Struct: run(e, f.nested_fixups()); break;
                default: break;
            }
        }
    }
};

// The reader's walk: each slot of the copied block from its file form back to a pointer or
// a symbol, every offset checked against the block. `ok` turns false at the first that is
// not, and the walk then writes nothing more.
struct Decoder {
    char* block = nullptr;
    size_t bytes = 0;
    const Vec<Symbol>& names;
    bool ok = true;

    [[nodiscard]] bool within(uintptr_t offset, size_t n, size_t align) noexcept {
        if (offset <= bytes && n <= bytes - offset && offset % align == 0) return true;
        ok = false;
        return false;
    }
    void symbol(char* at) {
        uintptr_t v = 0;
        std::memcpy(&v, at, sizeof v);
        Symbol s;
        if (v > names.size()) {
            ok = false;
            return;
        }
        if (v) s = names[v - 1];
        std::memcpy(at, &s, sizeof(Symbol));
    }
    void run(char* row, std::span<const ViewFixup> fixups) {
        for (const ViewFixup& f : fixups) {
            if (!ok) return;
            char* at = row + f.offset;
            switch (f.kind) {
                case ViewFixupKind::Symbol: symbol(at); break;
                case ViewFixupKind::String: string(at); break;
                case ViewFixupKind::Curve: curve(at); break;
                case ViewFixupKind::Struct: run(at, f.nested_fixups()); break;
                case ViewFixupKind::List: list(at, f); break;
            }
        }
    }
    void string(char* at) {
        FileString fs{};
        std::memcpy(&fs, at, sizeof fs);
        std::string_view s;
        if (fs.offset != kNowhere) {
            if (!within(fs.offset, fs.size, 1)) return;
            s = std::string_view(block + fs.offset, fs.size);
        } else if (fs.size) {
            ok = false;
            return;
        }
        std::memcpy(at, &s, sizeof(std::string_view));
    }
    void curve(char* at) {
        FileCurve fc{};
        std::memcpy(&fc, at, sizeof fc);
        if (fc.mode > static_cast<uint8_t>(CurveMode::Constant) ||
            fc.extend > static_cast<uint8_t>(CurveExtend::Loop)) {
            ok = false;
            return;
        }
        const CurvePoint* points = nullptr;
        if (fc.offset != kNowhere) {
            if (!within(fc.offset, size_t{fc.size} * sizeof(CurvePoint), alignof(CurvePoint)))
                return;
            points = reinterpret_cast<const CurvePoint*>(block + fc.offset);
        } else if (fc.size) {
            ok = false;
            return;
        }
        alignas(Curve) unsigned char moved[sizeof(Curve)] = {};
        new (moved) Curve(std::span<const CurvePoint>(points, fc.size),
                          static_cast<CurveMode>(fc.mode), static_cast<CurveExtend>(fc.extend));
        std::memcpy(at, moved, sizeof(Curve));
    }
    void list(char* at, const ViewFixup& f) {
        FileList fl{};
        std::memcpy(&fl, at, sizeof fl);
        if (!fl.packed) {
            if (fl.offset != kNowhere || fl.size) ok = false;
            const RawList empty{};
            std::memcpy(at, &empty, sizeof empty);
            return;
        }
        if (fl.packed != 1 || f.element_size == 0 ||
            !within(fl.offset, size_t{fl.size} * f.element_size, element_align(f.element_size)))
            return;
        char* elements = block + fl.offset;
        for (uint32_t i = 0; i < fl.size && ok; ++i) {
            char* e = elements + size_t{i} * f.element_size;
            switch (f.element) {
                case ViewFixup::Element::Symbol: symbol(e); break;
                case ViewFixup::Element::String: string(e); break;
                case ViewFixup::Element::Curve: curve(e); break;
                case ViewFixup::Element::Struct: run(e, f.nested_fixups()); break;
                default: break;
            }
        }
        const RawList moved{elements, fl.size, 1};
        std::memcpy(at, &moved, sizeof moved);
    }
};

}  // namespace

namespace detail {

void record_table_sources(TableSlot& slot, const Array& root, const Allocator& alloc) {
    Vec<Governed> found(alloc);
    Vec<Symbol> tags(alloc);
    Walker w{{slot.at.data(), slot.at.size()}, found, tags, Vec<Symbol>(alloc)};
    w.walk(root, 0);
    keep_sources(slot, found, tags);
}

Expected<void> write_table(const BakedTable& table, const TableSpec& spec, Str& out) {
    const auto* base = reinterpret_cast<const char*>(&table);
    const BakedTable::Layout::Fields f = BakedTable::Layout::fields(table);
    const auto offset_of = [&](const void* p) {
        return p ? static_cast<uint32_t>(static_cast<const char*>(p) - base) : 0;
    };
    // The block's copy, its header zeroed (the reader places its own), then each slot that
    // points or names in its file form.
    Str block(std::string_view(base, table.block_bytes()));
    std::memset(block.data(), 0, sizeof(BakedTable));
    HashMap<Symbol, uint32_t> index;
    Vec<Symbol> names;
    Encoder e{base, block.data(), index, names};
    for (uint32_t i = 0; i < table.size(); ++i)
        e.run(static_cast<const char*>(table.row(i)), spec.fixups);
    const size_t per_row = size_t{table.wildcards()} + 1;
    for (size_t i = 0; i < size_t{table.size()} * per_row; ++i)
        e.symbol(reinterpret_cast<const char*>(f.tags + i));
    for (uint32_t i = 0; i < f.key_count; ++i)
        e.symbol(reinterpret_cast<const char*>(&f.keys[i].key));
    for (uint32_t i = 0; i < f.parent_count; ++i)
        e.symbol(reinterpret_cast<const char*>(&f.parents[i].tag));

    out.clear();
    put(out, spec.digest);
    put(out, fixups_hash(spec.fixups));
    const uint32_t head[] = {spec.row_size,
                             table.size(),
                             static_cast<uint32_t>(table.block_bytes()),
                             table.wildcards(),
                             table.by_id() ? 1U : 0U,
                             offset_of(table.row(0)),
                             offset_of(f.tags),
                             offset_of(f.ids),
                             offset_of(f.keys),
                             f.key_count,
                             offset_of(f.idx),
                             f.id_count,
                             offset_of(f.parents),
                             f.parent_count};
    for (const uint32_t h : head) put(out, h);
    put(out, static_cast<uint32_t>(names.size()));
    for (const Symbol s : names) {
        put(out, static_cast<uint32_t>(s.str().size()));
        out.append(s.str());
        out.resize(aligned(out.size(), 8));
    }
    out.append(block.view());
    out.resize(aligned(out.size(), 8));
    return {};
}

Ref<const BakedTable> read_table(std::string_view in, const TableSpec& spec, const Allocator& alloc,
                                 Error& why) {
    const auto refuse = [&](std::string_view what) {
        why = make_error(ErrorCode::Refused, what);
        return Ref<const BakedTable>();
    };
    constexpr std::string_view kDamaged = "a damaged table section";
    size_t at = 0;
    uint64_t digest = 0, fixups = 0;
    uint32_t head[14] = {};
    if (!get(in, at, digest) || !get(in, at, fixups)) return refuse(kDamaged);
    for (uint32_t& h : head)
        if (!get(in, at, h)) return refuse(kDamaged);
    const uint32_t row_size = head[0], rows = head[1], block_bytes = head[2], wildcards = head[3],
                   by_id = head[4], rows_at = head[5], tags_at = head[6], ids_at = head[7],
                   keys_at = head[8], key_count = head[9], idx_at = head[10], id_count = head[11],
                   parents_at = head[12], parent_count = head[13];
    if (digest != spec.digest) return refuse("another view's table");
    if (row_size != spec.row_size) return refuse("the view's size differs");
    if (fixups != fixups_hash(spec.fixups)) return refuse("the view's fix-up list differs");
    if ((by_id != 0) != spec.by_id) return refuse("the view's lookup by ID differs");
    uint32_t name_count = 0;
    if (!get(in, at, name_count)) return refuse(kDamaged);
    Vec<Symbol> names(alloc);
    for (uint32_t i = 0; i < name_count; ++i) {
        uint32_t len = 0;
        if (!get(in, at, len) || in.size() - at < len) return refuse(kDamaged);
        names.emplace_back(in.substr(at, len));
        at = aligned(at + len, 8);
    }
    if (in.size() < at || in.size() - at < block_bytes) return refuse(kDamaged);
    // Every array the header places must lie in the block, aligned, past the header.
    const size_t align = std::max<size_t>(spec.row_align, kPoolAlign);
    const auto fits = [&](size_t offset, size_t n, size_t size, size_t a) {
        return offset >= sizeof(BakedTable) && offset <= block_bytes && offset % a == 0 &&
               (size == 0 || n <= (block_bytes - offset) / size);
    };
    if (wildcards >= Path::kMaxSegments || !fits(rows_at, rows, row_size, align) ||
        !fits(tags_at, size_t{rows} * (wildcards + 1), sizeof(Symbol), alignof(Symbol)) ||
        !fits(keys_at, key_count, sizeof(BakedTable::KeyEntry), alignof(BakedTable::KeyEntry)) ||
        !fits(parents_at, parent_count, sizeof(BakedTable::ParentEntry),
              alignof(BakedTable::ParentEntry)) ||
        (by_id &&
         (!fits(ids_at, rows, sizeof(RecordId), alignof(RecordId)) || id_count > rows ||
          !fits(idx_at, id_count, sizeof(BakedTable::IdEntry), alignof(BakedTable::IdEntry)))) ||
        (!by_id && id_count != 0))
        return refuse(kDamaged);
    char* block = static_cast<char*>(detail::allocate(alloc, block_bytes, align));
    std::memcpy(block, in.data() + at, block_bytes);
    BakedTable* table = BakedTable::Layout::place(block, block_bytes, alloc);
    const auto* keys = reinterpret_cast<const BakedTable::KeyEntry*>(block + keys_at);
    const auto* idx = reinterpret_cast<const BakedTable::IdEntry*>(block + idx_at);
    const auto* parents = reinterpret_cast<const BakedTable::ParentEntry*>(block + parents_at);
    BakedTable::Layout::set(*table, rows, row_size, wildcards, digest, block + rows_at,
                            reinterpret_cast<const Symbol*>(block + tags_at),
                            by_id ? reinterpret_cast<const RecordId*>(block + ids_at) : nullptr,
                            keys, key_count, by_id ? idx : nullptr, id_count, parents,
                            parent_count);
    Ref<const BakedTable> ref = Ref<const BakedTable>::adopt(table);
    // The rows through the fix-ups, then the indices' symbols, each slot to its pointer.
    Decoder d{block, block_bytes, names};
    for (uint32_t i = 0; i < rows && d.ok; ++i)
        d.run(block + rows_at + size_t{i} * row_size, spec.fixups);
    const size_t tags_n = size_t{rows} * (wildcards + 1);
    for (size_t i = 0; i < tags_n && d.ok; ++i) d.symbol(block + tags_at + i * sizeof(Symbol));
    for (uint32_t i = 0; i < key_count && d.ok; ++i) {
        if (keys[i].row >= rows) return refuse(kDamaged);
        d.symbol(block + keys_at + size_t{i} * sizeof(BakedTable::KeyEntry) +
                 offsetof(BakedTable::KeyEntry, key));
    }
    for (uint32_t i = 0; i < parent_count && d.ok; ++i) {
        if (parents[i].first > rows || parents[i].count > rows - parents[i].first)
            return refuse(kDamaged);
        d.symbol(block + parents_at + size_t{i} * sizeof(BakedTable::ParentEntry) +
                 offsetof(BakedTable::ParentEntry, tag));
    }
    for (uint32_t i = 0; i < id_count; ++i)
        if (idx[i].row >= rows) return refuse(kDamaged);
    if (!d.ok) return refuse(kDamaged);
    return ref;
}

}  // namespace detail
}  // namespace baker
