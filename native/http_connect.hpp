#pragma once
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>

std::pair<std::string, std::string> proxy_address(std::string address) {
    if (address.rfind("http://", 0) == 0) address.erase(0, 7);
    if (address.find_first_of("/@\r\n\t %'") != std::string::npos)
        throw std::runtime_error("SSH_HTTP_PROXY requires host:port, without authentication or paths");
    std::string host, port;
    if (!address.empty() && address[0] == '[') {
        auto end = address.find(']');
        if (end == std::string::npos || address.substr(end + 1, 1) != ":")
            throw std::runtime_error("invalid bracketed proxy address");
        host = address.substr(1, end - 1); port = address.substr(end + 2);
    } else {
        auto end = address.rfind(':');
        if (end == std::string::npos) throw std::runtime_error("proxy port is required");
        host = address.substr(0, end); port = address.substr(end + 1);
        if (host.find(':') != std::string::npos) throw std::runtime_error("bracket IPv6 proxy addresses");
    }
    if (host.empty() || port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos ||
        std::stoi(port) < 1 || std::stoi(port) > 65535) throw std::runtime_error("invalid proxy host/port");
    return {host, port};
}
// Bootstrap only: transfer the connected socket to OpenSSH with SCM_RIGHTS.
// The helper exits; OpenSSH subsequently owns all encrypted terminal traffic.
int http_connect_fd(const std::string& proxy, std::string host, const std::string& port) {
    auto address = proxy_address(proxy);
    if (host.empty() || host.find_first_of("\r\n /@") != std::string::npos || port.empty() || port.size() > 5 ||
        port.find_first_not_of("0123456789") != std::string::npos || std::stoi(port) < 1 || std::stoi(port) > 65535)
        throw std::runtime_error("invalid HTTP CONNECT destination");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto ready = [&](int fd, short events) {
        for (;;) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) throw std::runtime_error("HTTP proxy timeout");
            pollfd descriptor{fd, events, 0};
            int result = poll(&descriptor, 1, static_cast<int>(remaining));
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) throw std::runtime_error("HTTP proxy timeout");
            return;
        }
    };
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    int error = getaddrinfo(address.first.c_str(), address.second.c_str(), &hints, &addresses);
    if (error) throw std::runtime_error("cannot resolve HTTP proxy");
    int fd = -1;
    try {
        for (auto entry = addresses; entry; entry = entry->ai_next) {
            fd = socket(entry->ai_family, entry->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, entry->ai_protocol);
            if (fd < 0) continue;
            if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) break;
            if (errno == EINPROGRESS) {
                ready(fd, POLLOUT);
                int status = 0; socklen_t size = sizeof status;
                if (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &status, &size) && !status) break;
            }
            close(fd); fd = -1;
        }
        freeaddrinfo(addresses); addresses = nullptr;
        if (fd < 0) throw std::runtime_error("cannot connect to HTTP proxy");
        if (host.find(':') != std::string::npos && host[0] != '[') host = "[" + host + "]";
        std::string authority = host + ":" + port;
        std::string request = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n\r\n";
        size_t offset = 0;
        while (offset < request.size()) {
            ready(fd, POLLOUT);
            auto n = send(fd, request.data() + offset, request.size() - offset, MSG_NOSIGNAL);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) throw std::runtime_error("HTTP proxy write failed");
            offset += static_cast<size_t>(n);
        }
        // Do not read past the headers and accidentally consume SSH banner bytes.
        std::string response;
        while (response.size() < 16384 && (response.size() < 4 || response.compare(response.size() - 4, 4, "\r\n\r\n"))) {
            ready(fd, POLLIN); char c;
            auto n = recv(fd, &c, 1, 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n != 1) throw std::runtime_error("HTTP proxy closed connection");
            response += c;
        }
        if ((response.rfind("HTTP/1.0 200 ", 0) != 0 && response.rfind("HTTP/1.1 200 ", 0) != 0) ||
            response.size() < 4 || response.compare(response.size() - 4, 4, "\r\n\r\n"))
            throw std::runtime_error("HTTP CONNECT rejected or invalid response");
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK)) throw std::runtime_error("cannot prepare SSH socket");
        char byte = 0;
        iovec io{&byte, 1};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
        msghdr message{}; message.msg_iov = &io; message.msg_iovlen = 1;
        message.msg_control = control; message.msg_controllen = sizeof control;
        auto header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS; header->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header), &fd, sizeof fd);
        ssize_t sent;
        do { sent = sendmsg(STDOUT_FILENO, &message, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
        if (sent != 1) throw std::runtime_error("OpenSSH fd passing failed; requires ProxyUseFdpass=yes");
        close(fd); return 0;
    } catch (...) {
        if (addresses) freeaddrinfo(addresses);
        if (fd >= 0) close(fd);
        throw;
    }
}
