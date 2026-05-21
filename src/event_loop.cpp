// pulse/event_loop.cpp — Reactor implementation.
//
// Reading guide:
//   - run()                         : top-level dispatch
//   - accept_new_connections()      : drain accept() to EAGAIN
//   - on_readable() / on_writable() : drain read()/sendfile() to EAGAIN
//   - try_parse_and_dispatch()      : pipeline handling (multiple requests
//                                     per readable event for keep-alive)
//
// Every loop boundary in this file is annotated with WHY it terminates on
// EAGAIN. This is the kind of code where the comments matter more than the
// statements.

#include "pulse/event_loop.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "pulse/http_request.hpp"
#include "pulse/http_response.hpp"
#include "pulse/log.hpp"
#include "pulse/socket_utils.hpp"
#include "pulse/static_files.hpp"

namespace pulse {

EventLoop::EventLoop(EventLoopConfig cfg) : cfg_(std::move(cfg)) {
    sock::ignore_sigpipe();

    listen_fd_ = sock::make_listener(cfg_.port, cfg_.backlog, cfg_.enable_reuseport);

    int ep = ::epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) throw std::system_error(errno, std::generic_category(), "epoll_create1");
    epoll_fd_ = UniqueFd(ep);

    // Listening socket gets level-triggered for simplicity. ET on accept is
    // valid too, but level-triggered + accept-to-EAGAIN is just as cheap and
    // less error-prone.
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = nullptr;   // nullptr tag = listening socket
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, listen_fd_.get(), &ev) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl(ADD listen)");
    }

    log::info("server_start")
        .kv("port", static_cast<int>(cfg_.port))
        .kv("doc_root", cfg_.doc_root)
        .kv("reuseport", cfg_.enable_reuseport ? 1 : 0);
}

EventLoop::~EventLoop() = default;

void EventLoop::run() {
    std::vector<epoll_event> events(static_cast<size_t>(cfg_.max_events));

    while (!stopping_) {
        auto now = now_steady();
        int timeout_ms = timers_.next_timeout_ms(now, 1000);

        int n = ::epoll_wait(epoll_fd_.get(), events.data(),
                             static_cast<int>(events.size()), timeout_ms);
        if (n < 0) {
            if (errno == EINTR) continue;   // signal during wait; safe to retry
            log::error("epoll_wait_failed").kv("errno", errno);
            break;
        }

        // Dispatch ready fds first; do timer eviction after. Order matters:
        // an active connection should not be reaped if its activity is in
        // this batch.
        for (int i = 0; i < n; ++i) {
            auto& e = events[static_cast<size_t>(i)];
            if (e.data.ptr == nullptr) {
                // Listening socket.
                accept_new_connections();
                continue;
            }

            auto* c = static_cast<Connection*>(e.data.ptr);

            // EPOLLRDHUP / EPOLLHUP / EPOLLERR — peer closed or error.
            // We still try to read so we drain any final bytes, but if we
            // can't progress we'll close immediately.
            if (e.events & (EPOLLERR | EPOLLHUP)) {
                close_connection(*c, "peer_hangup_or_error");
                continue;
            }

            bool alive = true;
            if (e.events & (EPOLLIN | EPOLLRDHUP)) {
                alive = on_readable(*c);
            }
            if (alive && (e.events & EPOLLOUT)) {
                alive = on_writable(*c);
            }
            // If still alive, c was mutated in-place. Map entry still valid.
            (void)alive;
        }

        // Timer eviction: pop entries whose deadline ≤ now. Validate the
        // generation against the connection's current generation; stale
        // entries (from a since-touched connection) are dropped silently.
        now = now_steady();   // refresh after dispatch work
        timers_.pop_expired(now, [&](uint64_t conn_id, uint64_t gen) {
            // The id-to-Connection map isn't keyed by conn_id, it's keyed
            // by fd. Scan: in a real production server we'd keep a parallel
            // id→fd index. The scan is fine at our connection counts; if
            // profiling says otherwise we add the index. KISS for now.
            for (auto& [fd, cptr] : conns_) {
                if (cptr->id == conn_id) {
                    if (cptr->gen != gen) return;   // stale; connection was touched
                    log::debug("idle_timeout").kv("conn_id", static_cast<long long>(conn_id));
                    close_connection(*cptr, "idle_timeout");
                    return;
                }
            }
        });
    }

    log::info("server_stop").kv("accepts", static_cast<long long>(accepts_total_))
                            .kv("requests", static_cast<long long>(requests_total_))
                            .kv("bytes_out", static_cast<long long>(bytes_out_total_));
}

