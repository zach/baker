// The binary cache. The writer lays a tree out as the loader will use it:
// the file, after a block header, is the tree's arena block, so loading is one read into
// one allocation, then one pass that turns offsets into pointers.

#include <baker/cache.hpp>

#include "arena.hpp"
#include "common/containers.hpp"
#include "session.hpp"
#include "text.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace baker {
namespace detail {

// What the format relies on in the layouts of Node and Array.
struct CacheLayout {
    static constexpr size_t kValue = offsetof(Node, value_);
    static constexpr size_t kType = offsetof(Node, type_);
    static constexpr size_t kSize = offsetof(Array, size_);
    static constexpr size_t kFile = offsetof(Array, file_);
    static constexpr size_t kBlock = offsetof(Array, block_);
    static constexpr size_t kLine = offsetof(Array, line_);
    static const Node* slots(const Array& a) noexcept { return a.data(); }
    static uint32_t tag_key(const Node& n) noexcept { return Array::tag_key(n); }
};

namespace {

using L = CacheLayout;
static_assert(L::kValue == 0 && L::kType == 8 && L::kBlock == 0 && L::kSize == 4 && L::kFile == 8 &&
              L::kLine == 16);
static_assert(sizeof(String) == 8 && sizeof(Symbol) == 8);

constexpr char kMagic[10] = {'M', 'O', 'I', 'R', 'A', '.', 'D', 'A', 'T', 'B'};
// 2: packs' harness metadata, origins; 3: the header hashed; 4: '' keys no tag
// in the tag run; 5: packs' versions, and the versions they require; 6: the
// source-order layout, for delta patches.
constexpr uint16_t kVersion = 6;
constexpr uint32_t kEndian = 0x01020304;

// The layout is made for delta patches: an edit to the text changes the bytes of the
// cache near the edit, and few others, so a binary diff or a chunked store sends little more
// than the edit. Nothing in the file holds an absolute offset, and no index depends on
// what came before it.
//
// The records follow the header in source order: each array is its memory image (header,
// tag run, nodes), followed by what its nodes hold, in order. A string goes inline as
// [block 0][size][bytes, 0, padded to 8]; a child array goes inline as its own record and
// what follows it. The loader walks the records in the same order and makes each array or
// string node point at the next record, so those nodes hold 0 in the file. A node whose
// target was written before, which a tree shares, holds the distance back to it from the
// point the walk has reached instead. A symbol or var node holds the hash of its name, an
// array's file field the hash of the file's, and its line field the difference from the line
// of the array before it; the block fields and the tag run hold 0. The loader fills them all.
//
// The symbols follow the records: a table of 16-byte slots (the hash; 0, which the loader
// sets to the interned Symbol), and then the name of each used slot, in slot order, as a
// 32-bit length and its bytes. A name's hash has its low bit set, so 0 marks a free slot. A
// name's home slot is its hash's bits above the low one, modulo the table's 2^home_bits
// home slots; each name lies at its home or after it, the table sorted by home slot and
// then by hash, so it is the same for the same names whatever order they came in, and one
// more name moves only the names of its run. Past the home slots, the table runs on as far
// as the last run needs.
//
// A cache written from a recorded load has a second tree, `meta`, after the first:
// the files the load read, the defines it began with, and its effects on the session.
//   (files ("path" size hash) ...)     size -1: looked for and absent; -2: a directory
//                                      listed, hash its listing's; -3: one not listed
//   (before NAME hash ...)             0 for a flag, else the hash of the macro's text
//   (define NAME body-or-0 ...) (undefine NAME ...) (autorun {command} ...)
//   (required "path" ...)              files #require had read when the load began
//   (require "path" ...)               files the load #required
//   (levels L ...)                     the session's pack levels
//   (packs (name level file (requires name-or-(name "version") ...) (owners ...) (for ...)
//           stage-or-0 version-or-0) ...)
//                                      packs the load declared
//   (macro_packs NAME file ...)        the pack each macro the load defined came from
//   (origins (file "source" tool-or-0) ...)   origins the load's files declared
//   (definitions (NAME file line flag) ...)   where the load defined each macro
struct Header {
    char magic[10];
    uint16_t version;
    uint32_t endian;
    uint8_t pointer_bytes;
    uint8_t flags;
    uint16_t reserved;
    uint64_t hash;  // of the whole file, this field read as 0 (file_hash)
    uint32_t file_bytes;
    uint32_t root;  // the first record, just past the header
    uint32_t meta;  // 0: none
    uint32_t symbols;
    uint32_t symbol_slots;
    uint32_t symbol_count;
    uint32_t home_bits;
    uint32_t reserved2;
};
static_assert(sizeof(Header) == 64);
constexpr uint8_t kEnciphered = 1;

constexpr uint32_t round8(size_t n) noexcept {
    return static_cast<uint32_t>((n + 7) & ~size_t{7});
}
size_t array_bytes(uint32_t size) noexcept {
    return Array::bytes(size);
}
size_t string_bytes(uint32_t size) noexcept {
    return round8(sizeof(String) + size_t{size} + 1);
}
bool holds_target(Type t) noexcept {
    return t == Type::String || t == Type::Array || t == Type::Command || t == Type::Property;
}

// A checksum, not a cipher: four lanes of multiply and rotate, near memory speed.
uint64_t hash_bytes(const char* p, size_t n) noexcept {
    constexpr uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t h[4] = {k, k + 1, k + 2, k + 3};
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int l = 0; l < 4; ++l) {
            uint64_t w;
            std::memcpy(&w, p + i + 8 * static_cast<size_t>(l), 8);
            h[l] = std::rotl((h[l] ^ w) * k, 29);
        }
    // The length goes in after the bytes: started from it, a first byte equal to it zeroed
    // the state, so "" and "\x01" hashed alike (found by the fuzzer, as two symbols).
    uint64_t r = k;
    for (; i < n; ++i) r = (r ^ static_cast<unsigned char>(p[i])) * k;
    r = std::rotl((r ^ n) * k, 31);
    for (const uint64_t x : h) r = std::rotl((r ^ x) * k, 31);
    return r ^ (r >> 32);
}
uint64_t name_hash(std::string_view name) noexcept {
    return hash_bytes(name.data(), name.size()) | 1;
}
size_t home_of(uint64_t hash, uint32_t home_bits) noexcept {
    return static_cast<size_t>((hash >> 1) & ((uint64_t{1} << home_bits) - 1));
}

// The header's bytes as they lie in the file, with its hash field zeroed, and the rest.
uint64_t file_hash(const char* file, size_t bytes) noexcept {
    char header[sizeof(Header)];
    std::memcpy(header, file, sizeof header);
    std::memset(header + offsetof(Header, hash), 0, sizeof(uint64_t));
    return std::rotl(hash_bytes(header, sizeof header), 1) ^
           hash_bytes(file + sizeof header, bytes - sizeof header);
}

template <class T>
void put(Str& out, size_t at, const T& v) noexcept {
    std::memcpy(out.data() + at, &v, sizeof v);
}

// The offset each array and string of a tree was written at, by address: open addressing
// over a power-of-two table, with no allocation per entry.
class Seen {
public:
    static constexpr uint32_t kNone = UINT32_MAX;
    // The offset of `p`, or kNone after giving it `at`.
    uint32_t find_or_add(const void* p, uint32_t at) {
        if ((count_ + 1) * 2 > keys_.size()) grow();
        size_t i = slot(p);
        for (; keys_[i]; i = (i + 1) & (keys_.size() - 1))
            if (keys_[i] == p) return values_[i];
        keys_[i] = p;
        values_[i] = at;
        ++count_;
        return kNone;
    }

private:
    size_t slot(const void* p) const noexcept {
        const uint64_t h = reinterpret_cast<uintptr_t>(p) * 0x9E3779B97F4A7C15ull;
        return static_cast<size_t>(h >> 32) & (keys_.size() - 1);
    }
    void grow() {
        Vec<const void*> keys = std::move(keys_);
        Vec<uint32_t> values = std::move(values_);
        keys_.assign(keys.empty() ? 1024 : keys.size() * 2, nullptr);
        values_.resize(keys_.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            if (!keys[i]) continue;
            size_t j = slot(keys[i]);
            while (keys_[j]) j = (j + 1) & (keys_.size() - 1);
            keys_[j] = keys[i];
            values_[j] = values[i];
        }
    }
    Vec<const void*> keys_;
    Vec<uint32_t> values_;
    size_t count_ = 0;
};

class Writer {
public:
    Expected<void> run(const Array& root, const Array* meta, const Cipher* cipher, Str& out) {
        out.clear();
        out.resize(sizeof(Header));
        if (auto r = tree(root, out); !r) return r;
        const auto meta_at = static_cast<uint32_t>(meta ? out.size() : 0);
        if (meta)
            if (auto r = tree(*meta, out); !r) return r;
        const auto symbols = static_cast<uint32_t>(out.size());

        // The symbol table, sorted by home slot and then by hash; each name at its home or
        // after the run before it.
        uint32_t home_bits = 3;
        while ((size_t{1} << home_bits) < symbols_.size() * 2) ++home_bits;
        std::sort(symbols_.begin(), symbols_.end(), [&](const Name& a, const Name& b) {
            const size_t ha = home_of(a.hash, home_bits), hb = home_of(b.hash, home_bits);
            return ha != hb ? ha < hb : a.hash < b.hash;
        });
        size_t slots = 0;
        for (size_t i = 0; i < symbols_.size(); ++i) {
            if (i && symbols_[i].hash == symbols_[i - 1].hash)
                return Unexpected(make_error(ErrorCode::Refused,
                                             "the symbols '" + Str(symbols_[i - 1].sym.str()) +
                                                 "' and '" + Str(symbols_[i].sym.str()) +
                                                 "' hash alike; rename one"));
            slots = std::max(slots, home_of(symbols_[i].hash, home_bits)) + 1;
        }
        slots = std::max(slots, size_t{1} << home_bits);
        out.resize(symbols + slots * 16);
        size_t next = 0;
        for (const Name& n : symbols_) {
            next = std::max(next, home_of(n.hash, home_bits));
            put(out, symbols + next++ * 16, n.hash);
        }
        for (const Name& n : symbols_) {
            const auto size = static_cast<uint32_t>(n.sym.str().size());
            out.append(std::string_view(reinterpret_cast<const char*>(&size), 4));
            out.append(n.sym.str());
        }
        out.resize(round8(out.size()));
        if (out.size() > UINT32_MAX) return limit();

        Header h{};
        std::memcpy(h.magic, kMagic, sizeof kMagic);
        h.version = kVersion;
        h.endian = kEndian;
        h.pointer_bytes = sizeof(void*);
        h.file_bytes = static_cast<uint32_t>(out.size());
        h.root = sizeof(Header);
        h.meta = meta_at;
        h.symbols = symbols;
        h.symbol_slots = static_cast<uint32_t>(slots);
        h.symbol_count = static_cast<uint32_t>(symbols_.size());
        h.home_bits = home_bits;
        if (cipher) {
            h.flags = kEnciphered;
            cipher->encrypt(cipher->user,
                            {reinterpret_cast<std::byte*>(out.data()) + sizeof(Header),
                             out.size() - sizeof(Header)});
        }
        put(out, 0, h);
        put(out, offsetof(Header, hash), file_hash(out.data(), out.size()));
        return {};
    }

private:
    static constexpr uint32_t kNoSymbol = UINT32_MAX;
    struct Frame {
        const Array* array;
        size_t at;
        uint32_t next;
    };
    struct Name {
        Symbol sym;
        uint64_t hash;
    };

