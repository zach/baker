// Parses every .dta file under the given paths and reports which ones fail.
//
//   baker_parse_corpus [--roundtrip] [--milo-paths] [-D NAME]... [-I DIR]... PATH...
//
// Use it to check the parser against a local checkout of a real DTA corpus (for example
// mariteaux/milo-script-library). Nothing from such a corpus is part of this repository.
// Each file is read with a fresh session; includes resolve
// against the file's own directory, then each -I directory, then the PATH it was found
// under. Exits 1 if any file fails.
//
// --milo-paths reads the milo-script-library dump as its archives were laid out. The
// dump stores a `..` that climbs above an archive's root as a directory named "(..)", so an
// include that climbs out of the archive is looked up there. Some files the scripts include
// are not in the dump at all: each is read as empty and counted, so the run measures the
// parser, not the dump.

#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
// The nearest directory above `file` that holds a "(..)" directory: the archive's root.
fs::path archive_root(const fs::path& file) {
    for (fs::path p = file.parent_path(); p.has_relative_path(); p = p.parent_path()) {
        if (p.filename() == "(..)") continue;
        std::error_code ec;
        if (fs::is_directory(p / "(..)", ec)) return p;
    }
    return {};
}

// `path`, if it lies outside `root`, moved under root's "(..)" directories.
std::string under_root(const fs::path& root, const std::string& path) {
    const fs::path p(path);
    auto r = root.begin();
    auto q = p.begin();
    while (r != root.end() && q != p.end() && *r == *q) ++r, ++q;
    if (r == root.end()) return path;
    fs::path out = root;
    for (; r != root.end(); ++r)
        if (!r->empty()) out /= "(..)";
    for (; q != p.end(); ++q) out /= *q;
    return out.generic_string();
}

std::string normal(const fs::path& p) {
    return p.lexically_normal().generic_string();
}

// The infix rule's gate: how many braces of a corpus the rule rewrote (`fired`), and how
// many it left alone only because no operand was a number, variable, property or command
// (`guarded`: an operator at slot 1 among bare symbols).
struct InfixCount {
    uint64_t fired = 0, guarded = 0, refused = 0;  // refused: files the rule fails, malformed
};
bool refused_by_infix(const baker::Error& e) {
    const std::string_view m = e.message();
    return e.code() == baker::ErrorCode::Syntax &&
           (m.starts_with("infix {...}") || m.starts_with("mixed operators in {...}"));
}
bool operator_symbol(std::string_view s) {
    return !s.empty() && s.find_first_not_of("+-*/%<>=!&|^~") == std::string_view::npos;
}
bool operand(const baker::Node& n) {
    const baker::Type t = n.type();
    return t == baker::Type::Int || t == baker::Type::Float || t == baker::Type::Var ||
           t == baker::Type::Property || t == baker::Type::Command;
}
void count_infix(const baker::Array& a, bool command, InfixCount& c) {
    if (command && a.written_infix()) {
        ++c.fired;
    } else if (command && a.size() >= 3 && (a.size() & 1)) {
        const auto op = a[1].try_sym();
        bool any = false;
        for (uint32_t i = 0; i < a.size(); i += 2) any = any || operand(a[i]);
        if (op && operator_symbol(op->str()) && !any) ++c.guarded;
    }
    for (const baker::Node& n : a)
        if (const baker::Array* child = n.array())
            count_infix(*child, n.type() == baker::Type::Command, c);
}