// ──────────────────────────────────────────────────────────────────────
//   accept()
// ──────────────────────────────────────────────────────────────────────
void EventLoop::accept_new_connections() {
    // Drain to EAGAIN. The listen socket is level-triggered but draining
    // amortizes the cost of one epoll_wait notification across many accepts.
    for (;;) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);

        // accept4 with SOCK_NONBLOCK|SOCK_CLOEXEC: atomic, no fcntl race.
        int cfd = ::accept4(listen_fd_.get(), reinterpret_cast<sockaddr*>(&peer),
                            &plen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;   // expected end-of-burst
            if (errno == EINTR) continue;                          // try again
            // Per accept4(2): on Linux, accept errors on the new socket can
            // surface here (ECONNABORTED, EMFILE, ENFILE, etc.). For
            // ECONNABORTED we silently retry — the client gave up before
            // we accepted. For EMFILE/ENFILE we're out of fds; log and back
            // off by returning (the listener will fire again next loop tick).
            if (errno == ECONNABORTED) continue;
            log::warn("accept_failed").kv("errno", errno);
            return;
        }

        sock::tune_client_socket(cfd);

        auto c = std::make_unique<Connection>();
        c->sock_fd = cfd;
        c->id = next_conn_id_++;
        c->gen = 1;

        // EPOLLET: edge-triggered. We commit to drain-to-EAGAIN on every
        // read. EPOLLRDHUP: notify on peer half-close without a read.
        // We start with EPOLLIN only; EPOLLOUT is added when we have
        // pending writes that drained partially.
        Connection* tag = c.get();
        try {
            epoll_add(cfd, EPOLLIN | EPOLLET | EPOLLRDHUP, tag);
        } catch (const std::exception& e) {
            log::error("epoll_add_failed").kv("err", e.what());
            ::close(cfd);
            continue;
        }

        // Schedule the initial idle timer.
        c->last_active = now_steady();
        timers_.schedule(c->id, c->gen, c->last_active + cfg_.idle_timeout);

        conns_.emplace(cfd, std::move(c));
        ++accepts_total_;

        if (log::g_min_level == log::Level::kDebug) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            log::debug("accepted").kv("peer", ip).kv("port", ntohs(peer.sin_port));
        }
    }
}

// ──────────────────────────────────────────────────────────────────────
//   read() drain
// ──────────────────────────────────────────────────────────────────────
bool EventLoop::on_readable(Connection& c) {
    // ET semantics: keep reading until EAGAIN. If we leave bytes in the
    // kernel buffer without seeing EAGAIN, we may never get another EPOLLIN
    // for this fd. This is THE classic ET bug.
    for (;;) {
        c.read_buf.ensure_writable(4096);
        ssize_t n = ::read(c.sock_fd, c.read_buf.write_ptr(), c.read_buf.writable());

        if (n > 0) {
            c.read_buf.commit(static_cast<size_t>(n));
            continue;   // keep draining
        }
        if (n == 0) {
            // Orderly close from peer.
            close_connection(c, "peer_close");
            return false;
        }
        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // drained
        if (errno == EINTR) continue;
        log::warn("read_failed").kv("errno", errno).kv("conn_id", static_cast<long long>(c.id));
        close_connection(c, "read_error");
        return false;
    }

    touch_timer(c);

    // Now parse and dispatch every complete request in the buffer (pipelining).
    return try_parse_and_dispatch(c);
}

