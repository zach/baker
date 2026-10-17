// Runs the quickstart as a reader would: copies default.dta into the working directory,
// starts the program, edits the file, and presses Enter. Standard input is a stream buffer
// whose first read makes the edit, so nothing depends on timing.
//
//   baker_example_quickstart_test path/to/examples/quickstart/default.dta
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>

#define main quickstart_main
#include "main.cpp"
#undef main

namespace {

std::string original;

bool write_file(const std::string& text) {
    std::ofstream out("default.dta", std::ios::binary | std::ios::trunc);
    out << text;
    return bool(out);
}

// Each read is one press of Enter: the first after changing the speed from 4.5 to 6, the second
// after breaking the file (a reload that fails changes nothing), then end of input.
class Keyboard : public std::streambuf {
protected:
    int_type underflow() override {
        if (presses_ == 2) return traits_type::eof();
        std::string text = original;
        if (presses_++ == 0)
            text.replace(text.find("speed 4.5"), 9, "speed 6");
        else
            text = "(player (speed 7";
        write_file(text);
        setg(newline_, newline_, newline_ + 1);
        return traits_type::to_int_type(*gptr());
    }

private:
    char newline_[1] = {'\n'};
    int presses_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream in(argv[1], std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    original = text.str();
    if (original.find("speed 4.5") == std::string::npos || !write_file(original)) return 2;
    Keyboard keyboard;
    std::cin.rdbuf(&keyboard);
    return quickstart_main();
}