    static Unexpected limit() {
        return Unexpected(make_error(ErrorCode::Limit, "a cache holds at most 4 GB"));
    }
    // The records of the tree from `root`, in source order.
    Expected<void> tree(const Array& root, Str& out) {
        if (auto r = record(root, out); !r) return r;
        while (!stack_.empty()) {
            Frame& f = stack_.back();
            if (f.next == f.array->size()) {
                stack_.pop_back();
                continue;
            }
            const uint32_t i = f.next++;
            const Node& n = L::slots(*f.array)[i];
            const void* target = n.array();
            if (!target) target = n.string();
            if (!target) continue;
            const size_t node = f.at + Array::nodes_offset(f.array->size()) + size_t{i} * 16;
            if (out.size() >= UINT32_MAX) return limit();
            const uint32_t known = seen_.find_or_add(target, static_cast<uint32_t>(out.size()));
            if (known != Seen::kNone) {
                put(out, node, uint64_t{out.size() - known});
            } else if (const String* s = n.string()) {
                const size_t at = out.size();
                out.resize(at + string_bytes(s->size()));
                put(out, at + 4, s->size());
                std::memcpy(out.data() + at + sizeof(String), s->data(), s->size());
            } else if (auto r = record(*n.array(), out); !r) {  // invalidates f
                return r;
            }
        }
        return {};
    }
    // The record of `a`; its targets follow it, from tree().
    Expected<void> record(const Array& a, Str& out) {
        const size_t at = out.size();
        if (at + array_bytes(a.size()) > UINT32_MAX) return limit();
        seen_.find_or_add(&a, static_cast<uint32_t>(at));
        out.resize(at + array_bytes(a.size()));
        std::memcpy(out.data() + at, &a, sizeof(Array));
        put(out, at + L::kBlock, uint32_t{0});
        put(out, at + L::kFile, symbol(a.file()));
        put(out, at + L::kLine, static_cast<uint32_t>(a.line()) - line_);
        line_ = static_cast<uint32_t>(a.line());
        const Node* nodes = L::slots(a);
        char* slots = out.data() + at + Array::nodes_offset(a.size());
        // An empty array's one slot holds Int 0, which needs nothing more.
        for (uint32_t i = 0; i < (a.size() ? a.size() : 1); ++i) {
            const Node& n = nodes[i];
            char* p = slots + size_t{i} * sizeof(Node);
            // The payload and type bytes, into the file; the padding stays 0. Nothing becomes a
            // Node.
            // NOLINTNEXTLINE(bugprone-undefined-memory-manipulation)
            std::memcpy(p, &n, L::kType + 1);
            uint64_t v = 0;
            switch (n.type()) {
                case Type::Int:
                case Type::Float: continue;
                case Type::Symbol: v = symbol(*n.try_sym()); break;
                case Type::Var: v = symbol(n.var_name()); break;
                case Type::String:
                case Type::Array:
                case Type::Command:
                case Type::Property: break;
                default:
                    return Unexpected(make_error(ErrorCode::Refused,
                                                 "a cache holds data, and " + preview(n) +
                                                     " is a " + Str(type_name(n.type())),
                                                 a.file(), a.line()));
            }
            std::memcpy(p, &v, sizeof v);
        }
        stack_.push_back({&a, at, 0});
        return {};
    }
    uint64_t symbol(Symbol s) {
        if (s.index() >= symbol_index_.size()) symbol_index_.resize(s.index() + 1, kNoSymbol);
        uint32_t& i = symbol_index_[s.index()];
        if (i == kNoSymbol) {
            i = static_cast<uint32_t>(symbols_.size());
            symbols_.push_back({s, name_hash(s.str())});
        }
        return symbols_[i].hash;
    }

