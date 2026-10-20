// The few socket calls the example's client needs, on BSD sockets and on Winsock.
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
using socket_t = SOCKET;
inline constexpr socket_t kNoSocket = INVALID_SOCKET;
inline bool sockets_start() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
inline void sockets_stop() {
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
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using socket_t = int;
inline constexpr socket_t kNoSocket = -1;
inline bool sockets_start() {
    return true;
}
inline void sockets_stop() {}
inline void close_socket(socket_t s) {
    ::close(s);
}
inline bool set_nonblocking(socket_t s) {
    return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0;
}
inline bool would_block() {
    return errno == EWOULDBLOCK || errno == EAGAIN;
}
#endif
