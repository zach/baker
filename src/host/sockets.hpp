// The few socket calls the console needs, on BSD sockets and on Winsock.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace baker::host::net {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kNoSocket = INVALID_SOCKET;
inline bool start() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
inline void stop() {
    WSACleanup();
}
inline void close_socket(socket_t s) {
    closesocket(s);
}
inline bool set_nonblocking(socket_t s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
inline bool would_block() {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}
// Each write goes out at once: without it, a write waits for the acknowledgement of the
// last, which a client may hold back about 40 ms (Nagle's algorithm against delayed ACKs).
inline void set_nodelay(socket_t s) {
    const BOOL on = TRUE;
    (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}
#else
using socket_t = int;
inline constexpr socket_t kNoSocket = -1;
inline bool start() {
    return true;
}
inline void stop() {}
inline void close_socket(socket_t s) {
    ::close(s);
}
inline bool set_nonblocking(socket_t s) {
    return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0;
}
inline bool would_block() {
    return errno == EWOULDBLOCK || errno == EAGAIN;
}
// Each write goes out at once: without it, a write waits for the acknowledgement of the
// last, which a client may hold back about 40 ms (Nagle's algorithm against delayed ACKs).
inline void set_nodelay(socket_t s) {
    const int on = 1;
    (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}
#endif

}  // namespace baker::host::net
