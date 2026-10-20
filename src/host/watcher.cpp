// Watcher (host/watcher.hpp): the stack's files, through the operating system's change
// notifications on their directories, or by their stamps where it has none.

#include <baker/host/watcher.hpp>

#include "common/containers.hpp"

#include <chrono>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace baker::host {
namespace {

// A file's write time on the system clock, so the epoch is 1970 everywhere: a file clock's
// own epoch differs by library (libstdc++ counts from 2174), and a value at or above 2^63
// would read as negative wherever it became a script integer.
uint64_t clock_ns(fs::file_time_type t) noexcept {
#ifdef _LIBCPP_VERSION
    // libc++ has no clock_cast: through the two clocks' offset now, which is off by the
    // microseconds between the two reads, alike for every time converted.
    const auto sys = std::chrono::system_clock::now() +
                     std::chrono::duration_cast<std::chrono::system_clock::duration>(
                         t - fs::file_time_type::clock::now());
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

FileStamp disk_stamp(const fs::path& file) noexcept {
    std::error_code ec;
    FileStamp s;
    const auto t = fs::last_write_time(file, ec);
    if (ec) return s;
    s.present = true;
    s.write_ns = clock_ns(t);
    const auto size = fs::file_size(file, ec);
    s.size = ec ? 0 : static_cast<uint64_t>(size);
    return s;
}

bool same(const FileStamp& a, const FileStamp& b) noexcept {
    return a.present == b.present && a.write_ns == b.write_ns && a.size == b.size;
}

}  // namespace

uint64_t file_clock_now_ns() noexcept {
    return clock_ns(fs::file_time_type::clock::now());
}

struct Watcher::Impl {
    struct Entry {
        Symbol path;                 // as given
        fs::path file;               // on the disk, unless hooked
        fs::path::string_type name;  // its file name, as the system reports it
        size_t dir = SIZE_MAX;       // its directory's watch; SIZE_MAX: by stamp
        bool hooked = false;         // through the stamp hook
        FileStamp stamp;             // the last seen, for those checked by stamp
    };
    struct Dir {
        fs::path path;
#ifdef _WIN32
        HANDLE handle = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped{};
        alignas(DWORD) unsigned char buffer[16 * 1024];
#elif defined(__linux__)
        int wd = -1;
#endif
    };

    StampHook hook;
    DiskPathHook disk;
    Vec<Entry> entries;
    Vec<Own<Dir>> dirs;
    Vec<Symbol> changed;
    uint64_t newest_ns = 0;
    uint64_t next_stamp_ns = 0;
#if defined(__linux__)
    int fd = -1;
#endif

