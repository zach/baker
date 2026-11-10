// Watcher (host/watcher.hpp): the stack's files, through the operating system's change
// notifications on their directories, or by their stamps where it has none.

#include <baker/host/watcher.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"

#include <chrono>
#include <filesystem>  // watch() alone: a path's lexical normal form, its name and directory

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#endif
#endif

namespace fs = std::filesystem;

namespace baker::host {
namespace {

// Text as the system names files: ReadDirectoryChangesW's names are UTF-16.
#ifdef _WIN32
using NativeChar = wchar_t;
#else
using NativeChar = char;
#endif
using NativeView = std::basic_string_view<NativeChar>;

// A file's write time on the system clock, so the epoch is 1970 everywhere: a file clock's
// own epoch differs by library (libstdc++ counts from 2174), and a value at or above 2^63
// would read as negative wherever it became a script integer.
uint64_t clock_ns(std::chrono::file_clock::time_point t) noexcept {
#ifdef _LIBCPP_VERSION
    // libc++ has no clock_cast: through the two clocks' offset now, which is off by the
    // microseconds between the two reads, alike for every time converted.
    const auto sys = std::chrono::system_clock::now() +
                     std::chrono::duration_cast<std::chrono::system_clock::duration>(
                         t - std::chrono::file_clock::now());
#else
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
#endif
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch());
    return ns.count() > 0 ? static_cast<uint64_t>(ns.count()) : 0;  // before 1970: unknown
}

uint64_t monotonic_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

constexpr uint64_t kStampInterval = 100'000'000;  // 100 ms

// "name://": a path no directory watch can reach.
bool has_scheme(std::string_view p) noexcept {
    const size_t at = p.find("://");
    return at != std::string_view::npos && at >= 2;
}

// A file's stamp from the system's own call, which allocates nothing: its last write
// on file_clock_now_ns's clock and, for a regular file, its size. Not present if it cannot
// be read.
#ifdef _WIN32
// FILETIME counts 100 ns from 1601. Microsoft's file clock counts the same, so its times are
// converted as std::filesystem's were.
uint64_t filetime_ns(const FILETIME& t) noexcept {
    const auto ticks = static_cast<int64_t>((uint64_t{t.dwHighDateTime} << 32) | t.dwLowDateTime);
#if defined(_MSVC_STL_VERSION)
    return clock_ns(std::chrono::file_clock::time_point(std::chrono::file_clock::duration(ticks)));
#else
    constexpr int64_t k1601To1970 = 116'444'736'000'000'000;
    return ticks > k1601To1970 ? static_cast<uint64_t>(ticks - k1601To1970) * 100 : 0;
#endif
}
FileStamp stamp_of(const WIN32_FILE_ATTRIBUTE_DATA& d) noexcept {
    FileStamp s;
    s.present = true;
    s.write_ns = filetime_ns(d.ftLastWriteTime);
    if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        s.size = (uint64_t{d.nFileSizeHigh} << 32) | d.nFileSizeLow;
    return s;
}
FileStamp disk_stamp(const wchar_t* file) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(file, GetFileExInfoStandard, &d)) return {};
    return stamp_of(d);
}
FileStamp disk_stamp(const char* file) noexcept {  // in the code page fs::path reads with
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(file, GetFileExInfoStandard, &d)) return {};
    return stamp_of(d);
}
#else
FileStamp disk_stamp(const char* file) noexcept {
    struct stat st {};
    if (::stat(file, &st) != 0) return {};
    FileStamp s;
    s.present = true;
#if defined(__APPLE__)
    const timespec& t = st.st_mtimespec;
#else
    const timespec& t = st.st_mtim;
#endif
    const int64_t ns = static_cast<int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
    s.write_ns = ns > 0 ? static_cast<uint64_t>(ns) : 0;
    s.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
    return s;
}
#endif

bool same(const FileStamp& a, const FileStamp& b) noexcept {
    return a.present == b.present && a.write_ns == b.write_ns && a.size == b.size;
}

