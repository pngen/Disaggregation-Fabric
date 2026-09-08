// Disaggregation Fabric — Windows TCP socket wrapper (header-only).
// Uses WinSock2 with defensive, non-interactive defaults. Never discards a
// socket handle: every accepted/connected socket is owned and closed exactly.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <cstdio>

namespace dfabric {
namespace net {

inline void WsaInit() {
  static bool inited = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
  (void)inited;
}

inline std::string WsaError() {
  int e = WSAGetLastError();
  char buf[256];
  snprintf(buf, sizeof(buf), "winsock error %d", e);
  return std::string(buf);
}

class TcpSocket {
 public:
  TcpSocket() = default;
  explicit TcpSocket(SOCKET s) : s_(s) {}
  ~TcpSocket() { Close(); }
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;
  TcpSocket(TcpSocket&& o) noexcept { s_ = o.s_; o.s_ = INVALID_SOCKET; }
  TcpSocket& operator=(TcpSocket&& o) noexcept {
    if (this != &o) { Close(); s_ = o.s_; o.s_ = INVALID_SOCKET; }
    return *this;
  }
  void Close() { if (s_ != INVALID_SOCKET) { closesocket(s_); s_ = INVALID_SOCKET; } }
  bool Valid() const { return s_ != INVALID_SOCKET; }
  SOCKET Get() const { return s_; }

  static TcpSocket Connect(const std::string& host, int port) {
    WsaInit();
    struct addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) throw std::runtime_error("getaddrinfo failed: " + WsaError());
    SOCKET s = INVALID_SOCKET;
    for (auto* p = res; p; p = p->ai_next) {
      s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (s == INVALID_SOCKET) continue;
      if (connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
      closesocket(s); s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) throw std::runtime_error("connect failed: " + WsaError());
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    return TcpSocket(s);
  }

  long SendSome(const std::uint8_t* data, long len) {
    if (len <= 0) return 0;
    int r = ::send(s_, reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
    if (r == SOCKET_ERROR) { int e = WSAGetLastError(); if (e == WSAEWOULDBLOCK) return 0; return -1; }
    return r;
  }
  long RecvSome(std::uint8_t* buf, long cap) {
    int r = ::recv(s_, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0);
    if (r == SOCKET_ERROR) { int e = WSAGetLastError(); if (e == WSAEWOULDBLOCK) return -2; return -1; }
    return r;
  }

 private:
  SOCKET s_ = INVALID_SOCKET;
};

class ServerSocket {
 public:
  ServerSocket() = default;
  ServerSocket(const ServerSocket&) = delete;
  ServerSocket& operator=(const ServerSocket&) = delete;
  ~ServerSocket() { Close(); }
  bool Listen(int port) {
    WsaInit();
    s_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s_ == INVALID_SOCKET) return false;
    int one = 1; setsockopt(s_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    struct sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) return false;
    if (::listen(s_, 16) == SOCKET_ERROR) return false;
    u_long nb = 1; ioctlsocket(s_, FIONBIO, &nb);
    return true;
  }
  TcpSocket Accept() {
    struct sockaddr_in c{}; int len = sizeof(c);
    SOCKET cs = ::accept(s_, reinterpret_cast<struct sockaddr*>(&c), &len);
    if (cs == INVALID_SOCKET) return TcpSocket();
    u_long nb = 1; ioctlsocket(cs, FIONBIO, &nb);
    return TcpSocket(cs);
  }
  void Close() { if (s_ != INVALID_SOCKET) { closesocket(s_); s_ = INVALID_SOCKET; } }
 private:
  SOCKET s_ = INVALID_SOCKET;
};

}  // namespace net
}  // namespace dfabric