// ──────────────────────────────────────────────────────────────────────
//   parse + dispatch (handles pipelined keep-alive requests)
// ──────────────────────────────────────────────────────────────────────
bool EventLoop::try_parse_and_dispatch(Connection& c) {
    while (c.state == ConnState::kReadingRequest && c.read_buf.readable() > 0) {
        http::Request req;
        auto pr = http::parse_request(c.read_buf.readable_view(), req);

        if (pr.status == http::ParseStatus::kIncomplete) {
            return true;   // wait for more bytes
        }
        if (pr.status == http::ParseStatus::kBadRequest) {
            send_error(c, 400, /*keep_alive=*/false, /*http_minor=*/1);
            // After error: try to flush, then close. We mark as kWriting and
            // let on_writable() do the rest.
            c.keep_alive_after_response = false;
            c.state = ConnState::kWriting;
            return on_writable(c);
        }
        if (pr.status == http::ParseStatus::kTooLarge) {
            send_error(c, 431, /*keep_alive=*/false, /*http_minor=*/1);
            c.keep_alive_after_response = false;
            c.state = ConnState::kWriting;
            return on_writable(c);
        }

        // OK — we have a complete request. Consume the header bytes.
        c.read_buf.consume(pr.consumed);
        ++requests_total_;
        ++c.requests_served;

        // Body handling: this server doesn't accept bodies (GET/HEAD only).
        // If a Content-Length is present, we must drain it from the buffer
        // before processing the next pipelined request. For correctness with
        // POSTs we'd dispatch the body to a handler here.
        if (req.content_length > 0) {
            if (c.read_buf.readable() < req.content_length) {
                // Body not yet fully buffered. Stash the request and wait.
                // For this toy server we treat any body request as 405.
                send_error(c, 405, /*keep_alive=*/false, req.http_minor);
                c.keep_alive_after_response = false;
                c.state = ConnState::kWriting;
                return on_writable(c);
            }
            c.read_buf.consume(req.content_length);
        }

        // Method check — we only serve GET and HEAD.
        bool is_head = (req.method == "HEAD");
        if (req.method != "GET" && !is_head) {
            send_error(c, 405, req.keep_alive, req.http_minor);
            c.keep_alive_after_response = req.keep_alive;
            c.state = ConnState::kWriting;
            if (!on_writable(c)) return false;
            continue;   // try next pipelined request
        }

        serve_static(c, req);

        // Try to flush immediately. If the socket buffer accepts everything,
        // on_writable will transition us back to kReadingRequest in the same
        // tick — important for benchmarks where the same loop iteration
        // wants to handle the next pipelined request.
        c.state = ConnState::kWriting;
        c.keep_alive_after_response = req.keep_alive;
        if (!on_writable(c)) return false;

        // If we're still kReadingRequest after on_writable, the response
        // fully flushed and keep-alive is active — loop to next pipelined.
        // If kWriting, we need more EPOLLOUT events; bail.
        if (c.state == ConnState::kWriting) return true;
        if (c.state == ConnState::kClosing) return false;
    }
    return true;
}

// ──────────────────────────────────────────────────────────────────────
//   handler: serve a static file
// ──────────────────────────────────────────────────────────────────────
void EventLoop::serve_static(Connection& c, http::Request& req) {
    auto rf = http::resolve_and_open(cfg_.doc_root, req.path);
    if (!rf.fd.valid()) {
        int status = rf.errno_status ? rf.errno_status : 404;
        send_error(c, status, req.keep_alive, req.http_minor);
        return;
    }

    bool is_head = (req.method == "HEAD");
    size_t body_len = static_cast<size_t>(rf.size);

    http::write_headers(c.write_buf, 200, rf.mime, body_len,
                        req.keep_alive, req.http_minor);

    if (!is_head) {
        c.file_resp.fd = std::move(rf.fd);
        c.file_resp.offset = 0;
        c.file_resp.remaining = body_len;
    }
}