uint64_t hash_of(NativeView text) noexcept {  // FNV-1a
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const NativeChar c : text) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Positions by hash, open addressing, built by each watch(): a file or a directory is found
// among those already listed in one probe, not a scan, so watch() is linear in the files.
class Index {
public:
    explicit Index(const Allocator& a) : slots_(a) {}
    void reset(size_t n) {
        size_t cap = 16;
        while (cap < n * 2) cap *= 2;
        slots_.assign(cap, 0);
        mask_ = cap - 1;
    }
    // The slot for `hash` holding a position `same` accepts, plus one; or the empty slot (0)
    // where one goes.
    template <class Same>
    uint32_t& slot(uint64_t hash, Same&& same) {
        for (size_t i = hash & mask_;; i = (i + 1) & mask_) {
            uint32_t& s = slots_[i];
            if (s == 0 || same(s - 1)) return s;
        }
    }

private:
    Vec<uint32_t> slots_;
    size_t mask_ = 0;
};

}  // namespace

uint64_t file_clock_now_ns() noexcept {
    return clock_ns(std::chrono::file_clock::now());
}

uint64_t file_stamp(std::string_view path, const StampHook& hook) noexcept {
    FileStamp s;
    if (!(hook && hook(path, s))) s = disk_stamp(detail::CPath(path).c_str());
    if (!s.present) return 0;
    // A file written in the last few seconds has no stamp to trust: two saves of the same
    // size within the file system's time resolution (two seconds on FAT, coarser than it
    // looks elsewhere) leave its write time as it was. Unknown, it is read and hashed.
    constexpr uint64_t kRacyNs = uint64_t{3} * 1000 * 1000 * 1000;
    const uint64_t now = file_clock_now_ns();
    if (s.write_ns == 0 || s.write_ns + kRacyNs > now) return 0;
    uint64_t z = s.write_ns ^ (s.size * 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z ^= z >> 31;
    return z == 0 ? 1 : z;  // 0 says unknown
}

struct Watcher::Impl {
    struct Entry {
        Symbol path;  // as given
        // Its file on the disk, unless hooked, at `texts[file]`, terminated; 0 is empty.
        uint32_t file = 0;
        // Its file name, as the system reports it, at `texts[name]`; 0 long: any name in `dir`
        // (a directory of layers).
        uint32_t name = 0, name_size = 0;
        size_t dir = SIZE_MAX;  // its directory's watch; SIZE_MAX: by stamp
        bool hooked = false;    // through the stamp hook
        bool pending = false;   // changed before a watch() kept it: report at next poll
        FileStamp stamp;        // the last seen
    };
    struct Dir {
        explicit Dir(const Allocator& a) : path(a) {}
        Vec<NativeChar> path;  // terminated
        uint64_t hash = 0;     // of the path
#ifdef _WIN32
        HANDLE handle = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped{};
        alignas(DWORD) unsigned char buffer[16 * 1024];
#elif defined(__linux__)
        int wd = -1;
#endif
        [[nodiscard]] NativeView view() const noexcept { return {path.data(), path.size() - 1}; }
    };

    const Allocator& alloc = default_allocator(MemoryKind::Tools);
    StampHook hook;
    DiskPathHook disk;
    Vec<Entry> entries{alloc};
    Vec<NativeChar> texts{alloc};  // the entries' files and names, each terminated
    Vec<Own<Dir>> dirs{alloc};
    Vec<Symbol> changed{alloc};
    bool any_pending = false;  // an entry is pending
    uint64_t newest_ns = 0;
    uint64_t next_stamp_ns = 0;
#if defined(__linux__)
    int fd = -1;
#endif

    Impl() {
        texts.push_back(0);
#if defined(__linux__)
        if constexpr (!kShipping) fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#endif
    }
    ~Impl() {
        for (Own<Dir>& d : dirs)
            if (d) close(*d);
#if defined(__linux__)
        if (fd >= 0) ::close(fd);
#endif
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] FileStamp stamp_disk(const Entry& e) const noexcept {
        return disk_stamp(texts.data() + e.file);
    }
    [[nodiscard]] NativeView name_of(const Entry& e) const noexcept {
        return {texts.data() + e.name, e.name_size};
    }
    // Appends `text`, terminated, to `texts`; where it starts.
    uint32_t keep(NativeView text) {
        const auto at = static_cast<uint32_t>(texts.size());
        texts.insert(texts.end(), text.data(), text.data() + text.size());
        texts.push_back(0);
        return at;
    }

    // Starts watching `d`; false if the system refuses (it does not exist, say).
    bool open(Dir& d) const {
#ifdef _WIN32
        d.handle =
            CreateFileW(d.path.data(), FILE_LIST_DIRECTORY,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (d.handle == INVALID_HANDLE_VALUE) return false;
        if (issue(d)) return true;
        CloseHandle(d.handle);
        d.handle = INVALID_HANDLE_VALUE;
        return false;
#elif defined(__linux__)
        if (fd < 0) return false;
        d.wd =
            inotify_add_watch(fd, d.path.data(),
                              IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE);
        return d.wd >= 0;
#else
        (void)d;
        return false;  // no notifications here: every file by its stamp
#endif
    }
    void close(Dir& d) const {
#ifdef _WIN32
        if (d.handle == INVALID_HANDLE_VALUE) return;
        CancelIoEx(d.handle, &d.overlapped);
        DWORD bytes = 0;
        GetOverlappedResult(d.handle, &d.overlapped, &bytes, TRUE);  // let the cancel land
        CloseHandle(d.handle);
        d.handle = INVALID_HANDLE_VALUE;
#elif defined(__linux__)
        if (d.wd >= 0 && fd >= 0) inotify_rm_watch(fd, d.wd);
        d.wd = -1;
#else
        (void)d;
#endif
    }
#ifdef _WIN32
    static bool issue(Dir& d) {
        d.overlapped = OVERLAPPED{};
        return ReadDirectoryChangesW(d.handle, d.buffer, sizeof d.buffer, FALSE,
                                     FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                         FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                                     nullptr, &d.overlapped, nullptr) != 0;
    }
#endif

    void note(size_t i) {
        Entry& e = entries[i];
        // What was seen, so a later watch() can tell an edit since from one reported.
        if (!e.hooked) e.stamp = stamp_disk(e);
        if (e.stamp.write_ns > newest_ns) newest_ns = e.stamp.write_ns;
        for (const Symbol s : changed)
            if (s == e.path) return;
        changed.push_back(e.path);
    }

    // A change the system reported: `name` in the directory watched as `dir`.
    void reported(size_t dir, NativeView name) {
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].dir == dir && (entries[i].name_size == 0 || name_of(entries[i]) == name))
                note(i);
    }
    // Every file in the directory watched as `dir`: its notifications were lost.
    void all_in(size_t dir) {
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].dir == dir) note(i);
    }

    void poll_system() {
#ifdef _WIN32
        for (size_t di = 0; di < dirs.size(); ++di) {
            Dir& d = *dirs[di];
            DWORD bytes = 0;
            if (!GetOverlappedResult(d.handle, &d.overlapped, &bytes, FALSE)) {
                if (GetLastError() != ERROR_IO_INCOMPLETE) issue(d);
                continue;
            }
            if (bytes == 0) {
                all_in(di);  // the buffer overflowed: names lost
            } else {
                for (const unsigned char* p = d.buffer;;) {
                    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                    reported(di, {info->FileName, info->FileNameLength / sizeof(WCHAR)});
                    if (info->NextEntryOffset == 0) break;
                    p += info->NextEntryOffset;
                }
            }
            issue(d);
        }
#elif defined(__linux__)
        if (fd < 0) return;
        alignas(inotify_event) char buffer[16 * 1024];
        for (;;) {
            const ssize_t n = read(fd, buffer, sizeof buffer);
            if (n <= 0) break;  // EAGAIN: nothing more
            for (ssize_t at = 0; at < n;) {
                const auto* e = reinterpret_cast<const inotify_event*>(buffer + at);
                at += static_cast<ssize_t>(sizeof(inotify_event) + e->len);
                size_t di = SIZE_MAX;
                for (size_t k = 0; k < dirs.size() && di == SIZE_MAX; ++k)
                    if (dirs[k]->wd == e->wd) di = k;
                if (e->mask & IN_Q_OVERFLOW) {
                    for (size_t k = 0; k < dirs.size(); ++k) all_in(k);
                } else if (di == SIZE_MAX || ((e->mask & IN_CREATE) && !(e->mask & IN_ISDIR))) {
                    continue;  // a file is reported when it is written; a new folder now
                } else if (e->mask & IN_IGNORED) {
                    // The directory went: its files are checked by their stamps from now on.
                    for (Entry& entry : entries)
                        if (entry.dir == di) {
                            entry.dir = SIZE_MAX;
                            entry.stamp = stamp_disk(entry);
                            note(static_cast<size_t>(&entry - entries.data()));
                        }
                    dirs[di]->wd = -1;
                } else if (e->len > 0) {
                    reported(di, std::string_view(e->name));
                }
            }
        }
#endif
    }

    void watch(std::span<const StackFile> files, std::span<const std::string_view> more);

    void poll_stamps() {
        const uint64_t now = monotonic_ns();
        if (now < next_stamp_ns) return;
        next_stamp_ns = now + kStampInterval;
        for (size_t i = 0; i < entries.size(); ++i) {
            Entry& e = entries[i];
            if (e.dir != SIZE_MAX) continue;
            FileStamp s;
            if (!(e.hooked && hook && hook(e.path.str(), s))) s = stamp_disk(e);
            if (same(s, e.stamp)) continue;
            e.stamp = s;
            note(i);
        }
    }
};

