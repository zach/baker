// The disk's directory listing, LoadSession's default lister. Apart from the parser, so that
// the system's headers and their macros stay out of it.

#include "session.hpp"
#include "text.hpp"

#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace baker::detail {

// Without std::filesystem: each entry but . and .., as `dir` joined with its name.
bool list_disk_dir(std::string_view dir, Callback<void(std::string_view path)> entry) {
    Str path(dir);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    const size_t base = path.size();
    auto each = [&](const char* name) {
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) return;
        path.resize(base);
        path += name;
        entry(path);
    };
#if defined(_WIN32)
    WIN32_FIND_DATAA found;
    const HANDLE h = FindFirstFileA((path + "*").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) return false;
    do each(found.cFileName);
    while (FindNextFileA(h, &found));
    FindClose(h);
#else
    DIR* d = opendir(CPath(dir.empty() ? std::string_view(".") : dir).c_str());
    if (!d) return false;
    while (const dirent* e = readdir(d)) each(e->d_name);
    closedir(d);
#endif
    return true;
}

}  // namespace baker::detail
