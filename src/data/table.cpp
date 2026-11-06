// Baked tables (table.hpp): the block, and the builder that makes one from the merged
// tree through a view's fix-up list, never seeing the view's type.

#include <baker/table.hpp>

#include "common/containers.hpp"
#include "curve_access.hpp"
#include "markers.hpp"
#include "table_build.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstring>

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

// The pool's two passes: measured, then written. `at` is null while measuring.
class Pool {
public:
    explicit Pool(char* base) noexcept : base_(base) {}
    [[nodiscard]] size_t used() const noexcept { return used_; }
    // `bytes` of room, 16-aligned: where they are when writing, null when measuring.
    char* take(size_t bytes) noexcept {
        used_ = aligned(used_, kPoolAlign);
        char* p = base_ ? base_ + used_ : nullptr;
        used_ += bytes;
        return p;
    }

private:
    char* base_;
    size_t used_ = 0;
};

// Relocates what `row`'s fix-ups point at into the pool (or measures it). `temp` is room
// for one list element of any nested view, for reading elements that are views.
struct Relocator {
    Pool& pool;
    Vec<char>& temp;

    void run(char* row, std::span<const ViewFixup> fixups) {
        for (const ViewFixup& f : fixups) {
            char* at = row + f.offset;
            switch (f.kind) {
                case ViewFixupKind::Symbol: break;
                case ViewFixupKind::String: string(at); break;
                case ViewFixupKind::Curve: curve(at); break;
                case ViewFixupKind::Struct: run(at, f.nested_fixups()); break;
                case ViewFixupKind::List: list(at, f); break;
            }
        }
    }

    void string(char* at) {
        std::string_view s;
        std::memcpy(&s, at, sizeof(std::string_view));
        char* p = pool.take(s.size() + 1);
        if (!p) return;
        if (!s.empty()) std::memcpy(p, s.data(), s.size());  // an empty view may point nowhere
        p[s.size()] = '\0';
        const std::string_view moved(p, s.size());
        std::memcpy(at, &moved, sizeof(std::string_view));
    }

    void curve(char* at) {
        Curve c;
        std::memcpy(&c, at, sizeof(Curve));
        const auto n = static_cast<uint32_t>(c.size());
        char* p = pool.take(size_t{n} * sizeof(CurvePoint));
        if (!p) return;
        auto* points = reinterpret_cast<CurvePoint*>(p);
        for (uint32_t i = 0; i < n; ++i) points[i] = c.point(i);
        const Curve moved(std::span<const CurvePoint>(points, n), c.mode(), c.extend());
        std::memcpy(at, &moved, sizeof(Curve));
    }