    Seen seen_;
    Vec<Frame> stack_;
    uint32_t line_ = 0;  // of the record before
    Vec<Name> symbols_;
    Vec<uint32_t> symbol_index_;  // by Symbol::index()
};

Unexpected bad(std::string_view name, std::string_view why, ErrorCode code = ErrorCode::Io) {
    return Unexpected(make_error(code, "'" + Str(name) + "' " + Str(why)));
}

const Allocator& allocator_of(const LoadSession& s) noexcept {
    return s.allocator ? *s.allocator : default_allocator();
}

// The arrays a load is inside, deepest last: on the stack to the parser's default depth
// limit, and from the session's allocator past it.
class LoadStack {
public:
    struct Frame {
        char* array;
        uint32_t size;
        uint32_t next;
    };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init): a frame is written as it is pushed
    explicit LoadStack(const Allocator& a) noexcept : alloc_(a), data_(inline_) {}
    LoadStack(const LoadStack&) = delete;
    LoadStack& operator=(const LoadStack&) = delete;
    ~LoadStack() {
        if (data_ != inline_) detail::deallocate(alloc_, data_, cap_ * sizeof(Frame));
    }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    Frame& back() noexcept { return data_[size_ - 1]; }
    void pop() noexcept { --size_; }
    void push(Frame f) {
        if (size_ == cap_) grow();
        data_[size_++] = f;
    }

private:
    static constexpr size_t kInline = 256;
    void grow() {
        auto* bigger =
            static_cast<Frame*>(detail::allocate(alloc_, cap_ * 2 * sizeof(Frame), alignof(Frame)));
        std::memcpy(bigger, data_, size_ * sizeof(Frame));
        if (data_ != inline_) detail::deallocate(alloc_, data_, cap_ * sizeof(Frame));
        data_ = bigger;
        cap_ *= 2;
    }
    const Allocator& alloc_;
    Frame inline_[kInline];
    Frame* data_;
    size_t size_ = 0, cap_ = kInline;
};

