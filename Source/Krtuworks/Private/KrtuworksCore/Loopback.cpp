#include "Loopback.h"

#include <chrono>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace krtu::detail
{
namespace
{
#if defined(_WIN32)
using LoopSocket = SOCKET;
const std::intptr_t loopNoSocket = static_cast<std::intptr_t>(INVALID_SOCKET);

void loopStartup()
{
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

void loopClose(std::intptr_t s)
{
    closesocket(static_cast<SOCKET>(s));
}

int loopPoll(std::intptr_t s, int timeoutMs)
{
    WSAPOLLFD entry{};
    entry.fd = static_cast<SOCKET>(s);
    entry.events = POLLRDNORM;
    return WSAPoll(&entry, 1, timeoutMs);
}

int loopSend(std::intptr_t s, const char* data, std::size_t size)
{
    return send(static_cast<SOCKET>(s), data, static_cast<int>(size), 0);
}

int loopReceive(std::intptr_t s, char* data, std::size_t size)
{
    return recv(static_cast<SOCKET>(s), data, static_cast<int>(size), 0);
}
#else
using LoopSocket = int;
const std::intptr_t loopNoSocket = -1;

void loopStartup()
{
}

void loopClose(std::intptr_t s)
{
    close(static_cast<int>(s));
}

int loopPoll(std::intptr_t s, int timeoutMs)
{
    pollfd entry{};
    entry.fd = static_cast<int>(s);
    entry.events = POLLIN;
    return poll(&entry, 1, timeoutMs);
}

int loopSend(std::intptr_t s, const char* data, std::size_t size)
{
#if defined(MSG_NOSIGNAL)
    // A browser that closed first must not kill the game with SIGPIPE.
    return static_cast<int>(send(static_cast<int>(s), data, size, MSG_NOSIGNAL));
#else
    return static_cast<int>(send(static_cast<int>(s), data, size, 0));
#endif
}

int loopReceive(std::intptr_t s, char* data, std::size_t size)
{
    return static_cast<int>(recv(static_cast<int>(s), data, size, 0));
}
#endif

/// Reads the request head (up to 8 KB, 5 s): only its first line matters.
std::string loopReadHead(std::intptr_t client)
{
    std::string head;
    char buffer[2048];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (head.size() < 8192 && head.find("\r\n") == std::string::npos && std::chrono::steady_clock::now() < deadline)
    {
        if (loopPoll(client, 200) <= 0) continue;
        const int got = loopReceive(client, buffer, sizeof buffer);
        if (got <= 0) break;
        head.append(buffer, static_cast<std::size_t>(got));
    }
    return head;
}

void loopSendAll(std::intptr_t client, const std::string& bytes)
{
    std::size_t sent = 0;
    while (sent < bytes.size())
    {
        const int n = loopSend(client, bytes.data() + sent, bytes.size() - sent);
        if (n <= 0) return;
        sent += static_cast<std::size_t>(n);
    }
}
}  // namespace

LoopbackReceiver::LoopbackReceiver() : socket_(loopNoSocket)
{
    loopStartup();
    const auto s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    socket_ = static_cast<std::intptr_t>(s);
    if (socket_ == loopNoSocket) throw Error("loopback_unavailable", "No local socket for the sign-in's return.");
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(static_cast<LoopSocket>(socket_), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(static_cast<LoopSocket>(socket_), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 || listen(static_cast<LoopSocket>(socket_), 8) != 0)
    {
        loopClose(socket_);
        throw Error("loopback_unavailable", "The sign-in's return cannot listen on 127.0.0.1.");
    }
    sockaddr_in bound{};
    socklen_t length = sizeof bound;
    getsockname(static_cast<LoopSocket>(socket_), reinterpret_cast<sockaddr*>(&bound), &length);
    port_ = ntohs(bound.sin_port);
}

LoopbackReceiver::~LoopbackReceiver()
{
    if (socket_ != loopNoSocket) loopClose(socket_);
}

std::string LoopbackReceiver::wait(const CancelToken& cancel)
{
    static const std::string page =
        "<!doctype html><meta charset=utf-8><title>Krtuworks ID</title><body style=\"font-family:sans-serif;padding:3em\">You are signed in. You can close this window and go back to the game.</body>";
    while (true)
    {
        cancel.throwIfCancelled();
        if (loopPoll(socket_, 100) <= 0) continue;
        const auto accepted = accept(static_cast<LoopSocket>(socket_), nullptr, nullptr);
        const auto client = static_cast<std::intptr_t>(accepted);
        if (client == loopNoSocket) continue;
        const auto head = loopReadHead(client);
        const auto lineEnd = head.find("\r\n");
        const auto line = head.substr(0, lineEnd == std::string::npos ? head.size() : lineEnd);
        const auto firstSpace = line.find(' ');
        const auto secondSpace = firstSpace == std::string::npos ? std::string::npos : line.find(' ', firstSpace + 1);
        const auto path = firstSpace == std::string::npos ? std::string("/") : line.substr(firstSpace + 1, secondSpace == std::string::npos ? std::string::npos : secondSpace - firstSpace - 1);
        const bool ours = path.compare(0, 9, "/callback") == 0;
        const auto body = ours ? page : std::string();
        loopSendAll(client, std::string(ours ? "HTTP/1.1 200 OK" : "HTTP/1.1 404 Not Found") + "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) +
                                "\r\nConnection: close\r\n\r\n" + body);
        loopClose(client);
        if (ours) return "http://127.0.0.1:" + std::to_string(port_) + path;
    }
}
}  // namespace krtu::detail
