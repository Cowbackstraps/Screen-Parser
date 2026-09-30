/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Cross-platform BSD-socket shims.
//
// The plain-http:// transport in remote_vlm_engine.cpp is written against POSIX
// sockets. This header maps every primitive 1:1 onto the original syscall on
// POSIX (so on-device behavior is unchanged) and onto Winsock2 on Windows dev
// machines. That lets the SAME production transport (SocketHttpTransport) run in
// host-side live tests over real TCP, instead of only against injected fakes.
//
// Scope: no new behavior, only portability. Used by the SCREENPARSER_ENABLE_
// REMOTE_VLM transport and the host live-transport test / mock server.

#ifndef FOUNDATION_SCREENPARSER_COMMON_SOCKET_COMPAT_H
#define FOUNDATION_SCREENPARSER_COMMON_SOCKET_COMPAT_H

#include <cstddef>
#include <string>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32")  // MinGW/g++ links ws2_32 via CMake instead.
#endif

namespace OHOS {
namespace ScreenParser {
namespace socketcompat {

using SockFd = SOCKET;
constexpr SockFd kInvalidSock = INVALID_SOCKET;

inline bool SockValid(SockFd s) { return s != INVALID_SOCKET; }

// Idempotent WSAStartup guard (a no-op on POSIX).
inline void EnsureSockets() {
    static const struct WsaInit {
        WsaInit() {
            WSADATA data;
            ::WSAStartup(MAKEWORD(2, 2), &data);
        }
    } init;
    (void)init;
}

inline int CloseSock(SockFd s) { return ::closesocket(s); }
inline int ShutdownSock(SockFd s) { return ::shutdown(s, SD_BOTH); }
inline int SendSock(SockFd s, const char *data, size_t len) {
    return ::send(s, data, static_cast<int>(len), 0);
}
inline int RecvSock(SockFd s, char *buf, size_t len) {
    return ::recv(s, buf, static_cast<int>(len), 0);
}

// Winsock SO_RCVTIMEO / SO_SNDTIMEO take a DWORD of milliseconds.
inline void SetTimeouts(SockFd s, int timeoutMs) {
    DWORD ms = static_cast<DWORD>(timeoutMs > 0 ? timeoutMs : 0);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
}

inline void SetReuseAddr(SockFd s) {
    BOOL opt = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
}

inline std::string LastSockError() {
    return "socket error " + std::to_string(::WSAGetLastError());
}

}  // namespace socketcompat
}  // namespace ScreenParser
}  // namespace OHOS

#else  // POSIX (OpenHarmony device)

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace OHOS {
namespace ScreenParser {
namespace socketcompat {

using SockFd = int;
constexpr SockFd kInvalidSock = -1;

inline bool SockValid(SockFd s) { return s >= 0; }
inline void EnsureSockets() {}
inline int CloseSock(SockFd s) { return ::close(s); }
inline int ShutdownSock(SockFd s) { return ::shutdown(s, SHUT_RDWR); }
inline int SendSock(SockFd s, const char *data, size_t len) {
    return static_cast<int>(::send(s, data, len, 0));
}
inline int RecvSock(SockFd s, char *buf, size_t len) {
    return static_cast<int>(::recv(s, buf, len, 0));
}

inline void SetTimeouts(SockFd s, int timeoutMs) {
    timeval tv {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

inline void SetReuseAddr(SockFd s) {
    int opt = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

inline std::string LastSockError() { return std::strerror(errno); }

}  // namespace socketcompat
}  // namespace ScreenParser
}  // namespace OHOS

#endif  // _WIN32

namespace OHOS {
namespace ScreenParser {
namespace socketcompat {

// Local port bound to a listening socket. The host mock server binds port 0 to
// obtain an ephemeral port, then reads it back with this. Returns 0 on failure.
inline int LocalPort(SockFd s) {
    sockaddr_in addr {};
    socklen_t len = sizeof(addr);
    if (::getsockname(s, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        return 0;
    }
    return static_cast<int>(ntohs(addr.sin_port));
}

}  // namespace socketcompat
}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_COMMON_SOCKET_COMPAT_H
