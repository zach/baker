#pragma once

// What the binary cache reads from and replays into a LoadSession (cache.cpp).

#include <baker/parse.hpp>

#include "common/containers.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace baker::detail {

// A file a recorded load read, or looked for and did not find (size -1).
struct CacheFile {
    Str path;
    int64_t size = 0;  // -1: looked for and absent; or a directory's, below
    uint64_t hash = 0;
};
// A directory a load listed (#include_dir): `hash` is its listing's; one it could not list.
inline constexpr int64_t kListedDirectory = -2;
inline constexpr int64_t kUnlistedDirectory = -3;

// The cache's checksum, also of the files a load read (cache.cpp).
[[nodiscard]] uint64_t content_hash(std::string_view bytes) noexcept;
// Reads `path` through `reader` into `text`, replacing what it held (parse.cpp).
bool read_with(const LoadSession::Reader& reader, std::string_view path, Str& text);
// The disk's directory listing, LoadSession's default lister (list_dir.cpp).
bool list_disk_dir(std::string_view dir, Callback<void(std::string_view path)> entry);
// Lists `dir` through `lister` into `entries`, sorted, and returns a hash of them, or 0 if
// it cannot be listed (parse.cpp).
uint64_t list_with(const LoadSession::Lister& lister, std::string_view dir, Vec<Str>& entries,
                   const Allocator& alloc = default_allocator());
// The order a directory's order.dta (`text`, read from `file`) gives `names`, which are in
// name order: in `order`, the indices of the names it lists, in its order, then the rest's.
// A name it lists that `names` lacks is a warning, located in it. An error if it does not
// parse, or holds anything but (order NAME ...).
Expected<void> directory_order(std::string_view text, std::string_view file,
                               std::span<const std::string_view> names, Vec<uint32_t>& order,
                               Vec<Error>& warnings);

// The cache at `path`, replayed on the session, if it loads and is fresh; else null.
// `from_disk`: the cache file is read from disk straight into its arena, whatever reader the
// session has; a Config passes it when its own reader is the disk's, since its session's
// reader wraps that one only to record the text files a layer read.
[[nodiscard]] Ref<Array> read_fresh_cache(std::string_view path, LoadSession& session,
                                          bool from_disk = false);
// Shipping's read: the cache at `cache_path` if the session begins as its load did (macros,
// pack levels, required files; no file is read), else the text, else a located refusal
// when a cache exists but was baked for another session.
[[nodiscard]] Expected<Ref<Array>> read_shipped(std::string_view text_path,
                                                std::string_view cache_path, LoadSession& session,
                                                bool from_disk = false);

struct SessionAccess {
    using Defines = HashMap<Symbol, Ref<Array>>;
    [[nodiscard]] static bool reads_disk(const LoadSession& s) noexcept;
    // Whether the last top-level read was recorded (LoadSession::record_for_cache).
    [[nodiscard]] static bool recorded(const LoadSession& s) noexcept;
    [[nodiscard]] static std::span<const CacheFile> files(const LoadSession& s) noexcept;
    [[nodiscard]] static const Defines& defines(const LoadSession& s) noexcept;
    [[nodiscard]] static const Defines& defines_before(const LoadSession& s) noexcept;
    // Each name an #ifdef, #ifndef, #define or #undef named, in the order read; and the
    // files parsed, which a cache's replay leaves as they were.
    [[nodiscard]] static std::span<const Symbol> tested(const LoadSession& s) noexcept;
    [[nodiscard]] static uint64_t parses(const LoadSession& s) noexcept;
    // The #autorun commands the last top-level read added.
    [[nodiscard]] static std::span<const Node> new_autorun(const LoadSession& s) noexcept;
    static void add_autorun(LoadSession& s, Node command);
    // A warning for warnings(), as the one-way rule's are (a shipping cache set aside).
    static void add_warning(LoadSession& s, Error warning);
    // The files #require has read, in order; the first required_before() were read before
    // the last recorded top-level read began.
    [[nodiscard]] static std::span<const Str> required(const LoadSession& s) noexcept;
    [[nodiscard]] static size_t required_before(const LoadSession& s) noexcept;
    static void add_required(LoadSession& s, std::string_view path);
    // Packs: the levels, the packs the last recorded read declared, and the pack that
    // defined a macro, if any.
    [[nodiscard]] static std::span<const Symbol> pack_levels(const LoadSession& s) noexcept;
    [[nodiscard]] static std::span<const Pack> new_packs(const LoadSession& s) noexcept;
    [[nodiscard]] static const Pack* macro_pack(const LoadSession& s, Symbol macro) noexcept;
    // Adds a pack, ranked by the session's levels, unless its file has one; returns its index
    // either way. The Parser and a cache's replay share it. And attributes a macro
    // to the pack declared by `file`.
    static size_t add_pack(LoadSession& s, const Pack& pack);
    // Warns, at `line` of the pack's file, of each pack `pack` requires that the session has
    // not loaded: a required pack loads first, below or earlier.
    static void warn_missing_requires(LoadSession& s, const Pack& pack, int32_t line);
    static void set_macro_pack(LoadSession& s, Symbol macro, Symbol file);
    // Origins: those the last recorded read declared; and one a cache recorded.
    [[nodiscard]] static std::span<const Origin> new_origins(const LoadSession& s) noexcept;
    // The macro definitions the last recorded read made, and one replayed from a cache.
    [[nodiscard]] static std::span<const Definition> new_definitions(const LoadSession& s) noexcept;
    static void add_definition(LoadSession& s, const Definition& definition);
    static void add_origin(LoadSession& s, const Origin& origin);
};

}  // namespace baker::detail
