// A fatal error goes to the host's handler, with the script stack, then aborts. Each
// case runs in a child process, since a fatal error ends one; POSIX only.
#include <doctest.h>
#include <baker/allocator.hpp>
#include <baker/error.hpp>
#include <baker/script.hpp>

#include <string>
#include "ok.hpp"

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>

using namespace baker;
using baker::test::ok;

namespace {

int message_fd = -1;  // the child's end of the pipe

// What a child did: how it ended, and what its handler or stderr wrote.
struct Child {
    int status = 0;
    std::string output;
    [[nodiscard]] bool exited(int code) const {
        return WIFEXITED(status) && WEXITSTATUS(status) == code;
    }
    [[nodiscard]] bool aborted() const {
        return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    }
};

template <class F>
Child run_child(F body, bool capture_stderr = false) {
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        std::signal(SIGABRT, SIG_DFL);  // the test framework's crash report is the parent's
        message_fd = fds[1];
        if (capture_stderr) dup2(fds[1], 2);
        body();
        _exit(0);
    }
    close(fds[1]);
    Child c;
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;)
        c.output.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    waitpid(pid, &c.status, 0);
    return c;
}

// A host's handler: the message to the pipe, then the process ends, as a crash reporter would.
void report_and_exit(std::string_view message) {
    (void)!write(message_fd, message.data(), message.size());
    _exit(42);
}

void report_and_return(std::string_view message) {
    (void)!write(message_fd, message.data(), message.size());
}

// An allocator that has nothing to give.
const Allocator kEmpty{[](void*, size_t, size_t, baker::MemoryKind) -> void* { return nullptr; },
                       [](void*, void*, size_t, baker::MemoryKind) {}, nullptr};

}  // namespace

TEST_CASE("a fatal error goes to the host's handler, which ends the process") {
    const Child c = run_child([] {
        set_fatal_handler(report_and_exit);
        (void)detail::allocate(kEmpty, 24, 8);
    });
    CHECK(c.exited(42));
    CHECK(c.output == "baker: fatal: an allocator returned null, asked for 24 bytes");
}

TEST_CASE("a fatal error inside a script call carries the script stack") {
    const Child c = run_child([] {
        set_fatal_handler(report_and_exit);
        Context ctx;
        ok(ctx.func("exhaust", [] { return detail::allocate(kEmpty, 16, 8) != nullptr; }));
        ok(ctx.eval("{func spawn_wave () {exhaust}}"));
        (void)ctx.eval("{spawn_wave}", "level.dta");
    });
    CHECK(c.exited(42));
    CHECK(c.output.find("baker: fatal: an allocator returned null, asked for 16 bytes\nin ") == 0);
    CHECK(c.output.find("{exhaust}") != std::string::npos);
    CHECK(c.output.find("{spawn_wave}") != std::string::npos);
    CHECK(c.output.find("{exhaust}") < c.output.find("{spawn_wave}"));  // innermost first
}

#if !BAKER_HAS_STD_EXPECTED
TEST_CASE("reading the value of an Expected that holds an error is fatal, with the error") {
    const Child c = run_child([] {
        set_fatal_handler(report_and_exit);
        const Expected<int> missing = Unexpected(make_error(ErrorCode::NotFound, "no 'speed'"));
        (void)missing.value();
    });
    CHECK(c.exited(42));
    CHECK(c.output ==
          "baker: fatal: the value of an Expected that holds an error was read: baker: no 'speed'");
}
#endif

TEST_CASE("a handler that returns does not keep a fatal error from aborting") {
    const Child c = run_child([] {
        set_fatal_handler(report_and_return);
        (void)detail::allocate(kEmpty, 8, 8);
    });
    CHECK(c.aborted());
    CHECK(c.output.find("an allocator returned null") != std::string::npos);
}

TEST_CASE("without a handler a fatal error is written to stderr, and aborts") {
    const Child c = run_child([] { (void)detail::allocate(kEmpty, 8, 8); }, true);
    CHECK(c.aborted());
    CHECK(c.output == "baker: fatal: an allocator returned null, asked for 8 bytes\n");
    // Setting a handler returns the one it replaces; null restores the default.
    CHECK(set_fatal_handler(report_and_exit) == nullptr);
    CHECK(set_fatal_handler(nullptr) == report_and_exit);
}
#endif
