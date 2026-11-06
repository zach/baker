// Rewrites old Baker text into the current language: every migration Baker has, each finding
// and its edits from baker::tools::migrate_text, applied over the files given.
//
//   baker_migrate data/default.dta                           print the rewritten text
//   baker_migrate --write $(git ls-files '*.dta')            rewrite in place
//   baker_migrate --report $(git ls-files '*.dta')           what would change, for review
//   baker_migrate --check $(git ls-files '*.dta')            exit 1 if anything would change
//   baker_migrate --only metadata --write data/*.dta         one migration
//   baker_migrate --write --cpp tests/data/test_config.cpp   DTA in C++ string literals
//   baker_migrate --write --md docs/language.md              DTA in Markdown
//
// The migrations are named in <baker/tools/migrate.hpp>: metadata, (range 0 10) after a value
// becomes (@range 0 10); declarations, (pack ...), (origin ...), (shapes ...) and
// #define NAME (body) become #pack, #origin, #shape and [NAME body]. One pass does both, so
// their order does not matter, and a second run changes nothing.
//
// What a migration will not decide, it reports for review: a tagged array with no untagged
// value whose last child is tagged with a metadata word, (limits (min 0) (step 0.5)), and in
// --md a top-level form tagged with one, `(range 0 10)` in prose.
//
// --cpp reads each run of adjacent C++ string literals, as the compiler joins them, as one
// DTA text: escapes decoded, raw strings as written, comments and character literals
// skipped; a (shapes ...) there splits onto its line. --md reads ```dta and unmarked fenced
// blocks and inline code spans as DTA, ```cpp blocks as C++, and skips blocks in other
// languages. Every byte keeps its offset in the file, so an edit lands in the right literal;
// read the diff all the same.

#include <baker/tools/migrate.hpp>

#include "migrate_runs.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using namespace baker::migrate;
using baker::tools::Migration;
using baker::tools::MigrationFinding;

struct FileEdit {
    size_t begin, end;
    std::string_view text;
};
struct Review {
    size_t line;
    std::string text;
};

// A file's edits, by offset in the file, and its findings per migration and for review.
struct FileMigration {
    std::vector<FileEdit> edits;
    std::vector<Review> review;
    size_t by[baker::tools::kMigrations] = {};

    FileMigration(std::string_view src, Mode mode, uint32_t migrations) {
        for (const Run& run : runs_of(src, mode)) {
            const baker::tools::Migrated m =
                baker::tools::migrate_text(run.text, {},
                                           {.migrations = migrations,
                                            .fixed_lines = mode == Mode::Cpp,
                                            .prose = mode == Mode::Md});
            for (const MigrationFinding& f : m.findings) {
                if (f.edits.empty()) {
                    const size_t at = run.where(f.begin);
                    const auto line =
                        static_cast<size_t>(std::count(
                            src.begin(), src.begin() + static_cast<ptrdiff_t>(at), '\n')) +
                        1;
                    review.push_back({line, std::string(f.warning.message())});
                    continue;
                }
                ++by[static_cast<size_t>(f.migration)];
                for (const baker::tools::TextEdit& e : f.edits) {
                    const size_t begin = run.where(e.begin);
                    const size_t end = e.end == e.begin ? begin : run.where(e.end - 1) + 1;
                    edits.push_back({begin, end, e.text});
                }
            }
        }
        std::stable_sort(edits.begin(), edits.end(), [](const FileEdit& a, const FileEdit& b) {
            return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
        });
    }

    [[nodiscard]] std::string apply(std::string_view src) const {
        std::string out;
        out.reserve(src.size() + edits.size());
        size_t from = 0;
        for (const FileEdit& e : edits) {
            out.append(src.substr(from, e.begin - from));
            out.append(e.text);
            from = e.end;
        }
        out.append(src.substr(from));
        return out;
    }
};

std::string counts(const size_t (&by)[baker::tools::kMigrations]) {
    std::string out;
    for (size_t i = 0; i < baker::tools::kMigrations; ++i) {
        if (by[i] == 0) continue;
        if (!out.empty()) out += ", ";
        out += std::string(baker::tools::migration_name(static_cast<Migration>(i))) + " " +
               std::to_string(by[i]);
    }
    return out;
}

constexpr const char* kUsage =
    "usage: %s [--write | --report | --check] [--only MIGRATION] [--cpp | --md] FILE...\n"
    "migrations: metadata, declarations\n";

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // The text as migrated, byte for byte: a text-mode stdout would turn a CRLF text's \r\n
    // into \r\r\n.
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    bool write = false, report = false, check = false;
    uint32_t migrations = 0;
    Mode mode = Mode::Dta;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--write") {
            write = true;
        } else if (arg == "--report") {
            report = true;
        } else if (arg == "--check") {
            check = true;
        } else if (arg == "--cpp") {
            mode = Mode::Cpp;
        } else if (arg == "--md") {
            mode = mode == Mode::Cpp ? Mode::Cpp : Mode::Md;  // --cpp wins
        } else if (arg == "--only" && i + 1 < argc) {
            const std::string_view name = argv[++i];
            size_t k = 0;
            while (k < baker::tools::kMigrations &&
                   name != baker::tools::migration_name(static_cast<Migration>(k)))
                ++k;
            if (k == baker::tools::kMigrations) {
                std::fprintf(stderr, kUsage, argv[0]);
                return 2;
            }
            migrations |= 1U << k;
        } else if (arg.substr(0, 1) == "-" && arg != "-") {
            std::fprintf(stderr, kUsage, argv[0]);
            return 2;
        } else {
            files.emplace_back(arg);
        }
    }
    if (files.empty() || (write && (report || check))) {
        std::fprintf(stderr, kUsage, argv[0]);
        return 2;
    }
    if (migrations == 0) migrations = (1U << baker::tools::kMigrations) - 1;
    size_t total[baker::tools::kMigrations] = {};
    size_t review_count = 0, changed = 0;
    for (const std::string& path : files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return 1;
        }
        const std::string src((std::istreambuf_iterator<char>(in)), {});
        const FileMigration m(src, mode, migrations);
        const std::string out = m.apply(src);
        changed += out != src ? 1U : 0U;
        for (size_t i = 0; i < baker::tools::kMigrations; ++i) total[i] += m.by[i];
        review_count += m.review.size();
        if (report || check) {
            if (out != src || !m.review.empty())
                std::printf(
                    "%s: %s; %+lld bytes\n", path.c_str(),
                    out != src ? counts(m.by).c_str() : "nothing to rewrite",
                    static_cast<long long>(out.size()) - static_cast<long long>(src.size()));
            for (const Review& r : m.review)
                std::printf("  review %s:%zu: %s\n", path.c_str(), r.line, r.text.c_str());
            continue;
        }
        if (write && out != src) {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f.write(out.data(), static_cast<std::streamsize>(out.size()));
            if (!f) {
                std::fprintf(stderr, "cannot write %s\n", path.c_str());
                return 1;
            }
        }
        if (!write) std::fwrite(out.data(), 1, out.size(), stdout);
        for (const Review& r : m.review)
            std::fprintf(stderr, "review %s:%zu: %s\n", path.c_str(), r.line, r.text.c_str());
    }
    if (report || check) {
        const std::string by = counts(total);
        std::printf("total: %s in %zu of %zu files; %zu to review\n",
                    by.empty() ? "nothing to rewrite" : by.c_str(), changed, files.size(),
                    review_count);
    }
    return check && changed > 0 ? 1 : 0;
}
