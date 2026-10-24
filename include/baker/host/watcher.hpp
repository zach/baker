#pragma once

// Watcher: tells a host which of its stack's files were saved, so it reloads only then, with
// the save's time as the origin of edit-to-see.
//
//   baker::host::Watcher watcher;
//   watcher.watch(cfg.stack_files());              // again whenever stack_files_epoch() moves
//   each frame:
//     if (watcher.poll())
//         cfg.reload({.trigger = "watcher", .origin_ns = watcher.newest_write_ns()});
//
// It watches exactly the files it is given, not whole directories: an editor's swap file, or
// a data file nothing reads, changes nothing. Each file's directory is watched through the
// operating system (inotify on Linux, ReadDirectoryChangesW on Windows), so a quiet frame
// costs one non-blocking call and allocates nothing. A file whose directory does not exist
// yet, and every file on another system, is checked by its write time and size instead, at
// most every 100 ms. A path with a scheme ("user://...") is checked through the host's
// stamp hook, since it names no file on the disk, unless the host's disk-path hook says
// which file it is: then that file is watched as any other, and still reported by the path
// the host gave.
//
// Under BAKER_SHIPPING it is inert: it watches nothing and poll() never reports a change.

#include <baker/callback.hpp>
#include <baker/config.hpp>
#include <baker/symbol.hpp>
#include <baker/text.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace baker::host {

// A file's last write, as a host's stamp hook reports it.
struct FileStamp {
    uint64_t write_ns = 0;  // nanoseconds since 1970; 0 if unknown
    uint64_t size = 0;
    bool present = false;
};

// For a path the Watcher cannot reach on the disk: fills `out` and returns true, or returns
// false if the path is not the hook's, and the disk is tried.
using StampHook = Callback<bool(std::string_view path, FileStamp& out)>;

// For a path a host names virtually ("res://data/tuning.dta", or one relative to its project
// root): writes the file on the disk it is to `out` and returns true, or returns false if it
// has none (a packaged archive's), and the stamp hook or the path as given is used. Asked
// in watch(), never in poll().
using DiskPathHook = Callback<bool(std::string_view path, TextSink out)>;

// The time now, on the clock file write times are read with (nanoseconds since 1970): a
// host gives it to its Config (set_time_source), so a reload's origin and its other stages
// share one clock.
[[nodiscard]] uint64_t file_clock_now_ns() noexcept;

class Watcher {
public:
    Watcher();
    ~Watcher();
    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

    // The paths a host's stamp hook reaches; set it before watch(). Optional.
    void set_stamp_hook(StampHook hook);
    // Where the host's paths are on the disk; set it before watch(). Optional.
    void set_disk_path_hook(DiskPathHook hook);

    // Watches exactly `files` (a Config's stack_files()) and `more` (the project file, say),
    // replacing what it watched before; a directory already watched keeps its handle. A
    // file whose directory the system will not watch (it does not exist yet) is checked by
    // its stamp instead, so watching cannot fail. A directory of layers (StackFile::directory)
    // is watched whole: a file added to it, or removed, reports the directory's path.
    void watch(std::span<const StackFile> files, std::span<const std::string_view> more = {});

    // Once a frame; never blocks. True if a watched file was saved, appeared, or went away
    // since the last poll.
    bool poll();
    // The files the last poll found changed, as they were given to watch().
    [[nodiscard]] std::span<const Symbol> changed() const noexcept;
    // The newest write time among them (file_clock_now_ns's clock); 0 if none could be read
    // (all deleted, or the system's buffer overflowed).
    [[nodiscard]] uint64_t newest_write_ns() const noexcept;
    // How many files it watches, and how many of those by their stamps.
    [[nodiscard]] size_t watched() const noexcept;
    [[nodiscard]] size_t by_stamp() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace baker::host
