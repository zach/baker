// main for the test binaries that need nothing before doctest runs.
#include <doctest.h>

int main(int argc, char** argv) {
    return doctest::Context(argc, argv).run();
}
