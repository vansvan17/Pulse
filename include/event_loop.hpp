// pulse/event_loop.hpp — Single-threaded epoll reactor.
//
// Threading model:
//   This event loop is single-threaded. Run N of them, one per core, each
//   with its own listening socket bound to the same port via SO_REUSEPORT.
//   The kernel does the load balancing. No locks, no false sharing, no
//   worker-pool queueing overhead. This is the modern high-performance
//   pattern.
//
// Why edge-triggered:
//   With level-triggered epoll, the kernel reports readiness every iteration
//   while the condition holds. That's simple but costs O(n_ready) wakeups
//   for n_ready connections that haven't been fully drained.
//
//   Edge-triggered reports readiness only on the transition. To not deadlock
//   we MUST drain every fd to EAGAIN on each notification. The reward is
//   fewer epoll_wait wakeups and less syscall overhead.
//
//   The cost: a single bug where we forget to drain → connection wedges
//   forever. That's why every read/write loop in this file goes until EAGAIN.
//
// EPOLLRDHUP:
//   Detects "peer closed the write side" without us having to issue a read.
//   When we see it, we can short-circuit cleanup instead of waiting for the
//   next read to return 0. Small but important.

#pragma once

#include <sys/epoll.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "connection.hpp"
#include "fd.hpp"
#include "http_request.hpp"
#include "timer_queue.hpp"

namespace pulse {

struct EventLoopConfig {
    std::string doc_root;
    uint16_t port = 8080;
    int backlog = 1024;
    std::chrono::milliseconds idle_timeout{30'000};   // 30s, like nginx default
    int max_events = 256;                              // per epoll_wait batch
    bool enable_reuseport = true;
};

class EventLoop {
public:
    explicit EventLoop(EventLoopConfig cfg);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Run until stop() is called from a signal handler.
    void run();
    void stop() { stopping_ = true; }

private:
    // === Lifecycle ===
    void accept_new_connections();
    void close_connection(Connection& c, const char* reason);

    // === I/O drivers ===
    // Returns false if the connection was closed during this call.
    bool on_readable(Connection& c);
    bool on_writable(Connection& c);

    // === HTTP state transitions ===
    bool try_parse_and_dispatch(Connection& c);
    void serve_static(Connection& c, http::Request& req);
    void send_error(Connection& c, int status, bool keep_alive, int http_minor);
    void after_response_sent(Connection& c);   // keep-alive or close

    // === epoll plumbing ===
    void epoll_add(int fd, uint32_t events, Connection* tag);
    void epoll_mod(int fd, uint32_t events, Connection* tag);
    void touch_timer(Connection& c);

    // === Members ===
    EventLoopConfig cfg_;
    UniqueFd listen_fd_;
    UniqueFd epoll_fd_;

    // Owning store: fd → connection. Pointers handed to epoll's data.ptr.
    std::unordered_map<int, std::unique_ptr<Connection>> conns_;

    TimerQueue timers_;
    uint64_t next_conn_id_ = 1;
    bool stopping_ = false;

    // Stats — exposed later through a /metrics handler.
    uint64_t accepts_total_  = 0;
    uint64_t requests_total_ = 0;
    uint64_t bytes_out_total_ = 0;
};

} // namespace pulse