// Checks the cache at `file` and fixes it up in place, in the arena that holds it. `meta` is
// set to the cache's record of its load, or null.
Expected<Ref<Array>> load(char* file, size_t bytes, std::string_view name, const Cipher* cipher,
                          const Allocator& alloc, const Array*& meta) {
    Header h{};
    if (bytes < sizeof h) return bad(name, "is not a Baker cache");
    std::memcpy(&h, file, sizeof h);
    if (std::memcmp(h.magic, kMagic, sizeof kMagic) != 0) return bad(name, "is not a Baker cache");
    if (h.version != kVersion)
        return bad(name,
                   "is a cache of format version " + decimal(h.version) +
                       "; this build reads version " + decimal(kVersion),
                   ErrorCode::Refused);
    if (h.endian != kEndian || h.pointer_bytes != sizeof(void*))
        return bad(name, "was written on a platform of another endianness or pointer size",
                   ErrorCode::Refused);
    if ((h.flags & kEnciphered) && !cipher)
        return bad(name, "is enciphered, and the session has no Cipher", ErrorCode::Refused);
    const bool fits = h.file_bytes == bytes && h.root == sizeof h && h.root < h.symbols &&
                      (h.meta == 0 || (h.meta > h.root && h.meta < h.symbols)) &&
                      (h.flags & ~kEnciphered) == 0 && h.symbols % 8 == 0 && h.symbols <= bytes &&
                      h.symbol_slots <= (bytes - h.symbols) / 16 && h.home_bits < 32 &&
                      (size_t{1} << h.home_bits) <= h.symbol_slots;
    if (!fits || h.hash != file_hash(file, bytes)) return bad(name, "is damaged");
    if (h.flags & kEnciphered)
        cipher->decrypt(cipher->user,
                        {reinterpret_cast<std::byte*>(file) + sizeof h, bytes - sizeof h});

    // Intern the symbols into their slots.
    char* slots = file + h.symbols;
    const char* slots_end = slots + size_t{h.symbol_slots} * 16;
    size_t p = h.symbols + size_t{h.symbol_slots} * 16;
    uint32_t count = 0;
    for (char* s = slots; s < slots_end; s += 16) {
        uint64_t hash;
        std::memcpy(&hash, s, 8);
        if (!hash) continue;
        uint32_t size;
        if (bytes - p < 4) return bad(name, "is damaged");
        std::memcpy(&size, file + p, 4);
        p += 4;
        if (bytes - p < size) return bad(name, "is damaged");
        const void* id = Symbol(std::string_view(file + p, size)).id();
        std::memcpy(s + 8, &id, 8);
        p += size;
        ++count;
    }
    if (count != h.symbol_count) return bad(name, "is damaged");

    // A symbol node's or file field's hash becomes the Symbol, found from its home slot.
    const uint64_t home_mask = (uint64_t{1} << h.home_bits) - 1;
    auto symbol = [slots, slots_end, home_mask](char* at) {
        uint64_t hash;
        std::memcpy(&hash, at, 8);
        if (!(hash & 1)) return false;
        for (const char* s = slots + ((hash >> 1) & home_mask) * 16; s < slots_end; s += 16) {
            uint64_t k;
            std::memcpy(&k, s, 8);
            if (k == hash) {
                std::memcpy(at, s + 8, 8);
                return true;
            }
            if (!k) return false;
        }
        return false;
    };
    // The records, walked in the order they were written.
    const size_t records_end = h.symbols;
    size_t at = sizeof h;
    uint32_t line = 0;
    auto record = [&]() -> char* {
        if (records_end - at < sizeof(Array)) return nullptr;
        char* a = file + at;
        uint32_t size, delta;
        std::memcpy(&size, a + L::kSize, 4);
        if (records_end - at < array_bytes(size) || !symbol(a + L::kFile)) return nullptr;
        const auto block = static_cast<uint32_t>(Arena::kBlockHeader + at);
        std::memcpy(a + L::kBlock, &block, 4);
        std::memcpy(&delta, a + L::kLine, 4);
        line += delta;
        std::memcpy(a + L::kLine, &line, 4);
        // The first node now, so that the parent can take the tag once this is placed; the
        // rest as the walk reaches them. An empty array's one slot holds Int 0.
        char* first = a + Array::nodes_offset(size);
        const auto t = static_cast<Type>(first[L::kType]);
        if ((t == Type::Symbol || t == Type::Var) ? !symbol(first) : size == 0 && t != Type::Int)
            return nullptr;
        at += array_bytes(size);
        return a;
    };
    LoadStack stack(alloc);
    auto tree = [&](uint32_t start) -> char* {
        if (at != start) return nullptr;
        char* root = record();
        if (!root) return nullptr;
        uint32_t root_size;
        std::memcpy(&root_size, root + L::kSize, 4);
        stack.push({root, root_size, 0});
        while (!stack.empty()) {
            LoadStack::Frame& f = stack.back();
            char* parent = f.array;
            char* nodes = parent + Array::nodes_offset(f.size);
            char* child = nullptr;
            uint32_t i = f.next;
            while (i < f.size) {
                char* n = nodes + size_t{i} * sizeof(Node);
                const auto t = static_cast<Type>(n[L::kType]);
                if (t == Type::Int || t == Type::Float) {
                    ++i;
                    continue;
                }
                if (t == Type::Symbol || t == Type::Var) {
                    if (i && !symbol(n)) return nullptr;  // record() did the first
                    ++i;
                    continue;
                }
                if (!holds_target(t)) return nullptr;
                uint64_t back;
                std::memcpy(&back, n, 8);
                char* target;
                if (back) {  // shared: written before, and whole before the walk's point
                    if (back > at - sizeof h || back % 8 != 0) return nullptr;
                    target = file + (at - back);
                    uint32_t size;
                    std::memcpy(&size, target + 4, 4);
                    const size_t span = t == Type::String ? string_bytes(size) : array_bytes(size);
                    if (back < span) return nullptr;
                } else if (t == Type::String) {
                    if (records_end - at < sizeof(String)) return nullptr;
                    target = file + at;
                    uint32_t size;
                    std::memcpy(&size, target + 4, 4);
                    if (records_end - at < string_bytes(size)) return nullptr;
                    const auto block = static_cast<uint32_t>(Arena::kBlockHeader + at);
                    std::memcpy(target, &block, 4);
                    at += string_bytes(size);
                } else {
                    target = child = record();
                    if (!target) return nullptr;
                }
                std::memcpy(n, &target, 8);
                if (t != Type::String) {  // the parent's tag run, now the child's tag is set
                    const uint32_t key = L::tag_key(*reinterpret_cast<const Node*>(n));
                    std::memcpy(parent + sizeof(Array) + size_t{i} * 4, &key, 4);
                }
                ++i;
                if (child) break;
            }
            if (!child) {
                stack.pop();
                continue;
            }
            f.next = i;
            uint32_t size;
            std::memcpy(&size, child + L::kSize, 4);
            stack.push({child, size, 0});  // invalidates f
        }
        return root;
    };
    char* root = tree(sizeof h);
    char* meta_root = h.meta ? tree(h.meta) : nullptr;
    if (!root || (h.meta && !meta_root) || at != records_end) return bad(name, "is damaged");
    meta = reinterpret_cast<const Array*>(meta_root);
    return Ref<Array>(reinterpret_cast<Array*>(root));
}