    Impl() {
#if defined(__linux__)
        if constexpr (!kShipping) fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#endif
    }
    ~Impl() {
        for (Own<Dir>& d : dirs) close(*d);
#if defined(__linux__)
        if (fd >= 0) ::close(fd);
#endif
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // Starts watching `d`; false if the system refuses (it does not exist, say).
    bool open(Dir& d) const {
#ifdef _WIN32
        d.handle =
            CreateFileW(d.path.c_str(), FILE_LIST_DIRECTORY,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (d.handle == INVALID_HANDLE_VALUE) return false;
        if (issue(d)) return true;
        CloseHandle(d.handle);
        d.handle = INVALID_HANDLE_VALUE;
        return false;
#elif defined(__linux__)
        if (fd < 0) return false;
        d.wd = inotify_add_watch(fd, d.path.c_str(),
                                 IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE);
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
                                     FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                         FILE_NOTIFY_CHANGE_SIZE,
                                     nullptr, &d.overlapped, nullptr) != 0;
    }
#endif

    // The directory `dir` is watched as, opening it if new; SIZE_MAX if it cannot be.
    size_t dir_of(const fs::path& dir, Vec<Own<Dir>>& kept) {
        for (size_t i = 0; i < kept.size(); ++i)
            if (kept[i]->path == dir) return i;
        for (Own<Dir>& d : dirs)
            if (d && d->path == dir) {
                kept.push_back(std::move(d));
                return kept.size() - 1;
            }
        Own<Dir> d = Own<Dir>::make(default_allocator());
        d->path = dir;
        if (!open(*d)) return SIZE_MAX;
        kept.push_back(std::move(d));
        return kept.size() - 1;
    }

    void note(size_t i) {
        const Entry& e = entries[i];
        for (const Symbol s : changed)
            if (s == e.path) return;
        changed.push_back(e.path);
        const FileStamp s = e.hooked ? e.stamp : disk_stamp(e.file);
        if (s.write_ns > newest_ns) newest_ns = s.write_ns;
    }

    // A change the system reported: `name` in the directory watched as `dir`.
    void reported(size_t dir, std::basic_string_view<fs::path::value_type> name) {
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].dir == dir && entries[i].name == name) note(i);
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
                } else if (di == SIZE_MAX) {
                    continue;
                } else if (e->mask & IN_IGNORED) {
                    // The directory went: its files are checked by their stamps from now on.
                    for (Entry& entry : entries)
                        if (entry.dir == di) {
                            entry.dir = SIZE_MAX;
                            entry.stamp = disk_stamp(entry.file);
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
            if (!(e.hooked && hook && hook(e.path.str(), s))) s = disk_stamp(e.file);
            if (same(s, e.stamp)) continue;
            e.stamp = s;
            note(i);
        }
    }
};

Watcher::Watcher() : impl_(detail::create<Impl>(default_allocator())) {}

Watcher::~Watcher() {
    detail::destroy(default_allocator(), impl_);
}

void Watcher::set_stamp_hook(StampHook hook) {
    impl_->hook = std::move(hook);
}

void Watcher::set_disk_path_hook(DiskPathHook hook) {
    impl_->disk = std::move(hook);
}

void Watcher::watch(std::span<const StackFile> files, std::span<const std::string_view> more) {
    Impl& im = *impl_;
    im.entries.clear();
    im.changed.clear();
    if constexpr (!kShipping) im.watch(files, more);  // shipping: inert
}

void Watcher::Impl::watch(std::span<const StackFile> files,
                          std::span<const std::string_view> more) {
    Impl& im = *this;
    Vec<Own<Impl::Dir>> kept;
    auto add = [&](Symbol path) {
        for (const Impl::Entry& e : im.entries)
            if (e.path == path) return;
        Impl::Entry e;
        e.path = path;
        // The file the host says the path is, if it says.
        Str on_disk;
        const bool mapped = im.disk && im.disk(path.str(), {[](void* to, std::string_view text) {
                                                                static_cast<Str*>(to)->append(text);
                                                            },
                                                            &on_disk});
        FileStamp hooked;
        e.hooked = !mapped && (has_scheme(path.str()) ||
                               (im.hook && im.hook(path.str(), hooked)));  // the host's
        if (e.hooked) {
            if (im.hook) im.hook(path.str(), e.stamp);
        } else {
            e.file = fs::path(mapped ? std::string_view(on_disk) : path.str()).lexically_normal();
            e.name = e.file.filename().native();
            fs::path dir = e.file.parent_path();
            if (dir.empty()) dir = ".";
            e.dir = im.dir_of(dir, kept);
            e.stamp = disk_stamp(e.file);
        }
        im.entries.push_back(std::move(e));
    };
    for (const StackFile& f : files) add(f.path);
    for (const std::string_view p : more) add(Symbol(p));
    for (Own<Impl::Dir>& d : im.dirs)
        if (d) im.close(*d);  // directories no file is in any more
    im.dirs = std::move(kept);
    im.next_stamp_ns = monotonic_ns() + kStampInterval;
}

bool Watcher::poll() {
    Impl& im = *impl_;
    im.changed.clear();
    im.newest_ns = 0;
    if constexpr (!kShipping) {  // shipping: inert
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
