// pulse/fd.hpp — RAII wrapper for a Linux file descriptor.
//
// Why this exists:
//   Every long-running server eventually leaks fds. Tracking ownership by hand
//   is the #1 source of "works fine for 30s then EMFILE" bugs. UniqueFd gives
//   us move-only ownership and a guaranteed close() on scope exit.
//
//   This is NOT a smart pointer to a heap object — it's just a value that
//   happens to release a kernel resource. Same pattern as std::unique_ptr but
//   for int fds with a custom "deleter" (close).

#pragma once

#include <unistd.h>
#include <utility>

namespace pulse {

class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    ~UniqueFd() { reset(); }

    // Move-only. Copying an fd would be a double-close bug waiting to happen.
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    // Release ownership without closing. Caller takes responsibility.
    int release() noexcept {
        int tmp = fd_;
        fd_ = -1;
        return tmp;
    }

    void reset(int new_fd = -1) noexcept {
        if (fd_ >= 0) {
            // Note: we intentionally ignore close() errors here. EINTR on close
            // is debated (Linux closes the fd regardless), and there is no
            // meaningful recovery in a destructor. See close(2) NOTES.
            ::close(fd_);
        }
        fd_ = new_fd;
    }

private:
    int fd_ = -1;
};

} // namespace pulse