// --- The record of a load. ---

// A macro body's hash, from its nodes: the same tree hashes the same in any process, and
// nothing is printed or allocated. The text's hash cost a freshness check about half a
// microsecond a define.
uint64_t node_hash(const Node& n, uint64_t h) noexcept {
    constexpr uint64_t k = 0x9E3779B97F4A7C15ull;
    const auto mix = [&](uint64_t v) { h = std::rotl((h ^ v) * k, 27); };
    mix(static_cast<uint64_t>(n.type()) + 1);
    switch (n.type()) {
        case Type::Int: mix(static_cast<uint64_t>(*n.try_int())); break;
        case Type::Float: mix(n.float_bits()); break;
        case Type::Symbol: mix(n.try_sym()->hash()); break;
        case Type::Var: mix(n.var_name().hash()); break;
        case Type::String: mix(content_hash(n.string()->view())); break;
        case Type::Array:
        case Type::Command:
        case Type::Property:
            mix(n.array()->size());
            for (const Node& c : *n.array()) h = node_hash(c, h);
            break;
        default: break;  // a cache holds data only
    }
    return h;
}

uint64_t body_hash(const Ref<Array>& body) {
    return body ? node_hash(Node(body), 1) : 0;
}

Node list(Vec<Node>& nodes) {
    return Node(Array::make_from({nodes.data(), nodes.size()}));
}

