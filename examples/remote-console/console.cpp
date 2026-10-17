#include "console.hpp"

#include <baker/std_script.hpp>

Console::~Console() {
    for (Client& c : clients_) close_socket(c.socket);
    if (listener_ != kNoSocket) close_socket(listener_);
}

bool Console::listen(uint16_t port) {
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ == kNoSocket) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    socklen_t size = sizeof addr;
    if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(listener_, 4) != 0 || !set_nonblocking(listener_) ||
        getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &size) != 0)
        return false;
    port_ = ntohs(addr.sin_port);
    return true;
}

void Console::poll() {
    for (socket_t s; (s = accept(listener_, nullptr, nullptr)) != kNoSocket;)
        if (set_nonblocking(s))
            clients_.push_back({s, {}});
        else
            close_socket(s);
    for (size_t i = 0; i < clients_.size();) {
        if (serve(clients_[i])) {
            ++i;
        } else {
            close_socket(clients_[i].socket);
            clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

bool Console::serve(Client& c) {
    char buffer[512];
    for (;;) {
        const auto n = recv(c.socket, buffer, static_cast<int>(sizeof buffer), 0);
        if (n == 0) return false;  // closed
        if (n < 0) {
            if (!would_block()) return false;
            break;
        }
        c.pending.append(buffer, static_cast<size_t>(n));
    }
    for (size_t end; (end = c.pending.find('\n')) != std::string::npos;) {
        std::string line = c.pending.substr(0, end);
        c.pending.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // A reply is one or more lines, then an empty line.
        const std::string reply = answer(line) + "\n\n";
        // A short reply on a local socket is sent whole, or the client is gone.
        const auto sent = send(c.socket, reply.data(), static_cast<int>(reply.size()), 0);
        if (sent < 0 || static_cast<size_t>(sent) != reply.size()) return false;
    }
    return true;
}

std::string Console::answer(const std::string& line) {
    // A request, (ID {command}), gets one line of JSON (baker.reply/1.1) for a tool to read.
    if (const size_t first = line.find_first_not_of(" \t");
        first != std::string::npos && line[first] == '(') {
        std::string json;
        baker::eval_request(ctx_, line, baker::string_out(json));
        json.pop_back();  // its newline: the empty line below ends every reply
        return json;
    }
    // Anything else is for a person: "=> value", or the error with its script stack.
    auto r = ctx_.eval(line, "<console>");
    if (r) return "=> " + baker::to_string(*r);
    std::string error(baker::describe(r.error()));
    if (!error.empty() && error.back() == '\n') error.pop_back();
    return error;
}
