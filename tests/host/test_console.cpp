// The host kit's console: lines in, replies out, on loopback.
#include <doctest.h>
#include <baker/host/console.hpp>
#include <baker/std_interop.hpp>

#include "alloc_counter.hpp"
#include "host/sockets.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace baker;
using baker::host::Console;
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
        CHECK(c.reply(console) == R"({"schema":"baker.reply/1.1","id":7,"ok":true,)"
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
        CHECK(reply.rfind(R"({"schema":"baker.reply/1.1","id":1,"ok":true,)", 0) == 0);
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

TEST_CASE("under BAKER_SHIPPING the console never listens") {
    if constexpr (kShipping) {
        Context ctx;
        Console console(ctx);
        CHECK_FALSE(console.listen(0));
        console.poll();
        CHECK(console.clients() == 0);
    }
}