// The meta array for a load the session recorded, or null.
Ref<Array> record_of(const LoadSession& s) {
    using A = SessionAccess;
    if (!A::recorded(s)) return nullptr;
    Vec<Node> files{Node(Symbol("files"))}, before{Node(Symbol("before"))},
        define{Node(Symbol("define"))}, undefine{Node(Symbol("undefine"))},
        autorun{Node(Symbol("autorun"))}, required{Node(Symbol("required"))},
        require{Node(Symbol("require"))}, levels{Node(Symbol("levels"))},
        packs{Node(Symbol("packs"))}, macro_packs{Node(Symbol("macro_packs"))},
        origins{Node(Symbol("origins"))}, definitions{Node(Symbol("definitions"))};
    for (const CacheFile& f : A::files(s)) {
        Vec<Node> entry{Node(String::make(f.path)), Node(f.size), Node(f.hash)};
        files.push_back(list(entry));
    }
    const A::Defines &was = A::defines_before(s), &now = A::defines(s);
    for (const auto& [name, body] : was) {
        before.emplace_back(name);
        before.emplace_back(body_hash(body));
        if (!now.count(name)) undefine.emplace_back(name);
    }
    for (const auto& [name, body] : now) {
        const auto it = was.find(name);
        if (it != was.end() && it->second == body) continue;
        define.emplace_back(name);
        define.push_back(body ? Node(body) : Node(0));
        if (const Pack* p = A::macro_pack(s, name)) {
            macro_packs.emplace_back(name);
            macro_packs.emplace_back(p->file);
        }
    }
    for (const Node& n : A::new_autorun(s)) autorun.push_back(n);
    const auto paths = A::required(s);
    for (size_t i = 0; i < paths.size(); ++i)
        (i < A::required_before(s) ? required : require).emplace_back(String::make(paths[i]));
    for (const Symbol l : A::pack_levels(s)) levels.emplace_back(l);
    for (const Pack& p : A::new_packs(s)) {
        Vec<Node> deps{Node(Symbol("requires"))};
        for (size_t i = 0; i < p.requires_packs.size(); ++i) {
            const Symbol min = i < p.requires_versions.size() ? p.requires_versions[i] : Symbol();
            if (min.empty()) {
                deps.emplace_back(p.requires_packs[i]);
            } else {
                Vec<Node> versioned{Node(p.requires_packs[i]), Node(String::make(min.str()))};
                deps.push_back(list(versioned));
            }
        }
        Vec<Node> owners{Node(Symbol("owners"))}, audience{Node(Symbol("for"))};
        for (const Symbol o : p.owners) owners.emplace_back(o);
        for (const Symbol a : p.audience) audience.emplace_back(a);
        Vec<Node> entry{Node(p.name),
                        Node(p.level),
                        Node(p.file),
                        list(deps),
                        list(owners),
                        list(audience),
                        p.stage.empty() ? Node(0) : Node(p.stage),
                        p.version.empty() ? Node(0) : Node(String::make(p.version.str()))};
        packs.push_back(list(entry));
    }
    for (const Origin& o : A::new_origins(s)) {
        Vec<Node> entry{Node(o.file), Node(String::make(o.source.str())),
                        o.tool.empty() ? Node(0) : Node(o.tool)};
        origins.push_back(list(entry));
    }
    for (const Definition& d : A::new_definitions(s)) {
        Vec<Node> entry{Node(d.name), Node(d.file), Node(int64_t{d.line}),
                        Node(int64_t{d.flag ? 1 : 0})};
        definitions.push_back(list(entry));
    }
    Vec<Node> meta{list(files),   list(before),      list(define),  list(undefine),
                   list(autorun), list(required),    list(require), list(levels),
                   list(packs),   list(macro_packs), list(origins), list(definitions)};
    return Array::make_from({meta.data(), meta.size()});
}

// Whether the session is as the recorded load began: its defines and macros, its pack
// levels, and the files #require had read. No file is read, so shipping checks it too.
bool begins_alike(const Array& meta, LoadSession& s) {
    const Array* before = meta.try_find("before");
    if (!before) return false;
    // The files #require had read decide what a #require splices.
    const Array* required = meta.try_find("required");
    const auto paths = SessionAccess::required(s);
    if (!required || required->size() - 1 != paths.size()) return false;
    for (size_t i = 0; i < paths.size(); ++i) {
        const String* p = (*required)[static_cast<uint32_t>(i + 1)].string();
        if (!p || p->view() != paths[i].view()) return false;
    }
    // The pack levels decide which packs load and what they warn about.
    const Array* levels = meta.try_find("levels");
    const auto now_levels = SessionAccess::pack_levels(s);
    if (!levels || levels->size() - 1 != now_levels.size()) return false;
    for (size_t i = 0; i < now_levels.size(); ++i)
        if (!(*levels)[static_cast<uint32_t>(i + 1)].is(now_levels[i])) return false;
    const SessionAccess::Defines& now = SessionAccess::defines(s);
    if (before->size() % 2 == 0 || (before->size() - 1) / 2 != now.size()) return false;
    for (uint32_t i = 1; i + 1 < before->size(); i += 2) {
        const auto it = now.find((*before)[i].try_sym().value_or(Symbol()));
        if (it == now.end() ||
            body_hash(it->second) != static_cast<uint64_t>((*before)[i + 1].try_int().value_or(0)))
            return false;
    }
    return true;
}

// Whether the session begins alike and the files the load read are unchanged: the session
// first, which reads no file.
bool fresh(const Array& meta, LoadSession& s) {
    const Array* files = meta.try_find("files");
    if (!files || !begins_alike(meta, s)) return false;
    Str text;
    Vec<Str> entries;
    for (const Node& n : files->values()) {
        const Array* f = n.array();
        if (!f || f->size() != 3 || !f->nodes()[0].string()) return false;
        const int64_t size = f->nodes()[1].try_int().value_or(-1);
        const auto hash = static_cast<uint64_t>(f->nodes()[2].try_int().value_or(0));
        if (size == kListedDirectory || size == kUnlistedDirectory) {
            // A directory #include_dir listed: a file added, removed or renamed.
            if (list_with(s.lister(), f->nodes()[0].string()->view(), entries) !=
                (size == kListedDirectory ? hash : 0))
                return false;
            continue;
        }
        text.clear();
        const bool read = s.reader()(f->nodes()[0].string()->view(), str_out(text));
        if (size < 0
                ? read
                : !read || static_cast<int64_t>(text.size()) != size || content_hash(text) != hash)
            return false;
    }
    return true;
}