// ──────────────────────────────────────────────────────────────────────
//   write() / sendfile() drain
// ──────────────────────────────────────────────────────────────────────
bool EventLoop::on_writable(Connection& c) {
    // Phase 1: flush headers (and any small inline body) from write_buf.
    while (c.write_buf.readable() > 0) {
        ssize_t n = ::send(c.sock_fd, c.write_buf.read_ptr(),
                           c.write_buf.readable(), MSG_NOSIGNAL);
        if (n > 0) {
            c.write_buf.consume(static_cast<size_t>(n));
            bytes_out_total_ += static_cast<uint64_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Socket buffer full. Arm EPOLLOUT and wait.
            epoll_mod(c.sock_fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP, &c);
            return true;
        }
        if (n < 0 && errno == EINTR) continue;
        // Real error (EPIPE, ECONNRESET, etc.)
        close_connection(c, "send_error");
        return false;
    }

    // Phase 2: stream the file body with sendfile(2). Zero-copy: bytes go
    // page-cache → socket buffer without crossing into userspace.
    while (c.file_resp.remaining > 0) {
        ssize_t n = ::sendfile(c.sock_fd, c.file_resp.fd.get(),
                               &c.file_resp.offset, c.file_resp.remaining);
        if (n > 0) {
            c.file_resp.remaining -= static_cast<size_t>(n);
            bytes_out_total_ += static_cast<uint64_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            epoll_mod(c.sock_fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP, &c);
            return true;
        }
        if (n < 0 && errno == EINTR) continue;
        close_connection(c, "sendfile_error");
        return false;
    }

    // All flushed. Drop EPOLLOUT to avoid spurious wakeups.
    epoll_mod(c.sock_fd, EPOLLIN | EPOLLET | EPOLLRDHUP, &c);

    after_response_sent(c);
    return c.state != ConnState::kClosing;
}

void EventLoop::after_response_sent(Connection& c) {
    // Release the file fd promptly (don't wait for connection close).
    c.file_resp.fd.reset();
    c.file_resp.offset = 0;
    c.file_resp.remaining = 0;

    if (!c.keep_alive_after_response) {
        close_connection(c, "response_sent_close");
        return;
    }

    // Keep-alive: reset state, refresh idle timer, try to handle any
    // pipelined request already in the read buffer.
    c.state = ConnState::kReadingRequest;
    touch_timer(c);

    if (c.read_buf.readable() > 0) {
        try_parse_and_dispatch(c);
    }
}

void EventLoop::send_error(Connection& c, int status, bool keep_alive, int http_minor) {
    char body[256];
    int n = std::snprintf(body, sizeof(body), "%d %s\n", status, http::status_text(status));
    http::write_simple(c.write_buf, status,
                       std::string_view(body, static_cast<size_t>(n)),
                       keep_alive, http_minor);
    log::info("error_response")
        .kv("status", status)
        .kv("conn_id", static_cast<long long>(c.id));
}

// ──────────────────────────────────────────────────────────────────────
//   teardown
// ──────────────────────────────────────────────────────────────────────
void EventLoop::close_connection(Connection& c, const char* reason) {
    if (c.state == ConnState::kClosing) return;
    c.state = ConnState::kClosing;

    // epoll_ctl(EPOLL_CTL_DEL) is implicit on close() but being explicit
    // helps if we're closing while the fd is reused elsewhere. Cheap.
    if (c.sock_fd >= 0) {
        ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, c.sock_fd, nullptr);
    }

    log::info("conn_close")
        .kv("conn_id", static_cast<long long>(c.id))
        .kv("reason", reason)
        .kv("requests", static_cast<long long>(c.requests_served));

    int fd = c.sock_fd;
    c.sock_fd = -1;
    // Remove from map AFTER reading any needed fields — this destroys `c`.
    conns_.erase(fd);
    ::close(fd);
}

// ──────────────────────────────────────────────────────────────────────
//   epoll plumbing
// ──────────────────────────────────────────────────────────────────────
void EventLoop::epoll_add(int fd, uint32_t events, Connection* tag) {
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = tag;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl(ADD)");
    }
}

void EventLoop::epoll_mod(int fd, uint32_t events, Connection* tag) {
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = tag;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &ev) < 0) {
        // MOD shouldn't fail for an extant fd; if it does, log and continue.
        log::warn("epoll_mod_failed").kv("errno", errno).kv("fd", fd);
    }
}

void EventLoop::touch_timer(Connection& c) {
    c.last_active = now_steady();
    ++c.gen;
    timers_.schedule(c.id, c.gen, c.last_active + cfg_.idle_timeout);
}

} // namespace pulse
