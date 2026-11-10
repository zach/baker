#pragma once

// The stack image's container (image.hpp): sections of two kinds, a layer's tree as
// a cache and a baked table, each with a name and its bytes.

#include <baker/image.hpp>
#include <baker/parse.hpp>

#include "common/containers.hpp"

namespace baker::detail {

struct ImageSection {
    enum class Kind : uint32_t { Layer = 1, Table = 2 };
    Kind kind;
    std::string_view name;   // a layer's name; a table's view name (empty in shipping)
    std::string_view bytes;  // into LoadedImage::file
};

struct LoadedImage {
    explicit LoadedImage(const Allocator& a) : file(a), sections(a) {}
    Str file;  // the whole image, which the sections point into
    Vec<ImageSection> sections;
};

// Reads the image at `path` through the session's reader (or the disk), checking its
// header and hash.
// A layer section of an image read_image accepted: read as read_cache_bytes reads a cache,
// without the cache's own checksum, which the image's covered.
[[nodiscard]] Expected<Ref<Array>> read_image_layer(std::string_view bytes, std::string_view name,
                                                    LoadSession& session);
[[nodiscard]] Expected<LoadedImage> read_image(std::string_view path, const LoadSession& session,
                                               const Allocator& alloc);

// The writer's side: one section appended to `out`.
void append_section(Str& out, ImageSection::Kind kind, std::string_view name,
                    std::string_view bytes);
// The container around the sections: the header before them, the hash over all of it.
void begin_image(Str& out);
void finish_image(Str& out, uint32_t sections);

}  // namespace baker::detail
