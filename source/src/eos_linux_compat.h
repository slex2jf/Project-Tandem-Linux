#pragma once

#ifndef _WIN32

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

using SOCKET = int;
using BOOL = int;
using u_long = unsigned long;

constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
constexpr int WSAEWOULDBLOCK = EWOULDBLOCK;
constexpr int WSAECONNRESET = ECONNRESET;
constexpr int WSAECONNABORTED = ECONNABORTED;
constexpr int SO_EXCLUSIVEADDRUSE = SO_REUSEADDR;

inline int WSAGetLastError() { return errno; }
inline int closesocket(SOCKET s) { return close(s); }
inline int ioctlsocket(SOCKET s, long command, u_long* value) {
    if(command == FIONBIO) {
        int flags = fcntl(s, F_GETFL, 0);
        if(flags < 0) return -1;
        return fcntl(s, F_SETFL,
                     *value ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
    }
    return ioctl(s, command, value);
}

struct WSADATA {};
inline unsigned MAKEWORD(unsigned a,unsigned b) {
    return a | (b << 8);
}
inline int WSAStartup(unsigned,WSADATA*) { return 0; }
inline int WSACleanup() { return 0; }

#endif
