#pragma once

// Test helpers for files on disk: a scratch directory and a line diff.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace baker::test {

// A fresh directory of files for one test.
class TempDir {
public:
    TempDir() {
        // The process's own tag: test binaries run in parallel under ctest -j, and an
        // unseeded rand() gave every process the same names, so one could delete another's.
        static const unsigned process = std::random_device{}();
        static std::atomic<int> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("baker_test_" + std::to_string(process) + "_" + std::to_string(counter++));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::string write(const std::string& name, const std::string& text) const {
        const std::filesystem::path p = dir_ / name;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
        return p.generic_string();
    }
    [[nodiscard]] std::string read(const std::string& name) const {
        std::ifstream in(dir_ / name, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    [[nodiscard]] bool exists(const std::string& name) const {
        return std::filesystem::exists(dir_ / name);
    }
    [[nodiscard]] std::string path(const std::string& name) const {
        return (dir_ / name).generic_string();
    }

private:
    std::filesystem::path dir_;
};

// The lines that differ between two texts, as "-old" / "+new".
inline std::vector<std::string> diff_lines(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<std::string> lines;
        std::stringstream ss(s);
        for (std::string l; std::getline(ss, l);) lines.push_back(l);
        return lines;
    };
    const auto la = split(a), lb = split(b);
    std::vector<std::string> out;
    for (size_t i = 0; i < std::max(la.size(), lb.size()); ++i) {
        const std::string x = i < la.size() ? la[i] : "<none>";
        const std::string y = i < lb.size() ? lb[i] : "<none>";
        if (x != y) {
            out.push_back("-" + x);
            out.push_back("+" + y);
        }
    }
    return out;
}

}  // namespace baker::test
