#pragma once

// The binary cache: a tree written as it lies in memory,
// and loaded with one read into one allocation and one pass that turns offsets into
// pointers.
//
//   auto root = baker::read_file("level1.dta", session);
//   baker::write_cache(**root, "level1.datb");
//   auto same = baker::read_cache("level1.datb", session);    // Expected<Ref<Array>>
//
// A cache holds the expanded tree: macros and #ifdef are resolved before it is written.
// Written from a recorded load, it also replays that load's defines, macros, and #autorun
// commands on the session that reads it. Functions and objects, which
// baker::script makes and a parse never does, cannot be written. A cache is read only by
// a build of the same format version, endianness, and pointer size.
//
// The header's hash finds a damaged file, not a forged one: a cache is build output, so
// read text, which the parser checks, from a source you do not trust.

#include <baker/array.hpp>
#include <baker/error.hpp>
#include <baker/parse.hpp>
#include <baker/text.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace baker {

// Enciphers a cache in place, and deciphers it: set LoadSession::cipher. The
// cipher must keep the length. No implementation ships. The original enciphered its caches
// so that players would not mail the studio their own cheat codes.
struct Cipher {
    void (*encrypt)(void* user, std::span<std::byte> bytes) = nullptr;
    void (*decrypt)(void* user, std::span<std::byte> bytes) = nullptr;
    void* user = nullptr;
};

// With a session whose last top-level read was recorded (LoadSession::record_for_cache),
// the cache also keeps the files that read used, the defines it began with, and its effects
// on the session, which a load of the cache replays. The session's cipher, if any,
// enciphers it.
[[nodiscard]] Expected<void> write_cache(const Array& root, std::string_view path,
                                         const LoadSession* session = nullptr);
// The same bytes, to `out`: for a host's own file system.
[[nodiscard]] Expected<void> write_cache(const Array& root, TextSink out,
                                         const LoadSession* session = nullptr);

// Through the session's reader, allocator, and cipher; replays the recorded load's effects
// on the session. With the default reader, the tree is one allocation; a custom reader
// costs a temporary copy of the file.
[[nodiscard]] Expected<Ref<Array>> read_cache(std::string_view path, LoadSession& session);
[[nodiscard]] Expected<Ref<Array>> read_cache(std::string_view path);
// From a cache's bytes in memory, copied into the tree's one allocation. `name` labels
// errors.
[[nodiscard]] Expected<Ref<Array>> read_cache_bytes(std::string_view bytes, std::string_view name,
                                                    LoadSession& session);
[[nodiscard]] Expected<Ref<Array>> read_cache_bytes(std::string_view bytes,
                                                    std::string_view name = "<cache>");

// True if the cache at `cache_path` was written from a recorded load, every file that load
// read (through the session's reader) is unchanged, every file it looked for and missed is
// still missing, and the session's defines and macros are those the load began with.
// Re-reads every file: for development and build steps, not every launch of a shipped game.
[[nodiscard]] bool cache_fresh(std::string_view cache_path, LoadSession& session);
// The cache if it is fresh; otherwise the text, and a new cache written from it. Under
// BAKER_SHIPPING, the cache if it loads, and no cache is written.
[[nodiscard]] Expected<Ref<Array>> read_cached(std::string_view text_path,
                                               std::string_view cache_path, LoadSession& session);

}  // namespace baker
