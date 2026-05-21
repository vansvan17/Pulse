// pulse/connection.hpp — Per-client connection state machine.
//
// State machine (drawn ASCII because mentors will ask):
//
//   ┌─────────────┐  data in   ┌─────────────┐  parsed   ┌─────────────┐
//   │ kReadingReq │──────────▶│ kProcessing │──────────▶│  kWriting   │
//   └─────────────┘            └─────────────┘           └──────┬──────┘
//          ▲                                                    │
//          │                        keep-alive                  │
//          └────────────────────────────────────────────────────┘
//                                                               │ close
//                                                               ▼
//                                                        ┌─────────────┐
//                                                        │  kClosing   │
//                                                        └─────────────┘
//
//   At any point: read error / write error / timeout → kClosing.
//
// Generation counter:
//   Every time we re-arm the idle timer (because activity occurred), we bump
//   `gen_`. Pending entries in the TimerQueue with stale generations are
//   dropped silently when they fire. See timer_queue.hpp for rationale.
//
// File response:
//   When the response body is a file we keep a UniqueFd + offset + remaining.
//   The writable-event path uses sendfile(2) to splice kernel-side. Headers
//   are written from the write_buf_ first.

#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstdint>

#include "buffer.hpp"
#include "fd.hpp"
#include "timer_queue.hpp"

namespace pulse {

enum class ConnState {
    kReadingRequest,
    kProcessing,
    kWriting,
    kClosing,
};

struct FileResponse {
    UniqueFd fd;
    off_t offset = 0;
    size_t remaining = 0;
};

struct Connection {
    // Identity
    int sock_fd = -1;              // raw fd; ownership lives in the event loop's map.
    uint64_t id  = 0;
    TimerQueue::Generation gen = 0;

    // FSM
    ConnState state = ConnState::kReadingRequest;

    // I/O buffers
    Buffer read_buf;
    Buffer write_buf;              // outgoing headers (and small bodies)
    FileResponse file_resp;        // optional file body sent via sendfile

    // Keep-alive policy from the most recent request
    bool keep_alive_after_response = false;

    // Stats / observability
    TimePoint accepted_at = now_steady();
    TimePoint last_active = now_steady();
    uint32_t requests_served = 0;
};

} // namespace pulse
