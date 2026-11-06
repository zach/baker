// The stack image (image.hpp): a container of sections, each a layer's tree as a
// cache (cache.hpp, unchanged) or a baked table with what fixes it up.
//
//   header: magic "MOIRA.IMG1", version u16, endian u32, pointer_bytes u8, pad, sections
//           u32, hash u64 (of the whole file, this field read as 0)
//   section: kind u32, name length u32, name (padded to 8), bytes u32, bytes (padded to 8)

#include "image.hpp"

#include <baker/cache.hpp>
#include <baker/table.hpp>

#include "session.hpp"
#include "table_build.hpp"
#include "text.hpp"

#include <cstring>

namespace baker {

namespace {

constexpr char kMagic[10] = {'M', 'O', 'I', 'R', 'A', '.', 'I', 'M', 'G', '1'};
constexpr uint16_t kVersion = 1;
constexpr uint32_t kEndian = 0x01020304;

struct Header {
    char magic[10];
    uint16_t version;
    uint32_t endian;
    uint8_t pointer_bytes;
    uint8_t pad[3];
    uint32_t sections;
    uint64_t hash;
};
static_assert(sizeof(Header) == 32);

constexpr size_t round8(size_t n) noexcept {
    return (n + 7) & ~size_t{7};
}

template <class T>
void put(Str& out, size_t at, const T& v) {
    std::memcpy(out.data() + at, &v, sizeof v);
}

template <class T>
bool get(std::string_view in, size_t at, T& v) {
    if (in.size() < at + sizeof v) return false;
    std::memcpy(&v, in.data() + at, sizeof v);
    return true;
}

uint64_t image_hash(std::string_view bytes) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < bytes.size(); ++i) {
        const bool hash_field = i >= offsetof(Header, hash) && i < offsetof(Header, hash) + 8;
        h ^= hash_field ? 0 : static_cast<unsigned char>(bytes[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

Unexpected bad(std::string_view name, std::string_view why, ErrorCode code = ErrorCode::Syntax) {
    return Unexpected(make_error(code, "'" + Str(name) + "' " + Str(why), Symbol(name)));
}

}  // namespace

namespace detail {

void begin_image(Str& out) {
    out.clear();
    out.resize(sizeof(Header));
}

void append_section(Str& out, ImageSection::Kind kind, std::string_view name,
                    std::string_view bytes) {
    const size_t at = out.size();
    out.resize(at + 8 + round8(name.size()) + 4 + round8(bytes.size()) + 4);
    // Laid out as: kind u32, name length u32, name padded, bytes u32, bytes padded.
    put(out, at, static_cast<uint32_t>(kind));
    put(out, at + 4, static_cast<uint32_t>(name.size()));
    std::memcpy(out.data() + at + 8, name.data(), name.size());
    const size_t b = at + 8 + round8(name.size());
    put(out, b, static_cast<uint32_t>(bytes.size()));
    std::memcpy(out.data() + b + 8, bytes.data(), bytes.size());
    out.resize(b + 8 + round8(bytes.size()));
}

void finish_image(Str& out, uint32_t sections) {
    Header h{};
    std::memcpy(h.magic, kMagic, sizeof kMagic);
    h.version = kVersion;
    h.endian = kEndian;
    h.pointer_bytes = sizeof(void*);
    h.sections = sections;
    put(out, 0, h);
    put(out, offsetof(Header, hash), image_hash(out.view()));
}

Expected<LoadedImage> read_image(std::string_view path, const LoadSession& session,
                                 const Allocator& alloc) {
    LoadedImage image(alloc);
    if (!read_with(session.reader(), path, image.file))
        return Unexpected(make_error(ErrorCode::Io, "cannot read '" + Str(path) + "'"));
    const std::string_view in = image.file.view();
    Header h{};
    if (!get(in, 0, h) || std::memcmp(h.magic, kMagic, sizeof kMagic) != 0)
        return bad(path, "is not a Baker stack image");
    if (h.version != kVersion)
        return bad(path,
                   "is a stack image of version " + decimal(h.version) +
                       "; this build reads version " + decimal(kVersion),
                   ErrorCode::Refused);
    if (h.endian != kEndian || h.pointer_bytes != sizeof(void*))
        return bad(path, "was written on a platform of another endianness or pointer size",
                   ErrorCode::Refused);
    if (h.hash != image_hash(in)) return bad(path, "is damaged");
    size_t at = sizeof(Header);
    for (uint32_t i = 0; i < h.sections; ++i) {
        uint32_t kind = 0, name_len = 0, bytes = 0;
        if (!get(in, at, kind) || !get(in, at + 4, name_len) || in.size() - at - 8 < name_len)
            return bad(path, "is damaged");
        const std::string_view name = in.substr(at + 8, name_len);
        const size_t b = at + 8 + round8(name_len);
        if (!get(in, b, bytes) || in.size() - b - 8 < bytes) return bad(path, "is damaged");
        if (kind != static_cast<uint32_t>(ImageSection::Kind::Layer) &&
            kind != static_cast<uint32_t>(ImageSection::Kind::Table))
            return bad(path, "holds a section of an unknown kind");
        image.sections.push_back(
            {static_cast<ImageSection::Kind>(kind), name, in.substr(b + 8, bytes)});
        at = b + 8 + round8(bytes);
    }
    return image;
}

}  // namespace detail

Expected<void> write_stack_image(const Config& cfg, std::span<const Symbol> layers, TextSink out) {
    Str image;
    detail::begin_image(image);
    uint32_t sections = 0;
    for (const Symbol layer : layers) {
        const Ref<Array> root = cfg.layer_root(layer.str());
        if (!root)
            return Unexpected(make_error(
                ErrorCode::NotFound,
                "the stack has no layer '" + Str(layer.str()) + "' to put in the image"));
        Str bytes;
        if (auto r = write_cache(*root, detail::str_out(bytes)); !r) return r;
        detail::append_section(image, detail::ImageSection::Kind::Layer, layer.str(), bytes.view());
        ++sections;
    }
    const List<Error> table_errors = cfg.table_errors();  // built with errors: not shipped
    if (!table_errors.empty()) return Unexpected(table_errors[0]);
    for (const detail::TableSlot& slot : detail::table_slots(cfg)) {
        if (!slot.built) continue;
        Str bytes;
        if (auto r = detail::write_table(*slot.built, slot.spec, bytes); !r) return r;
        detail::append_section(image, detail::ImageSection::Kind::Table, slot.spec.name,
                               bytes.view());
        ++sections;
    }
    detail::finish_image(image, sections);
    out(image.view());
    return {};
}

}  // namespace baker
