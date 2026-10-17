// A remote console: a game polls it once a frame, and a developer connects with any line
// client (`nc 127.0.0.1 7000`) to evaluate commands in the running game.
//
//   baker_example_remote_console [--port 7000]   serve until interrupted
//   baker_example_remote_console --test          connect over loopback, send four
//                                                commands, print the replies, and exit
#include <baker/script.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "console.hpp"

namespace {

std::atomic<bool> client_done{false};

// What a developer does at the other end: here, three commands from a thread.
void client(uint16_t port) {
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (s == kNoSocket || connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        std::printf("cannot connect\n");
        client_done = true;
        return;
    }
    // Two commands for a person, a mistake, and a request for a tool, answered in JSON.
    for (const char* command :
         {"{set $speed {* 2 4.5}}", "{boost 3}", "{boost}", "(4 {boost 1})"}) {
        const std::string line = std::string(command) + "\n";
        send(s, line.data(), static_cast<int>(line.size()), 0);
        std::string reply;  // up to the empty line that ends it
        for (char c; !reply.ends_with("\n\n") && recv(s, &c, 1, 0) == 1;) reply += c;
        std::printf("> %s\n%s", command, reply.c_str());
    }
    close_socket(s);
    client_done = true;
}

}  // namespace

int main(int argc, char** argv) {
    const bool test = argc > 1 && std::strcmp(argv[1], "--test") == 0;
    const uint16_t port =
        argc > 2 && std::strcmp(argv[1], "--port") == 0 ? uint16_t(std::atoi(argv[2])) : 7000;
    if (!sockets_start()) return 1;

    baker::Context ctx;
    ctx.global("speed", 1.0, "The player's speed.");
    if (!ctx.func("boost", [&ctx](double factor) {
            const double speed = ctx.var("speed")->try_float().value_or(0) * factor;
            ctx.var("speed") = speed;
            return speed;
        }))
        return 1;

    int status = 0;
    {
        Console console(ctx);
        if (!console.listen(test ? 0 : port)) {
            std::printf("cannot listen on port %u\n", unsigned(port));
            return 1;
        }
        if (!test) std::printf("listening on 127.0.0.1:%u\n", unsigned(console.port()));
        std::thread developer;
        if (test) developer = std::thread(client, console.port());
        // The game loop: a frame, then the console.
        while (!test || !client_done) {
            console.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(test ? 1 : 16));
        }
        if (developer.joinable()) developer.join();
        status = ctx.var("speed")->try_float() == 27.0 ? 0 : 1;
    }
    sockets_stop();
    return status;
}
