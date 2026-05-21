// pulse/socket_utils.hpp — Thin wrappers over Linux socket syscalls.
//
// Why a header-only utility module:
//   The interesting bits aren't the wrapping — they're the FLAGS and which
//   syscalls we chose. Anyone reviewing this file should immediately see:
//
//     - accept4() not accept(): atomic SOCK_NONBLOCK | SOCK_CLOEXEC. The
//       2-syscall version (accept + fcntl) has a window where the fd leaks
//       to a forked child or is briefly blocking.
//
//     - SO_REUSEPORT not just SO_REUSEADDR: lets N event loops each call
//       accept() on the same port. Kernel hash-distributes connections.
//       This is how we scale across cores without a thundering-herd or a
//       single-accept-thread bottleneck.
//
//     - TCP_NODELAY: HTTP responses are small and we want them on the wire
//       NOW, not when Nagle decides. Mandatory for low-latency HTTP.
//
//     - SIGPIPE ignored at startup: writing to a closed peer otherwise kills
//       the process. The alternative is MSG_NOSIGNAL on every send(), which
//       we use too, but ignoring it once is simpler and harmless.

#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include "fd.hpp"

namespace pulse::sock {

inline void ignore_sigpipe() {
    // Idempotent. Safe to call multiple times.
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGPIPE, &sa, nullptr);
}

inline void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl(F_GETFL)");
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl(F_SETFL)");
    }
}

inline void set_int_sockopt(int fd, int level, int opt, int value, const char* name) {
    if (::setsockopt(fd, level, opt, &value, sizeof(value)) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                std::string("setsockopt(") + name + ")");
    }
}

// Create a listening TCP socket bound to `port` on all interfaces.
// reuse_port=true is required when running multiple event loops on the same port.
inline UniqueFd make_listener(uint16_t port, int backlog = 1024, bool reuse_port = true) {
    // SOCK_NONBLOCK | SOCK_CLOEXEC at socket creation: same reasoning as accept4.
    int s = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) throw std::system_error(errno, std::generic_category(), "socket");
    UniqueFd fd(s);

    // SO_REUSEADDR: skip the TIME_WAIT delay on restart. Safe for servers.
    set_int_sockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, 1, "SO_REUSEADDR");

    // SO_REUSEPORT: kernel-level load balancing across multiple listeners.
    // Each worker process/thread calls bind() on the same port and the kernel
    // distributes incoming connections. This is how nginx and envoy scale.
    if (reuse_port) {
        set_int_sockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, 1, "SO_REUSEPORT");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::system_error(errno, std::generic_category(), "bind");
    }
    if (::listen(fd.get(), backlog) < 0) {
        throw std::system_error(errno, std::generic_category(), "listen");
    }
    return fd;
}

// Per-connection tuning. Called once when we accept a client.
inline void tune_client_socket(int fd) {
    // Disable Nagle: HTTP responses are typically a small header batch and we
    // already buffer in userspace before send(). Nagle would add up to 200ms
    // of latency for no benefit.
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // Note: we deliberately do NOT set SO_KEEPALIVE. HTTP keep-alive is
    // application-layer; TCP keepalive defaults (2h) are useless to us.
    // Our timer wheel handles idle timeouts at the application layer.
}

} // namespace pulse::sock
