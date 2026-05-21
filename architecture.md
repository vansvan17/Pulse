# Architecture notes

## Connection state machine

```
                  ┌─────────────────────┐
   accept() ────▶ │  kReadingRequest    │ ◀───── keep-alive ──┐
                  └──────────┬──────────┘                     │
                             │ complete request parsed        │
                             ▼                                │
                  ┌─────────────────────┐                     │
                  │     kProcessing     │                     │
                  └──────────┬──────────┘                     │
                             │ response queued                │
                             ▼                                │
                  ┌─────────────────────┐                     │
                  │      kWriting       │ ─────────────────── ┘
                  └──────────┬──────────┘
                             │ Connection: close, or error
                             ▼
                  ┌─────────────────────┐
                  │      kClosing       │
                  └──────────┬──────────┘
                             │
                       close(fd), erase from conns_
```

Transitions to `kClosing` can happen from any state on:

- `read()` error (anything besides `EAGAIN`, `EINTR`)
- `send()` / `sendfile()` error
- malformed request (after sending the 4xx/5xx response)
- explicit `Connection: close` header
- HTTP/1.0 without keep-alive
- idle timeout fired by the timer queue
- `EPOLLHUP` or `EPOLLERR`

## Event sequence for one keep-alive request

```
client                  kernel                  pulse (event loop)
──────                  ──────                  ──────────────────
SYN ───────────────────▶
                                                epoll_wait returns:
                                                  listener fd readable
                                                accept4() loop:
                                                  cfd = accept4(...)
                                                  set TCP_NODELAY
                                                  epoll_ctl ADD cfd
                                                                EPOLLIN | EPOLLET | EPOLLRDHUP
                                                  timers_.schedule(...)
GET / HTTP/1.1 ────────▶
Host: x
\r\n\r\n
                                                epoll_wait returns:
                                                  cfd readable (EPOLLIN edge)
                                                on_readable(c):
                                                  read() → 38 bytes, commit
                                                  read() → EAGAIN ✓ drained
                                                  touch_timer()
                                                  parse → Ok, consumed=38
                                                  serve_static():
                                                    realpath check
                                                    open() index.html
                                                    write_headers → write_buf
                                                  state = kWriting
                                                  on_writable():
                                                    send() headers, drained
                                                    sendfile() body, drained
                                                    after_response_sent():
                                                      keep_alive=true
                                                      state = kReadingRequest
                                                      touch_timer()
                  ◀──── HTTP/1.1 200 OK
                       ...
                       <body>
... next request ─────▶
                                                same cycle, same connection
```

Note: when `send()` or `sendfile()` returns `EAGAIN` mid-response, we arm
`EPOLLOUT` and return. A subsequent `EPOLLOUT` event resumes the flush from
the current offset (for `write_buf` via its cursor; for the file via
`file_resp.offset`).

## File descriptor accounting

Three classes of fd:

- **listening socket** — one per loop, lives forever
- **epoll fd** — one per loop, lives forever
- **client sockets** — one per connection, owned by entries in `conns_`

Client sockets are owned by raw int in `Connection::sock_fd` (the map is
keyed by fd, so a `UniqueFd` wrapper would have to know the map and
unregister itself — clean ownership story but more code). `close_connection`
is the single point of cleanup: it removes from epoll, erases from `conns_`
(which destroys the Connection), then closes.

The file fd from `sendfile()` lives in `Connection::file_resp.fd` as a
`UniqueFd`. It is released the moment the response is fully sent — we do
**not** hold it across keep-alive idles.

## Memory model

Per-connection allocations:

- `Connection` struct itself, on the heap via `std::unique_ptr`
- `read_buf` — starts at 4 KB capacity, grows on demand to fit request
  headers + body. Compacted when read cursor crosses 2 KB.
- `write_buf` — starts at 4 KB capacity, grows on demand to fit response
  headers. For static files, body is `sendfile`'d so this stays small.

No per-request heap allocations on the parser hot path: headers are
`string_view`s into the read buffer.

## Concurrency

There is none, per loop. Run more loops. The whole point of `SO_REUSEPORT`
is to make `mutex` a thing that doesn't exist in the data path.

Cross-loop synchronization is also nonexistent — workers never communicate.
If we ever need shared state (rate limits, cache invalidation), it goes
through the kernel (shared memory, eventfd, signal) or a separate
coordination service. We don't have those needs today.