    void list(char* at, const ViewFixup& f) {
        RawList raw{};
        std::memcpy(&raw, at, sizeof(RawList));
        if (raw.packed || !raw.data) return;  // packed already, or empty
        const auto* list = static_cast<const Array*>(raw.data);
        const Values v = list->values();
        const uint32_t n = raw.size;
        char* elements = pool.take(size_t{n} * f.element_size);
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
                    string(into);
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
    return i < rows_ ? ids_[i] : RecordId{};
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

bool table_sources_unchanged(const TableSlot& slot, const Array& root, const Allocator& alloc) {
    Vec<Governed> found(alloc);
    Vec<Symbol> tags(alloc);
    Walker w{{slot.at.data(), slot.at.size()}, found, tags, Vec<Symbol>(alloc)};
    w.walk(root, 0);
    if (found.size() != slot.sources.size()) return false;
    for (size_t i = 0; i < found.size(); ++i)
        if (found[i].parent != slot.sources[i]) return false;
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
    slot.sources.clear();
    slot.errors.clear();
    struct Record {
        const Array* array;
        uint32_t governed;
    };
    Vec<Record> records(alloc);
    for (uint32_t g = 0; g < found.size(); ++g) {
        slot.sources.push_back(found[g].parent);
        const Array& p = *found[g].parent;
        for (uint32_t i = p.tag().empty() ? 0 : 1; i < p.size(); ++i)
            if (is_record(p[i])) records.push_back({p[i].try_array(), g});
    }
    const auto rows = static_cast<uint32_t>(records.size());

    // Pass 1: each row read into temp, its pool bytes measured.
    Vec<char> row_temp(alloc), elem_temp(alloc);
    row_temp.resize(spec.row_size);
    size_t pool_bytes = 0;
    {
        Pool measure(nullptr);
        Relocator r{measure, elem_temp};
        for (const Record& rec : records) {
            spec.init(row_temp.data());
            (void)spec.read(*rec.array, row_temp.data(), nullptr);
            r.run(row_temp.data(), spec.fixups);
        }
        pool_bytes = measure.used();
    }
    // Aliases: an index entry each.
    uint32_t alias_count = 0;
    for (const Record& rec : records)
        if (rec.array->meta_first())
            if (const Array* aka = rec.array->meta(aka_tag()))
                alias_count += static_cast<uint32_t>(aka->values().size());

    // The block: the header, the rows, the pool, the tags, the ids, the three indices.
    const size_t rows_at =
        aligned(sizeof(BakedTable), std::max<size_t>(spec.row_align, kPoolAlign));
    const size_t pool_at = aligned(rows_at + size_t{rows} * spec.row_size, kPoolAlign);
    const size_t tags_at = aligned(pool_at + pool_bytes, alignof(Symbol));
    const size_t tags_n = size_t{rows} * (wildcards + 1);
    const size_t ids_at = aligned(tags_at + tags_n * sizeof(Symbol), alignof(RecordId));
    const size_t keys_at =
        aligned(ids_at + size_t{rows} * sizeof(RecordId), alignof(BakedTable::KeyEntry));
    const size_t keys_n = size_t{rows} + alias_count;
    const size_t idx_at =
        aligned(keys_at + keys_n * sizeof(BakedTable::KeyEntry), alignof(BakedTable::IdEntry));
    const size_t parents_at = aligned(idx_at + size_t{rows} * sizeof(BakedTable::IdEntry),
                                      alignof(BakedTable::ParentEntry));
    const size_t parents_n = wildcards ? found.size() : 0;
    const size_t bytes = parents_at + parents_n * sizeof(BakedTable::ParentEntry);
    char* block = static_cast<char*>(
        detail::allocate(alloc, bytes, std::max<size_t>(spec.row_align, kPoolAlign)));
    std::memset(block, 0, bytes);
    BakedTable* table = BakedTable::Layout::place(block, bytes, alloc);
    auto* tags = reinterpret_cast<Symbol*>(block + tags_at);
    auto* ids = reinterpret_cast<RecordId*>(block + ids_at);
    auto* keys = reinterpret_cast<BakedTable::KeyEntry*>(block + keys_at);
    auto* idx = reinterpret_cast<BakedTable::IdEntry*>(block + idx_at);
    auto* parents = reinterpret_cast<BakedTable::ParentEntry*>(block + parents_at);

    // Pass 2: the rows in place, relocated into the pool; the tags, ids and keys.
    Pool pool(block + pool_at);
    Relocator r{pool, elem_temp};
    uint32_t key_count = 0, id_count = 0;
    for (uint32_t i = 0; i < rows; ++i) {
        const Record& rec = records[i];
        char* row = block + rows_at + size_t{i} * spec.row_size;
        spec.init(row);
        Error err;
        if (!spec.read(*rec.array, row, &err)) {
            const Symbol* wild_tags = wild.data() + found[rec.governed].first_tag;
            err.set_path(record_path(at, wild_tags, rec.array->tag()).view());
            slot.errors.push_back(err);
            if (warn) warn(err);
        }
        r.run(row, spec.fixups);
        Symbol* row_tags = tags + size_t{i} * (wildcards + 1);
        for (uint32_t k = 0; k < wildcards; ++k)
            row_tags[k] = wild[found[rec.governed].first_tag + k];
        row_tags[wildcards] = rec.array->tag();
        new (ids + i) RecordId(rec.array->record_id());
        if (!ids[i].empty() && ids[i].valid()) idx[id_count++] = {ids[i], i};
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

// A section: digest u64, row_size u32, rows u32, fixups u64, block u32, wildcards u32,
// the block's layout as offsets (rows, tags, ids, keys, key_count, idx, id_count, parents,
// parent_count: u32 each), then the names (u32 count; u32 length + bytes, each padded to 8),
// then the block's bytes (padded to 8), then the relocations (u32 count; u32 offset, u32
// kind, u32 value each).
enum class RelocKind : uint32_t { Pointer, String, Curve, Symbol };
struct Reloc {
    uint32_t offset, kind, value;
};

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

// Collects the block's relocations: every pointer into the block and every symbol, as the
// fix-ups and the indices say where they are.
struct Collector {
    const char* base;
    size_t bytes;
    Vec<Reloc>& relocs;
    Vec<Symbol>& names;

    [[nodiscard]] uint32_t name_of(Symbol s) {
        if (s.empty()) return UINT32_MAX;
        for (uint32_t i = 0; i < names.size(); ++i)
            if (names[i] == s) return i;
        names.push_back(s);
        return static_cast<uint32_t>(names.size() - 1);
    }
    [[nodiscard]] uint32_t offset_of(const void* p) const noexcept {
        return static_cast<uint32_t>(static_cast<const char*>(p) - base);
    }
    void symbol(const char* at) {
        Symbol s;
        std::memcpy(&s, at, sizeof(Symbol));
        relocs.push_back({offset_of(at), static_cast<uint32_t>(RelocKind::Symbol), name_of(s)});
    }
    void run(const char* row, std::span<const ViewFixup> fixups) {
        for (const ViewFixup& f : fixups) {
            const char* at = row + f.offset;
            switch (f.kind) {
                case ViewFixupKind::Symbol: symbol(at); break;
                case ViewFixupKind::String: {
                    std::string_view s;
                    std::memcpy(&s, at, sizeof(std::string_view));
                    if (s.data())
                        relocs.push_back({offset_of(at), static_cast<uint32_t>(RelocKind::String),
                                          offset_of(s.data())});
                    break;
                }
                case ViewFixupKind::Curve: {
                    Curve c;
                    std::memcpy(&c, at, sizeof(Curve));
                    if (!c.empty())
                        relocs.push_back(
                            {offset_of(at), static_cast<uint32_t>(RelocKind::Curve), 0});
                    break;
                }
                case ViewFixupKind::Struct: run(at, f.nested_fixups()); break;
                case ViewFixupKind::List: {
                    RawList raw{};
                    std::memcpy(&raw, at, sizeof(RawList));
                    if (!raw.packed || !raw.data) break;
                    relocs.push_back({offset_of(at), static_cast<uint32_t>(RelocKind::Pointer),
                                      offset_of(raw.data)});
                    const auto* elements = static_cast<const char*>(raw.data);
                    for (uint32_t i = 0; i < raw.size; ++i) {
                        const char* e = elements + size_t{i} * f.element_size;
                        switch (f.element) {
                            case ViewFixup::Element::Symbol: symbol(e); break;
                            case ViewFixup::Element::String: {
                                const ViewFixup one{0,
                                                    ViewFixupKind::String,
                                                    ViewFixup::Element::Int32,
                                                    0,
                                                    nullptr,
                                                    0,
                                                    nullptr,
                                                    nullptr};
                                run(e, std::span<const ViewFixup>(&one, 1));
                                break;
                            }
                            case ViewFixup::Element::Curve: {
                                const ViewFixup one{0,
                                                    ViewFixupKind::Curve,
                                                    ViewFixup::Element::Int32,
                                                    0,
                                                    nullptr,
                                                    0,
                                                    nullptr,
                                                    nullptr};
                                run(e, std::span<const ViewFixup>(&one, 1));
                                break;
                            }
                            case ViewFixup::Element::Struct: run(e, f.nested_fixups()); break;
                            default: break;
                        }
                    }
                    break;
                }
            }
        }
    }
};

}  // namespace

namespace detail {

void record_table_sources(TableSlot& slot, const Array& root, const Allocator& alloc) {
    Vec<Governed> found(alloc);
    Vec<Symbol> tags(alloc);
    Walker w{{slot.at.data(), slot.at.size()}, found, tags, Vec<Symbol>(alloc)};
    w.walk(root, 0);
    slot.sources.clear();
    for (const Governed& g : found) slot.sources.push_back(g.parent);
}

Expected<void> write_table(const BakedTable& table, const TableSpec& spec, Str& out) {
    const auto* base = reinterpret_cast<const char*>(&table);
    const BakedTable::Layout::Fields f = BakedTable::Layout::fields(table);
    Vec<Reloc> relocs;
    Vec<Symbol> names;
    Collector c{base, table.block_bytes(), relocs, names};
    // The rows through the fix-ups, with a curve's points found through the reader.
    for (uint32_t i = 0; i < table.size(); ++i)
        c.run(static_cast<const char*>(table.row(i)), spec.fixups);
    for (Reloc& r : relocs)
        if (r.kind == static_cast<uint32_t>(RelocKind::Curve)) {
            Curve curve;
            std::memcpy(&curve, base + r.offset, sizeof(Curve));
            r.value = c.offset_of(CurveReader::points(curve));
        }
    // The indices' symbols.
    const size_t per_row = size_t{table.wildcards()} + 1;
    for (size_t i = 0; i < size_t{table.size()} * per_row; ++i)
        c.symbol(reinterpret_cast<const char*>(f.tags + i));
    for (uint32_t i = 0; i < f.key_count; ++i)
        c.symbol(reinterpret_cast<const char*>(&f.keys[i].key));
    for (uint32_t i = 0; i < f.parent_count; ++i)
        c.symbol(reinterpret_cast<const char*>(&f.parents[i].tag));

    out.clear();
    put(out, spec.digest);
    put(out, spec.row_size);
    put(out, table.size());
    put(out, fixups_hash(spec.fixups));
    put(out, static_cast<uint32_t>(table.block_bytes()));
    put(out, table.wildcards());
    const uint32_t offsets[] = {c.offset_of(table.row(0)),
                                c.offset_of(f.tags),
                                c.offset_of(f.ids),
                                c.offset_of(f.keys),
                                f.key_count,
                                c.offset_of(f.idx),
                                f.id_count,
                                c.offset_of(f.parents),
                                f.parent_count};
    for (const uint32_t o : offsets) put(out, o);
    put(out, static_cast<uint32_t>(names.size()));
    for (const Symbol s : names) {
        put(out, static_cast<uint32_t>(s.str().size()));
        out.append(s.str());
        out.resize(aligned(out.size(), 8));
    }
    out.append(std::string_view(base, table.block_bytes()));
    out.resize(aligned(out.size(), 8));
    put(out, static_cast<uint32_t>(relocs.size()));
    for (const Reloc& r : relocs) {
        put(out, r.offset);
        put(out, r.kind);
        put(out, r.value);
    }
    return {};
}

Ref<const BakedTable> read_table(std::string_view in, const TableSpec& spec, const Allocator& alloc,
                                 Error& why) {
    const auto refuse = [&](std::string_view what) {
        why = make_error(ErrorCode::Refused, what);
        return Ref<const BakedTable>();
    };
    size_t at = 0;
    uint64_t digest = 0, fixups = 0;
    uint32_t row_size = 0, rows = 0, block_bytes = 0, wildcards = 0, offsets[9] = {},
             name_count = 0;
    if (!get(in, at, digest) || !get(in, at, row_size) || !get(in, at, rows) ||
        !get(in, at, fixups) || !get(in, at, block_bytes) || !get(in, at, wildcards))
        return refuse("a damaged table section");
    if (digest != spec.digest) return refuse("another view's table");
    if (row_size != spec.row_size) return refuse("the view's size differs");
    if (fixups != fixups_hash(spec.fixups)) return refuse("the view's fix-up list differs");
    for (uint32_t& o : offsets)
        if (!get(in, at, o)) return refuse("a damaged table section");
    if (!get(in, at, name_count)) return refuse("a damaged table section");
    Vec<Symbol> names(alloc);
    for (uint32_t i = 0; i < name_count; ++i) {
        uint32_t len = 0;
        if (!get(in, at, len) || in.size() - at < len) return refuse("a damaged table section");
        names.emplace_back(in.substr(at, len));
        at = aligned(at + len, 8);
    }
    if (in.size() - at < block_bytes) return refuse("a damaged table section");
    const std::string_view block_bytes_in = in.substr(at, block_bytes);
    at = aligned(at + block_bytes, 8);
    uint32_t reloc_count = 0;
    if (!get(in, at, reloc_count) || (in.size() - at) / sizeof(Reloc) < reloc_count)
        return refuse("a damaged table section");
    const size_t align = std::max<size_t>(spec.row_align, kPoolAlign);
    for (const uint32_t o : offsets)
        if (o > block_bytes) return refuse("a damaged table section");
    char* block = static_cast<char*>(detail::allocate(alloc, block_bytes, align));
    std::memcpy(block, block_bytes_in.data(), block_bytes);
    BakedTable* table = BakedTable::Layout::place(block, block_bytes, alloc);
    BakedTable::Layout::set(
        *table, rows, row_size, wildcards, digest, block + offsets[0],
        reinterpret_cast<const Symbol*>(block + offsets[1]),
        reinterpret_cast<const RecordId*>(block + offsets[2]),
        reinterpret_cast<const BakedTable::KeyEntry*>(block + offsets[3]), offsets[4],
        reinterpret_cast<const BakedTable::IdEntry*>(block + offsets[5]), offsets[6],
        reinterpret_cast<const BakedTable::ParentEntry*>(block + offsets[7]), offsets[8]);
    Ref<const BakedTable> ref = Ref<const BakedTable>::adopt(table);
    for (uint32_t i = 0; i < reloc_count; ++i) {
        Reloc r{};
        if (!get(in, at, r)) return refuse("a damaged table section");
        if (r.offset + 8 > block_bytes) return refuse("a damaged table section");
        char* slot = block + r.offset;
        switch (static_cast<RelocKind>(r.kind)) {
            case RelocKind::Pointer: {
                if (r.value > block_bytes) return refuse("a damaged table section");
                const void* p = block + r.value;
                std::memcpy(slot, &p, sizeof p);
                break;
            }
            case RelocKind::String: {
                std::string_view s;
                std::memcpy(&s, slot, sizeof(std::string_view));
                if (r.value > block_bytes) return refuse("a damaged table section");
                const std::string_view moved(block + r.value, s.size());
                std::memcpy(slot, &moved, sizeof(std::string_view));
                break;
            }
            case RelocKind::Curve: {
                Curve curve;
                std::memcpy(&curve, slot, sizeof(Curve));
                if (r.value > block_bytes) return refuse("a damaged table section");
                CurveReader::set_points(curve,
                                        reinterpret_cast<const CurvePoint*>(block + r.value));
                std::memcpy(slot, &curve, sizeof(Curve));
                break;
            }
            case RelocKind::Symbol: {
                const Symbol s = r.value == UINT32_MAX    ? Symbol()
                                 : r.value < names.size() ? names[r.value]
                                                          : Symbol();
                if (r.value != UINT32_MAX && r.value >= names.size())
                    return refuse("a damaged table section");
                std::memcpy(slot, &s, sizeof(Symbol));
                break;
            }
            default: return refuse("a damaged table section");
        }
    }
    return ref;
}

}  // namespace detail
}  // namespace baker