Watcher::Watcher() : impl_(detail::create<Impl>(default_allocator(MemoryKind::Tools))) {}

Watcher::~Watcher() {
    detail::destroy(default_allocator(MemoryKind::Tools), impl_);
}

void Watcher::set_stamp_hook(StampHook hook) {
    impl_->hook = std::move(hook);
}

void Watcher::set_disk_path_hook(DiskPathHook hook) {
    impl_->disk = std::move(hook);
}

void Watcher::watch(std::span<const StackFile> files, std::span<const std::string_view> more) {
    Impl& im = *impl_;
    im.changed.clear();
    if constexpr (kShipping) {
        im.entries.clear();  // shipping: inert
    } else {
        im.watch(files, more);
    }
}

// Inline, so a shipping build, which never calls it, does not emit it.
inline void Watcher::Impl::watch(std::span<const StackFile> files,
                                 std::span<const std::string_view> more) {
    // A file that stays in the list keeps what was last seen of it, so an edit since the
    // last poll is still reported; a file new to the list is stamped as now.
    const Vec<Entry> before = std::move(entries);
    entries.clear();
    entries.reserve(files.size() + more.size());
    texts.clear();
    texts.push_back(0);  // the empty text, a hooked entry's file
    // Each found by hash: the files listed so far, those listed before, the directories kept
    // so far, and those watched before.
    Index listed(alloc), was(alloc), kept_at(alloc), dirs_at(alloc);
    listed.reset(files.size() + more.size());
    was.reset(before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        uint32_t& s = was.slot(before[i].path.hash(),
                               [&](uint32_t k) { return before[k].path == before[i].path; });
        if (!s) s = static_cast<uint32_t>(i + 1);
    }
    dirs_at.reset(dirs.size());
    for (size_t i = 0; i < dirs.size(); ++i)
        if (dirs[i]) {
            uint32_t& s = dirs_at.slot(dirs[i]->hash, [](uint32_t) { return false; });
            s = static_cast<uint32_t>(i + 1);
        }
    Vec<Own<Dir>> kept(alloc);
    kept_at.reset(files.size() + more.size());
    // The directory `dir` is watched as, opening it if new; SIZE_MAX if it cannot be.
    auto dir_of = [&](NativeView dir) -> size_t {
        const uint64_t h = hash_of(dir);
        uint32_t& k = kept_at.slot(h, [&](uint32_t i) { return kept[i]->view() == dir; });
        if (k) return k - 1;
        uint32_t& d =
            dirs_at.slot(h, [&](uint32_t i) { return dirs[i] && dirs[i]->view() == dir; });
        if (d) {
            kept.push_back(std::move(dirs[d - 1]));
        } else {
            Own<Dir> nd = Own<Dir>::make(alloc, alloc);
            nd->path.assign(dir.data(), dir.data() + dir.size());
            nd->path.push_back(0);
            nd->hash = h;
            if (!open(*nd)) return SIZE_MAX;
            kept.push_back(std::move(nd));
        }
        k = static_cast<uint32_t>(kept.size());
        return kept.size() - 1;
    };
    auto add = [&](Symbol path, bool directory) {
        uint32_t& at =
            listed.slot(path.hash(), [&](uint32_t i) { return entries[i].path == path; });
        if (at) return;
        Entry e;
        e.path = path;
        // The file the host says the path is, if it says.
        Str on_disk(alloc);
        const bool mapped = disk && disk(path.str(), {[](void* to, std::string_view text) {
                                                          static_cast<Str*>(to)->append(text);
                                                      },
                                                      &on_disk});
        FileStamp hooked;
        e.hooked = !mapped &&
                   (has_scheme(path.str()) || (hook && hook(path.str(), hooked)));  // the host's
        if (e.hooked) {
            if (hook) hook(path.str(), e.stamp);
        } else {
            fs::path file = fs::path(mapped ? on_disk.view() : path.str()).lexically_normal();
            fs::path dir;
            if (directory) {
                if (!file.has_filename()) file = file.parent_path();  // "mods/"
                dir = file;  // a file added to it, or removed, is a change
            } else {
                const fs::path name = file.filename();
                e.name = keep(name.native());
                e.name_size = static_cast<uint32_t>(name.native().size());
                dir = file.parent_path();
            }
            if (dir.empty()) dir = ".";
            e.file = keep(file.native());
            e.dir = dir_of(dir.native());
            e.stamp = stamp_disk(e);
        }
        if (const uint32_t w =
                was.slot(path.hash(), [&](uint32_t k) { return before[k].path == path; })) {
            const Entry& prior = before[w - 1];
            e.pending = prior.pending || !same(prior.stamp, e.stamp);
            any_pending = any_pending || e.pending;
        }
        at = static_cast<uint32_t>(entries.size() + 1);
        entries.push_back(e);
    };
    for (const StackFile& f : files) add(f.path, f.directory);
    for (const std::string_view p : more) add(Symbol(p), false);
    for (Own<Dir>& d : dirs)
        if (d) close(*d);  // directories no file is in any more
    dirs = std::move(kept);
    next_stamp_ns = monotonic_ns() + kStampInterval;
}

bool Watcher::poll() {
    Impl& im = *impl_;
    im.changed.clear();
    im.newest_ns = 0;
    if constexpr (!kShipping) {  // shipping: inert
        if (im.any_pending) {    // edits a watch() found unreported
            im.any_pending = false;
            for (size_t i = 0; i < im.entries.size(); ++i)
                if (im.entries[i].pending) {
                    im.entries[i].pending = false;
                    im.note(i);
                }
        }
        im.poll_system();
        im.poll_stamps();
    }
    return !im.changed.empty();
}

std::span<const Symbol> Watcher::changed() const noexcept {
    return {impl_->changed.data(), impl_->changed.size()};
}

uint64_t Watcher::newest_write_ns() const noexcept {
    return impl_->newest_ns;
}

size_t Watcher::watched() const noexcept {
    return impl_->entries.size();
}

size_t Watcher::by_stamp() const noexcept {
    size_t n = 0;
    for (const Impl::Entry& e : impl_->entries) n += e.dir == SIZE_MAX ? 1 : 0;
    return n;
}

}  // namespace baker::host
