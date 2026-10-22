#pragma once
// Модуль tcp — порт tcp.py: голые TCP-сокеты.
//   intptr_t TcpListen(port)          — слушающий сокет (0 = fail)
//   intptr_t TcpAccept(lsock)         — ждать клиента
//   intptr_t TcpConnect(host, port)   — подключиться (0 = fail)
//   TcpSend(sock, str)            — отправить байты
//   string TcpRecvLine(sock)      — до \n (без \r), "" = закрыто
//   string TcpRecvAll(sock)       — до закрытия
//   TcpClose(sock)
//   string TcpPeerIp(sock)        — ip клиента
//   TcpSetTimeout(sock, ms) / TcpSleepMs(ms)
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define closesocket close
#define SOCKET_ERROR (-1)
#endif
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdint>

inline bool _tcpInit() {
    static bool done = false;
    if (done) return true;
    done = true;
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

inline intptr_t TcpListen(int port) {
    _tcpInit();
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 0;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&one, sizeof(one));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons((u_short)port);
    if (::bind(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR
            || listen(s, 16) == SOCKET_ERROR) {
        closesocket(s);
        return 0;
    }
    return (intptr_t)s;
}

inline intptr_t TcpAccept(intptr_t lsock) {
    SOCKET c = accept((SOCKET)lsock, nullptr, nullptr);
    if (c == INVALID_SOCKET) return 0;
    return (intptr_t)c;
}

inline intptr_t TcpConnect(const std::string &host, int port) {
    _tcpInit();
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    std::string ps = std::to_string(port);
    if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0
            || !res) {
        return 0;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype,
                      res->ai_protocol);
    if (s == INVALID_SOCKET
            || connect(s, res->ai_addr, (int)res->ai_addrlen)
                   == SOCKET_ERROR) {
        closesocket(s);
        freeaddrinfo(res);
        return 0;
    }
    freeaddrinfo(res);
    return (intptr_t)s;
}

inline void TcpSend(intptr_t sock, const std::string &data) {
    SOCKET s = (SOCKET)sock;
    int sent = 0;
    while (sent < (int)data.size()) {
        int n = send(s, data.c_str() + sent,
                     (int)data.size() - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

inline std::string TcpRecvLine(intptr_t sock) {
    SOCKET s = (SOCKET)sock;
    std::string out;
    char c;
    while (true) {
        int n = recv(s, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\n') break;
        if (c != '\r') out += c;
    }
    return out;
}

inline std::string TcpRecvAll(intptr_t sock) {
    SOCKET s = (SOCKET)sock;
    std::string out;
    char buf[8192];
    int n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        out.append(buf, n);
    }
    return out;
}

inline void TcpSetTimeout(intptr_t sock, int ms) {
    SOCKET s = (SOCKET)sock;
#ifdef _WIN32
    DWORD t = (DWORD)ms;
#else
    struct timeval t = {(time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
}

inline void TcpClose(intptr_t sock) {
    closesocket((SOCKET)sock);
}

inline void TcpSleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline std::string TcpPeerIp(intptr_t sock) {
    sockaddr_in a = {};
    socklen_t al = sizeof(a);
    if (getpeername((SOCKET)sock, (sockaddr*)&a, &al) != 0)
        return "";
#ifdef _WIN32
    // у TDM-MinGW inet_ntop может не объявляться
    char* p = inet_ntoa(a.sin_addr);
    return p ? p : "";
#else
    char buf[64];
    return inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf))
           ? buf : "";
#endif
}
