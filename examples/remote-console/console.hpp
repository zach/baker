// A developer console over TCP: each line a client sends is evaluated, and the result, or
// the error with its script stack, is sent back, followed by an empty line. A line in
// request form, (ID {command}), is answered with one line of JSON instead (baker.reply/1.1,
// docs/reply-json.md), for tools. poll() is called once a
// frame, on the thread that owns the Context, so commands run between frames like any other script.
#pragma once

#include <baker/script.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "sockets.hpp"

class Console {
public:
    explicit Console(baker::Context& ctx) : ctx_(ctx) {}
    ~Console();
    // Listens on 127.0.0.1 only: a console runs any command it is sent. Port 0 picks a
    // free port; port() says which.
    bool listen(uint16_t port);
    [[nodiscard]] uint16_t port() const { return port_; }
    // Accepts clients and answers every complete line they have sent. Never blocks.
    void poll();

private:
    struct Client {
        socket_t socket;
        std::string pending;  // received, up to an incomplete line
    };
    bool serve(Client& c);  // false once the client has gone
    std::string answer(const std::string& line);

    baker::Context& ctx_;
    socket_t listener_ = kNoSocket;
    uint16_t port_ = 0;
    std::vector<Client> clients_;
};