// Leaves the session as the recorded load left it.
void replay(const Array& meta, LoadSession& s) {
    const size_t packs_before = s.packs().size();
    if (const Array* d = meta.try_find("define"))
        for (uint32_t i = 1; i + 1 < d->size(); i += 2) {
            const Symbol name = (*d)[i].try_sym().value_or(Symbol());
            if (const Array* body = (*d)[i + 1].array())
                s.define(name, Ref<Array>(const_cast<Array*>(body)));
            else
                s.define(name);
        }
    if (const Array* u = meta.try_find("undefine"))
        for (const Node& n : u->values())
            if (const auto name = n.try_sym()) s.undefine(*name);
    if (const Array* a = meta.try_find("autorun"))
        for (const Node& n : a->values()) SessionAccess::add_autorun(s, n);
    if (const Array* ps = meta.try_find("packs"))
        for (const Node& n : ps->values()) {
            const Array* p = n.array();
            if (!p || p->size() != 8) continue;
            Vec<Symbol> lists[3], at_least;  // requires, owners, for; requires' versions
            for (uint32_t k = 0; k < 3; ++k)
                if (const Array* d = (*p)[3 + k].array())
                    for (const Node& x : d->values()) {
                        if (const auto sym = x.try_sym()) {
                            lists[k].push_back(*sym);
                            if (k == 0) at_least.emplace_back();
                        } else if (const Array* v = x.array(); k == 0 && v && v->size() == 2) {
                            lists[0].push_back(v->tag());
                            at_least.emplace_back((*v)[1].try_str().value_or(""));
                        }
                    }
            const String* version = (*p)[7].string();
            const Pack pack{(*p)[0].try_sym().value_or(Symbol()),
                            (*p)[1].try_sym().value_or(Symbol()),
                            0,
                            (*p)[2].try_sym().value_or(Symbol()),
                            List<Symbol>({lists[0].data(), lists[0].size()}),
                            List<Symbol>({at_least.data(), at_least.size()}),
                            version ? Symbol(version->view()) : Symbol(),
                            List<Symbol>({lists[1].data(), lists[1].size()}),
                            List<Symbol>({lists[2].data(), lists[2].size()}),
                            (*p)[6].try_sym().value_or(Symbol())};
            SessionAccess::add_pack(s, pack);
        }
    // Once every recorded pack is back, as the read checked once each file was read.
    for (const Pack& pack : s.packs().subspan(packs_before))
        SessionAccess::warn_missing_requires(s, pack, 1);  // a cache keeps no lines
    if (const Array* os = meta.try_find("origins"))
        for (const Node& n : os->values()) {
            const Array* o = n.array();
            if (!o || o->size() != 3 || !(*o)[1].string()) continue;
            SessionAccess::add_origin(
                s, Origin{(*o)[0].try_sym().value_or(Symbol()), Symbol((*o)[1].string()->view()),
                          (*o)[2].try_sym().value_or(Symbol())});
        }
    if (const Array* ds = meta.try_find("definitions"))
        for (const Node& n : ds->values()) {
            const Array* d = n.array();
            if (!d || d->size() != 4) continue;
            SessionAccess::add_definition(
                s, Definition{(*d)[0].try_sym().value_or(Symbol()),
                              (*d)[1].try_sym().value_or(Symbol()),
                              static_cast<int32_t>((*d)[2].try_int().value_or(0)),
                              (*d)[3].try_int().value_or(0) != 0});
        }
    if (const Array* m = meta.try_find("macro_packs"))
        for (uint32_t i = 1; i + 1 < m->size(); i += 2)
            SessionAccess::set_macro_pack(s, (*m)[i].try_sym().value_or(Symbol()),
                                          (*m)[i + 1].try_sym().value_or(Symbol()));
    if (const Array* r = meta.try_find("require"))
        for (const Node& n : r->values())
            if (const String* p = n.string()) SessionAccess::add_required(s, p->view());
}

Expected<void> bytes_of(const Array& root, const LoadSession* session, Str& out) {
    Ref<Array> meta = session ? record_of(*session) : nullptr;
    return Writer().run(root, meta.get(), session ? session->cipher : nullptr, out);
}

// A text-only session takes no cache.
Unexpected refused_text_only(std::string_view name) {
    return Unexpected(make_error(ErrorCode::Refused,
                                 "'" + Str(name) + "' is a cache, and this session reads text only",
                                 Symbol(name)));
}

Expected<Ref<Array>> from_bytes(std::string_view bytes, std::string_view name,
                                const LoadSession& session, const Array*& meta) {
    if (session.text_only) return refused_text_only(name);
    char* file;
    const Arena::Handle arena(Arena::create_holding(allocator_of(session), bytes.size(), file));
    if (!bytes.empty()) std::memcpy(file, bytes.data(), bytes.size());
    return load(file, bytes.size(), name, session.cipher, allocator_of(session), meta);
}

