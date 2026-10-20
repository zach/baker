// Console (host/console.hpp): lines in, replies out, on 127.0.0.1.

#include <baker/host/console.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"
#include "host/sockets.hpp"

#include <atomic>

namespace baker::host {
namespace {

// Winsock is started once for every console alive, and stopped after the last.
std::atomic<int> g_users{0};

bool start_sockets() {
    if (g_users.fetch_add(1) == 0 && !net::start()) {
        g_users.fetch_sub(1);
        return false;
    }
    return true;
}

void stop_sockets() {
    if (g_users.fetch_sub(1) == 1) net::stop();
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // a client gone mid-reply is not a SIGPIPE
#else
constexpr int kSendFlags = 0;
#endif

}  // namespace

struct Console::Impl {
    struct Client {
        net::socket_t socket = net::kNoSocket;
        Str in;   // received, up to an incomplete line
        Str out;  // replies not yet sent
    };

    explicit Impl(Context& c) : ctx(c) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        for (Client& c : clients) net::close_socket(c.socket);
        if (listener != net::kNoSocket) {
            net::close_socket(listener);
            stop_sockets();
        }
    }

    // The reply to one line, without the empty line that ends it.
    Str answer(std::string_view line) {
        // A request, (ID {command}), gets one line of JSON, for a tool to read.
        if (const size_t first = line.find_first_not_of(" \t");
            first != std::string_view::npos && line[first] == '(') {
            Str json;
            eval_request(ctx, line, detail::str_out(json));
            if (!json.empty() && json.back() == '\n') json.pop_back();
            return json;
        }
        // Anything else is for a person: "=> value", or the error with its script stack.
        auto r = ctx.eval(line, "<console>");
        if (r) return "=> " + detail::text_of(*r);
        Str error(describe(r.error()));
        if (!error.empty() && error.back() == '\n') error.pop_back();
        return error;
    }

    // Reads what the client sent, answers its complete lines, and sends what it can. False
    // once the client has gone.
    bool serve(Client& c) {
        char buffer[512];
        for (;;) {
            const auto n = recv(c.socket, buffer, static_cast<int>(sizeof buffer), 0);
            if (n == 0) return false;  // closed
            if (n < 0) {
                if (!net::would_block()) return false;
                break;
            }
            c.in.append(std::string_view(buffer, static_cast<size_t>(n)));
        }
        size_t from = 0;
        for (size_t end; (end = c.in.find('\n', from)) != Str::npos; from = end + 1) {
            std::string_view line(c.in.data() + from, end - from);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            c.out += answer(line);
            c.out += "\n\n";  // a reply is its lines, then an empty line
        }
        if (from > 0) c.in.erase(0, from);
        return flush(c);
    }

    // Sends as much of the client's pending replies as the socket takes now.
    static bool flush(Client& c) {
        size_t sent = 0;
        while (sent < c.out.size()) {
            const auto n = send(c.socket, c.out.data() + sent,
                                static_cast<int>(c.out.size() - sent), kSendFlags);
            if (n < 0) {
                if (!net::would_block()) return false;
                break;
            }
            sent += static_cast<size_t>(n);
        }
        if (sent > 0) c.out.erase(0, sent);
        return true;
    }

    Context& ctx;
    net::socket_t listener = net::kNoSocket;
    uint16_t port = 0;
    Vec<Client> clients;
};

Console::Console(Context& ctx) : impl_(detail::create<Impl>(default_allocator(), ctx)) {}

Console::~Console() {
    detail::destroy(default_allocator(), impl_);
}

bool Console::listen(uint16_t port) {
    if constexpr (kShipping) {
        (void)port;
        return false;  // a shipped game opens no console
    } else {
        Impl& im = *impl_;
        if (im.listener != net::kNoSocket || !start_sockets()) return false;
        im.listener = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never another machine
        addr.sin_port = htons(port);
        socklen_t size = sizeof addr;
        if (im.listener == net::kNoSocket ||
            bind(im.listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(im.listener, 4) != 0 || !net::set_nonblocking(im.listener) ||
            getsockname(im.listener, reinterpret_cast<sockaddr*>(&addr), &size) != 0) {
            if (im.listener != net::kNoSocket) net::close_socket(im.listener);
            im.listener = net::kNoSocket;
            stop_sockets();
            return false;
        }
        im.port = ntohs(addr.sin_port);
        return true;
    }
}

uint16_t Console::port() const noexcept {
    return impl_->port;
}

size_t Console::clients() const noexcept {
    return impl_->clients.size();
}

void Console::poll() {
    Impl& im = *impl_;
    if (im.listener == net::kNoSocket) return;
    for (net::socket_t s; (s = accept(im.listener, nullptr, nullptr)) != net::kNoSocket;) {
        if (!net::set_nonblocking(s)) {
            net::close_socket(s);
            continue;
        }
        // A frame's replies are one write; the next frame's must not wait on the client's
        // acknowledgement of it, or a tool's many requests take 40 ms a frame.
        net::set_nodelay(s);
        im.clients.push_back({s, {}, {}});
    }
    for (size_t i = 0; i < im.clients.size();) {
        if (im.serve(im.clients[i])) {
            ++i;
        } else {
            net::close_socket(im.clients[i].socket);
            im.clients.erase(im.clients.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

}  // namespace baker::host
