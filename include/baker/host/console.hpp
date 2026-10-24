#pragma once

// Console: a game's tools on a loopback socket. Each line a client sends is evaluated in the
// game's Context, between frames, and answered:
//
//   (ID {command})   a request, from a tool: one line of JSON (baker.reply/1.1,
//                    eval_request, docs/reply-json.md)
//   {command}        anything else, from a person: "=> value", or the error and its script
//                    stack
//
// Every reply is its lines, then an empty line, so a client reads up to the empty line
// whatever it sent; a tool that sends only requests may also read one line and skip empty
// ones. Lines end with "\n"; a "\r" before it is dropped.
//
//   baker::host::Console console(ctx);
//   if (!console.listen(4555)) ...;   // 127.0.0.1 only
//   each frame: console.poll();       // on the thread that owns the Context
//
// It listens on 127.0.0.1 only: a console runs any command it is sent, so it is never
// reachable from another machine. Bind the Config's tools first (tools::bind_config_tools) to
// offer them. A quiet frame (no new client, no complete line) allocates nothing. Under
// BAKER_SHIPPING it is inert: listen() refuses, and nothing opens a socket.

#include <baker/script.hpp>

#include <cstddef>
#include <cstdint>

namespace baker::host {

class Console {
public:
    explicit Console(Context& ctx);
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    // Listens on 127.0.0.1:`port`; port 0 picks a free one, which port() then says. False if
    // the port is taken or sockets are unavailable, and always under BAKER_SHIPPING.
    [[nodiscard]] bool listen(uint16_t port);
    [[nodiscard]] uint16_t port() const noexcept;
    // Accepts new clients and answers every complete line they have sent. Never blocks.
    void poll();
    // Clients connected now.
    [[nodiscard]] size_t clients() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace baker::host
