// The host kit's console: lines in, replies out, on loopback.
#include <doctest.h>
#include <baker/host/console.hpp>
#include <baker/std_interop.hpp>

#include "alloc_counter.hpp"
#include "host/sockets.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace baker;
using baker::host::Console;
using baker::host::ConsoleOptions;
namespace net = baker::host::net;

namespace {

// A client over loopback, as a tool or a person at `nc` would be.
class Client {
public:
    explicit Client(uint16_t port) {
        s_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        ok_ = s_ != net::kNoSocket &&
              connect(s_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 &&
              net::set_nonblocking(s_);
    }
    ~Client() {
        if (s_ != net::kNoSocket) net::close_socket(s_);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    [[nodiscard]] bool ok() const { return ok_; }
    void send_text(const std::string& text) {
        (void)send(s_, text.data(), static_cast<int>(text.size()), 0);
    }
    // One reply: up to the empty line that ends it, polling the console meanwhile.
    std::string reply(Console& console) {
        for (int i = 0; i < 500; ++i) {
            console.poll();
            char buffer[65536];
            for (;;) {  // everything there is now
                const auto n = recv(s_, buffer, static_cast<int>(sizeof buffer), 0);
                if (n <= 0) break;
                got_.append(buffer, static_cast<size_t>(n));
            }
            if (const size_t end = got_.find("\n\n"); end != std::string::npos) {
                std::string out = got_.substr(0, end);
                got_.erase(0, end + 2);
                return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return "<no reply>";
    }

    // Everything the console has sent so far, without polling it. False once the console
    // has closed the connection.
    bool drain() {
        char buffer[65536];
        for (;;) {
            const auto n = recv(s_, buffer, static_cast<int>(sizeof buffer), 0);
            if (n == 0) return false;
            if (n < 0) return net::would_block();
            got_.append(buffer, static_cast<size_t>(n));
        }
    }
    // The replies drained so far, each without its empty line; taken.
    std::vector<std::string> replies() {
        std::vector<std::string> out;
        for (size_t end; (end = got_.find("\n\n")) != std::string::npos;) {
            out.push_back(got_.substr(0, end));
            got_.erase(0, end + 2);
        }
        return out;
    }

private:
    net::socket_t s_ = net::kNoSocket;
    bool ok_ = false;
    std::string got_;
};

}  // namespace

TEST_CASE("the console answers a person and a tool, each reply ending with an empty line") {
    if constexpr (!kShipping) {
        Context ctx;
        REQUIRE(ctx.func("double", [](int64_t n) { return n * 2; }));
        Console console(ctx);
        REQUIRE(console.listen(0));
        REQUIRE(console.port() != 0);
        Client c(console.port());
        REQUIRE(c.ok());

        c.send_text("{double 21}\n");
        CHECK(c.reply(console) == "=> 42");
        c.send_text("(7 {double 4})\r\n");  // a tool's request; \r\n is a line end too
        CHECK(c.reply(console) == R"({"schema":"baker.reply/1.2","id":7,"ok":true,)"
                                  R"("text":"8","value":8})");
        c.send_text("{double}\n");  // a mistake, for a person: the error and its stack
        const std::string error = c.reply(console);
        CHECK(error.find("'double' takes 1 argument, given 0") != std::string::npos);

        // A line in pieces is answered once it is whole; two lines at once, in order.
        c.send_text("{dou");
        console.poll();
        c.send_text("ble 1}\n{double 2}\n");
        CHECK(c.reply(console) == "=> 2");
        CHECK(c.reply(console) == "=> 4");
    }
}

TEST_CASE("the console serves several clients, and lets one go") {
    if constexpr (!kShipping) {
        Context ctx;
        Console console(ctx);
        REQUIRE(console.listen(0));
        {
            Client a(console.port());
            Client b(console.port());
            REQUIRE((a.ok() && b.ok()));
            a.send_text("{+ 1 1}\n");
            b.send_text("{+ 2 2}\n");
            CHECK(b.reply(console) == "=> 4");
            CHECK(a.reply(console) == "=> 2");
            CHECK(console.clients() == 2);
        }
        for (int i = 0; i < 100 && console.clients() > 0; ++i) {
            console.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(console.clients() == 0);
    }
}

TEST_CASE("a large reply reaches the client whole") {
    if constexpr (!kShipping) {
        Context ctx;
        REQUIRE(
            ctx.func("big", [](int64_t n) { return std::string(static_cast<size_t>(n), 'x'); }));
        Console console(ctx);
        REQUIRE(console.listen(0));
        Client c(console.port());
        REQUIRE(c.ok());
        // A 2 MB string, twice in the reply (its text and its value): more than one send
        // takes, so the console keeps the rest and sends it over the next polls.
        c.send_text("(1 {big 2000000})\n");
        const std::string reply = c.reply(console);
        CHECK(reply.size() > 4'000'000);
        CHECK(reply.rfind(R"({"schema":"baker.reply/1.2","id":1,"ok":true,)", 0) == 0);
    }
}

TEST_CASE("the console listens on loopback only, once") {
    if constexpr (!kShipping) {
        Context ctx;
        Console console(ctx);
        REQUIRE(console.listen(0));
        CHECK_FALSE(console.listen(0));  // one socket a console
        // The port is taken on 127.0.0.1: a second console on it fails.
        Console second(ctx);
        CHECK_FALSE(second.listen(console.port()));
    }
}

TEST_CASE("a quiet frame allocates nothing") {
    if constexpr (!kShipping) {
        Context ctx;
        Console console(ctx);
        REQUIRE(console.listen(0));
        Client c(console.port());
        REQUIRE(c.ok());
        c.send_text("{+ 1 2}\n");
        CHECK(c.reply(console) == "=> 3");
        const test::AllocationScope scope;
        for (int i = 0; i < 10; ++i) console.poll();  // a client, sending nothing
        CHECK(scope.count() == 0);
    }
}

namespace {

// Spins for `us` microseconds: a request of a known cost, which sleep cannot give.
void spin_us(int64_t us) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < until) {
    }
}

// (I {tick I}) for each I in [0, n), as one send.
std::string ticks(int n) {
    std::string text;
    for (int i = 0; i < n; ++i)
        text += "(" + std::to_string(i) + " {tick " + std::to_string(i) + "})\n";
    return text;
}

}  // namespace

TEST_CASE("a backlog is answered over several polls, each within the budget, in order") {
    if constexpr (!kShipping) {
        Context ctx;
        std::vector<int64_t> calls;
        calls.reserve(1000);
        REQUIRE(ctx.func("tick", [&calls](int64_t i) {
            spin_us(10);  // 1,000 of them: 10 ms, twenty polls' budget
            calls.push_back(i);
            return i;
        }));
        Console console(ctx);  // the default budget, 0.5 ms
        REQUIRE(console.listen(0));
        Client c(console.port());
        REQUIRE(c.ok());
        c.send_text(ticks(1000));

        std::vector<std::string> replies;
        int polls = 0;
        double slowest_ms = 0;
        for (int i = 0; i < 5000 && replies.size() < 1000; ++i) {
            const auto start = std::chrono::steady_clock::now();
            console.poll();
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            if (ms > slowest_ms) slowest_ms = ms;
            ++polls;
            REQUIRE(c.drain());
            for (std::string& r : c.replies()) replies.push_back(std::move(r));
            if (replies.size() < 1000) std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        // Each answered once, in order, and each reply its request's.
        REQUIRE(calls.size() == 1000);
        REQUIRE(replies.size() == 1000);
        for (int64_t i = 0; i < 1000; ++i) {
            CHECK(calls[static_cast<size_t>(i)] == i);
            CHECK(replies[static_cast<size_t>(i)] ==
                  R"({"schema":"baker.reply/1.2","id":)" + std::to_string(i) +
                      R"(,"ok":true,"text":")" + std::to_string(i) + R"(","value":)" +
                      std::to_string(i) + "}");
        }
        // 10 ms of work in polls of 0.5 ms: twenty at least. Each poll stops once its budget
        // is spent, so it overruns by one request; the slack is for a loaded CI machine.
        CHECK(polls >= 20);
        MESSAGE("1,000 requests of 10 us: ", polls, " polls, the slowest ", slowest_ms, " ms");
        CHECK(slowest_ms < 50.0);
    }
}

TEST_CASE("a poll answers at least one line, whatever its budget, and 0 is no budget") {
    if constexpr (!kShipping) {
        Context ctx;
        size_t calls = 0;
        REQUIRE(ctx.func("tick", [&calls](int64_t i) {
            spin_us(10);
            ++calls;
            return i;
        }));
        SUBCASE("a budget of 1 ns: one line a poll") {
            Console console(ctx, {.budget_ns = 1});
            REQUIRE(console.listen(0));
            Client c(console.port());
            REQUIRE(c.ok());
            c.send_text(ticks(50));
            for (int i = 0; i < 5000 && calls < 50; ++i) {
                const size_t before = calls;
                console.poll();
                CHECK(calls - before <= 1);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            CHECK(calls == 50);
            REQUIRE(c.drain());
            CHECK(c.replies().size() == 50);
        }
        SUBCASE("no budget: every line there is, in one poll") {
            Console console(ctx, {.budget_ns = 0});
            REQUIRE(console.listen(0));
            Client c(console.port());
            REQUIRE(c.ok());
            c.send_text(ticks(500));
            // The lines may arrive in pieces; whatever a poll has, it answers whole. A budget
            // would answer a few dozen of these 10 us requests a poll.
            size_t most = 0;
            for (int i = 0; i < 1000 && calls < 500; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const size_t before = calls;
                console.poll();
                most = std::max(most, calls - before);
            }
            CHECK(calls == 500);
            CHECK(most > 100);
        }
    }
}

TEST_CASE("a client past the byte cap is dropped, and the others are served") {
    if constexpr (!kShipping) {
        Context ctx;
        std::string logged;
        Console console(ctx, {.max_bytes = 4096, .log = [&logged](const Error& e) {
                                  logged = std::string(e.message());
                              }});
        REQUIRE(console.listen(0));
        Client flood(console.port());
        Client calm(console.port());
        REQUIRE((flood.ok() && calm.ok()));
        flood.send_text(std::string(10000, 'x'));  // one line that never ends
        bool closed = false;
        for (int i = 0; i < 500 && !closed; ++i) {
            console.poll();
            closed = !flood.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(closed);
        CHECK(console.clients() == 1);
        CHECK(logged.find("dropped a client holding more than 4096 bytes") != std::string::npos);
        calm.send_text("{+ 1 2}\n");
        CHECK(calm.reply(console) == "=> 3");
    }
}

TEST_CASE("a client that does not read waits once its unsent replies pass the cap") {
    if constexpr (!kShipping) {
        Context ctx;
        int calls = 0;
        REQUIRE(ctx.func("big", [&calls]() {
            ++calls;
            return std::string(200'000, 'x');
        }));
        Console console(ctx, {.max_bytes = 65536});
        REQUIRE(console.listen(0));
        Client c(console.port());
        REQUIRE(c.ok());
        // 100 replies of 400 KB (the text and the value): 40 MB, more than the sockets
        // hold, so the console stops answering while its replies wait unsent.
        std::string text;
        for (int i = 0; i < 100; ++i) text += "(" + std::to_string(i) + " {big})\n";
        c.send_text(text);
        for (int i = 0; i < 200; ++i) {
            console.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(calls < 100);
        CHECK(console.clients() == 1);  // its input is under the cap: kept
        // Reading, it gets every reply, in order.
        std::vector<std::string> replies;
        for (int i = 0; i < 20000 && replies.size() < 100; ++i) {
            console.poll();
            REQUIRE(c.drain());
            for (std::string& r : c.replies()) replies.push_back(std::move(r));
        }
        REQUIRE(replies.size() == 100);
        for (size_t i = 0; i < replies.size(); ++i)
            CHECK(replies[i].rfind(R"({"schema":"baker.reply/1.2","id":)" + std::to_string(i) + ",",
                                   0) == 0);
        CHECK(calls == 100);
    }
}

TEST_CASE("under BAKER_SHIPPING the console never listens") {
    if constexpr (kShipping) {
        Context ctx;
        Console console(ctx);
        CHECK_FALSE(console.listen(0));
        console.poll();
        CHECK(console.clients() == 0);
    }
}