// Loads the cache at `path` without replaying it.
Expected<Ref<Array>> open(std::string_view path, const LoadSession& session, const Array*& meta,
                          bool from_disk = false) {
    if (session.text_only) return refused_text_only(path);
    auto cannot = [&] {
        return Unexpected(make_error(ErrorCode::Io, "cannot read '" + Str(path) + "'"));
    };
    if (!from_disk && !SessionAccess::reads_disk(session)) {
        const Str file = normalize_path(path);
        Str bytes;
        if (!session.reader()(file, str_out(bytes))) return cannot();
        return from_bytes(bytes, file, session, meta);
    }
    const CPath c_path(path);  // on the stack, so the tree is the only allocation
    std::FILE* f = std::fopen(c_path.c_str(), "rb");
    if (!f) return cannot();
    std::setvbuf(f, nullptr, _IONBF, 0);  // read straight into the arena
    long size = -1;
    if (std::fseek(f, 0, SEEK_END) == 0) size = std::ftell(f);
    if (size < 0 || static_cast<unsigned long>(size) > UINT32_MAX ||
        std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return cannot();
    }
    const auto bytes = static_cast<size_t>(size);
    char* data;
    const Arena::Handle arena(Arena::create_holding(allocator_of(session), bytes, data));
    const bool ok = std::fread(data, 1, bytes, f) == bytes;
    std::fclose(f);
    if (!ok) return cannot();
    return load(data, bytes, path, session.cipher, allocator_of(session), meta);
}

Expected<Ref<Array>> replayed(Expected<Ref<Array>> r, const Array* meta, LoadSession& session) {
    if (r && meta) replay(*meta, session);
    return r;
}
template <class Load>
Expected<Ref<Array>> load_and_replay(LoadSession& session, Load load) {
    const Array* meta = nullptr;
    auto r = load(meta);  // sets meta, so it is called before replayed() reads it
    return replayed(std::move(r), meta, session);
}

}  // namespace
}  // namespace detail

uint64_t detail::content_hash(std::string_view bytes) noexcept {
    return hash_bytes(bytes.data(), bytes.size());
}

Ref<Array> detail::read_fresh_cache(std::string_view path, LoadSession& session, bool from_disk) {
    const Array* meta = nullptr;
    auto r = open(path, session, meta, from_disk);
    if (!r || !meta || !fresh(*meta, session)) return nullptr;
    replay(*meta, session);
    return std::move(*r);
}

Expected<Ref<Array>> detail::read_shipped(std::string_view text_path, std::string_view cache_path,
                                          LoadSession& session, bool from_disk) {
    const Array* meta = nullptr;
    auto r = open(cache_path, session, meta, from_disk);
    // A cache baked without a recorded load says nothing to check, and is trusted.
    if (r && (!meta || begins_alike(*meta, session))) return replayed(std::move(r), meta, session);
    // Why the cache was set aside, for a warning if the text reads and the error if not.
    const Str why = r ? "'" + Str(cache_path) +
                            "' was baked for other macros, pack levels, or required files than "
                            "this session has"
                      : "no usable cache for '" + Str(text_path) + "': " + r.error().message();
    auto text = read_file(text_path, session);
    if (text) {
        SessionAccess::add_warning(session,
                                   make_error(r ? ErrorCode::Refused : ErrorCode::Io,
                                              why + "; read the text instead", Symbol(cache_path)));
        return text;
    }
    return Unexpected(make_error(r ? ErrorCode::Refused : text.error().code(),
                                 why + ", and the text cannot be read: " + text.error().message(),
                                 Symbol(cache_path)));
}

Expected<void> write_cache(const Array& root, TextSink out, const LoadSession* session) {
    Str bytes;
    if (auto r = detail::bytes_of(root, session, bytes); !r) return r;
    out(bytes);
    return {};
}

Expected<void> write_cache(const Array& root, std::string_view path, const LoadSession* session) {
    Str bytes;
    if (auto r = detail::bytes_of(root, session, bytes); !r) return r;
    const Str file = detail::normalize_path(path);
    std::FILE* f = std::fopen(file.c_str(), "wb");
    bool ok = f && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (f) ok = std::fclose(f) == 0 && ok;
    if (!ok) return Unexpected(make_error(ErrorCode::Io, "cannot write '" + file + "'"));
    return {};
}

Expected<Ref<Array>> read_cache_bytes(std::string_view bytes, std::string_view name,
                                      LoadSession& session) {
    return detail::load_and_replay(session, [&](const Array*& meta) {
        return detail::from_bytes(bytes, name, session, meta);
    });
}

Expected<Ref<Array>> read_cache_bytes(std::string_view bytes, std::string_view name) {
    LoadSession session;
    return read_cache_bytes(bytes, name, session);
}

Expected<Ref<Array>> read_cache(std::string_view path, LoadSession& session) {
    return detail::load_and_replay(
        session, [&](const Array*& meta) { return detail::open(path, session, meta); });
}

Expected<Ref<Array>> read_cache(std::string_view path) {
    LoadSession session;
    return read_cache(path, session);
}

bool cache_fresh(std::string_view cache_path, LoadSession& session) {
    const Array* meta = nullptr;
    auto r = detail::open(cache_path, session, meta);
    return r && meta && detail::fresh(*meta, session);
}

Expected<Ref<Array>> read_cached(std::string_view text_path, std::string_view cache_path,
                                 LoadSession& session) {
    if constexpr (kShipping) {  // a cache for this session, else the text; writes none
        return detail::read_shipped(text_path, cache_path, session);
    } else {
        if (Ref<Array> fresh = detail::read_fresh_cache(cache_path, session)) return fresh;
        const bool was = std::exchange(session.record_for_cache, true);
        auto root = read_file(text_path, session);
        session.record_for_cache = was;
        // A cache that cannot be written costs the next launch a parse, not this one its
        // tree.
        if (root) (void)write_cache(**root, cache_path, &session);
        return root;
    }
}

}  // namespace baker
