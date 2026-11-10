#pragma once

// Console: a game's tools on a loopback socket. Each line a client sends is evaluated in the
// game's Context, between frames, and answered:
//
//   (ID {command})   a request, from a tool: one line of JSON (baker.reply/1.2,
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
//
// A frame pays a bounded share: a poll answers lines until its budget is spent and leaves
// the rest, in order, for the next poll, and a client holding more than the byte cap of
// unanswered input is dropped (ConsoleOptions).

#include <baker/callback.hpp>
#include <baker/error.hpp>
#include <baker/script.hpp>

#include <cstddef>
#include <cstdint>

namespace baker::host {

struct ConsoleOptions {
    // A poll answers lines until this much of it is spent, then leaves the rest for the next
    // poll; it always answers one, so a backlog moves every frame. Read on a steady clock
    // after each line. 0: every complete line, however long it takes.
    uint64_t budget_ns = 500'000;
    // A client holding more than this many bytes of unanswered input is dropped. Its lines
    // wait unanswered while more than this of its replies is unsent, so a client that sends
    // without reading reaches the cap; one reply, however large, is always sent whole.
    // 0: no cap.
    size_t max_bytes = size_t{1} << 20;
    // Told when a client is dropped for the cap. Optional.
    Callback<void(const Error&)> log{};
};

class Console {
public:
    explicit Console(Context& ctx, const ConsoleOptions& options = {});
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    // Listens on 127.0.0.1:`port`; port 0 picks a free one, which port() then says. False if
    // the port is taken or sockets are unavailable, and always under BAKER_SHIPPING.
    [[nodiscard]] bool listen(uint16_t port);
    [[nodiscard]] uint16_t port() const noexcept;
    // Accepts new clients and answers the complete lines they have sent, oldest first, within
    // the budget; the clients take turns at being first. Never blocks.
    void poll();
    // Clients connected now.
    [[nodiscard]] size_t clients() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace baker::host