bool fits(const baker::Node& n, uint64_t& budget) {
    if (budget == 0) return false;
    --budget;
    if (const baker::Array* a = n.array())
        for (const baker::Node& c : *a)
            if (!fits(c, budget)) return false;
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    bool roundtrip = false, milo = false;
    std::vector<std::string> defines, include_dirs, roots;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--roundtrip") {
            roundtrip = true;
        } else if (arg == "--milo-paths") {
            milo = true;
        } else if (arg == "-D" && i + 1 < argc) {
            defines.emplace_back(argv[++i]);
        } else if (arg == "-I" && i + 1 < argc) {
            include_dirs.emplace_back(argv[++i]);
        } else if (arg == "-h" || arg == "--help" || arg[0] == '-') {
            std::fprintf(
                stderr, "usage: %s [--roundtrip] [--milo-paths] [-D NAME]... [-I DIR]... PATH...\n",
                argv[0]);
            return 2;
        } else {
            roots.push_back(arg);
        }
    }
    if (roots.empty()) {
        std::fprintf(stderr,
                     "usage: %s [--roundtrip] [--milo-paths] [-D NAME]... [-I DIR]... PATH...\n",
                     argv[0]);
        return 2;
    }

    struct Item {
        fs::path file;
        fs::path root;
    };
    std::vector<Item> items;
    for (const std::string& r : roots) {
        std::error_code ec;
        if (fs::is_regular_file(r, ec)) {
            items.push_back({r, fs::path(r).parent_path()});
            continue;
        }
        for (auto it = fs::recursive_directory_iterator(r, ec); !ec && it != fs::end(it);
             it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".dta")
                items.push_back({it->path(), r});
        }
    }
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.file < b.file; });

    size_t ok = 0, failed = 0, roundtrip_failed = 0, stubbed_files = 0;
    InfixCount infix;
    std::vector<std::string> absent_all;
    for (const Item& item : items) {
        // With --milo-paths, includes the dump lacks, read as empty.
        std::vector<std::string> absent;
        const fs::path archive = milo ? archive_root(item.file) : fs::path();
        auto load = [&] {
            baker::LoadSession session;
            for (const std::string& d : include_dirs) session.add_include_dir(d);
            session.add_include_dir(item.root.generic_string());
            for (const std::string& d : defines) session.define(baker::Symbol(d));
            if (milo)
                session.set_reader([&](std::string_view path, baker::TextSink out) {
                    std::string p(path);
                    std::replace(p.begin(), p.end(), '\\', '/');
                    p = normal(p);
                    if (!archive.empty()) p = under_root(archive, p);
                    if (std::find(absent.begin(), absent.end(), p) != absent.end()) return true;
                    std::FILE* f = std::fopen(p.c_str(), "rb");
                    if (!f) return false;
                    char buf[65536];
                    size_t got = 0;
                    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0)
                        out(std::string_view(buf, got));
                    std::fclose(f);
                    return true;
                });
            return baker::read_file(item.file.generic_string(), session);
        };
        auto root = load();
        for (int retry = 0; milo && !root && retry < 4096; ++retry) {
            const baker::Error& e = root.error();
            const std::string_view m = e.message();
            constexpr std::string_view kMissing = "cannot read included file '";
            if (e.code() != baker::ErrorCode::Io || m.substr(0, kMissing.size()) != kMissing) break;
            std::string inc(m.substr(kMissing.size(), m.size() - kMissing.size() - 1));
            std::replace(inc.begin(), inc.end(), '\\', '/');  // Baker reads both separators
            std::string p = normal(fs::path(std::string(e.file().str())).parent_path() / inc);
            if (!archive.empty()) p = under_root(archive, p);
            if (std::find(absent.begin(), absent.end(), p) != absent.end()) break;
            absent.push_back(p);
            root = load();
        }
        if (!absent.empty()) {
            ++stubbed_files;
            absent_all.insert(absent_all.end(), absent.begin(), absent.end());
        }
        if (!root) {
            ++failed;
            if (refused_by_infix(root.error())) ++infix.refused;
            std::printf("FAIL %s\n     %s\n", item.file.generic_string().c_str(),
                        baker::to_string(root.error()).data());
            continue;
        }
        ++ok;
        count_infix(**root, false, infix);
        if (!roundtrip) continue;
        uint64_t budget = 10'000'000;
        bool small = true;
        for (const baker::Node& n : **root) small = small && fits(n, budget);
        if (!small) continue;
        auto again = baker::read_string(baker::to_dta(**root), "printed.dta");
        if (!again || !baker::structurally_equal(**root, **again)) {
            ++roundtrip_failed;
            std::printf("ROUNDTRIP %s\n", item.file.generic_string().c_str());
        }
    }
    const size_t total = ok + failed;
    std::printf("%zu/%zu files parsed (%.2f%%)", ok, total,
                total ? 100.0 * static_cast<double>(ok) / static_cast<double>(total) : 100.0);
    if (roundtrip) std::printf(", %zu round-trip failures", roundtrip_failed);
    std::printf(
        "; infix rule: %llu braces fired, %llu held back by the guard, %llu files "
        "refused as malformed infix",
        static_cast<unsigned long long>(infix.fired),
        static_cast<unsigned long long>(infix.guarded),
        static_cast<unsigned long long>(infix.refused));
    if (milo) {
        std::sort(absent_all.begin(), absent_all.end());
        absent_all.erase(std::unique(absent_all.begin(), absent_all.end()), absent_all.end());
        std::printf("; %zu files include one of %zu files the dump lacks", stubbed_files,
                    absent_all.size());
    }
    std::printf("\n");
    return failed == 0 && roundtrip_failed == 0 ? 0 : 1;
}
