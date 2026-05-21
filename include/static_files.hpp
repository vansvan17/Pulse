// pulse/static_files.hpp — Serve files from a document root with safety checks.
//
// Threat model:
//   - Path traversal: "../etc/passwd". Mitigated by realpath() check that the
//     resolved path is still a prefix of the document root.
//   - Symlink escape: same realpath check handles this.
//   - Special files (devices, fifos): we stat() and only serve regular files.
//
// Why we open the fd here (not stream from disk synchronously):
//   The connection handler will pass this fd to sendfile() during writable
//   events. sendfile() pulls from the file fd in the kernel page cache and
//   pushes to the socket fd without crossing into userspace. Zero copy.
//   We pay only the open()+stat() cost on the event-loop thread, which is
//   typically a microsecond hit on hot cache.

#pragma once

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <string_view>

#include "fd.hpp"

namespace pulse::http {

inline std::string_view mime_for(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return "application/octet-stream";
    std::string_view ext = path.substr(dot + 1);

    if (ext == "html" || ext == "htm")  return "text/html; charset=utf-8";
    if (ext == "css")                    return "text/css; charset=utf-8";
    if (ext == "js")                     return "application/javascript; charset=utf-8";
    if (ext == "json")                   return "application/json";
    if (ext == "png")                    return "image/png";
    if (ext == "jpg" || ext == "jpeg")   return "image/jpeg";
    if (ext == "gif")                    return "image/gif";
    if (ext == "svg")                    return "image/svg+xml";
    if (ext == "ico")                    return "image/x-icon";
    if (ext == "txt")                    return "text/plain; charset=utf-8";
    if (ext == "pdf")                    return "application/pdf";
    if (ext == "wasm")                   return "application/wasm";
    return "application/octet-stream";
}

struct ResolvedFile {
    UniqueFd fd;
    off_t size = 0;
    std::string_view mime;
    int errno_status = 0;   // 0 on success; 404/403 mapped by caller
};

// Resolve and open a file from `doc_root` matching `url_path`.
// Returns a ResolvedFile with fd.valid() on success. On failure, errno_status
// carries an HTTP status hint (404, 403).
inline ResolvedFile resolve_and_open(std::string_view doc_root, std::string_view url_path) {
    ResolvedFile r;

    // Default to index.html for "/" or paths ending in "/".
    std::string rel(url_path);
    if (rel.empty() || rel == "/") rel = "/index.html";
    else if (rel.back() == '/') rel += "index.html";

    // Build absolute candidate path: doc_root + rel.
    std::string full;
    full.reserve(doc_root.size() + rel.size());
    full.append(doc_root.data(), doc_root.size());
    full.append(rel);

    // Resolve symlinks + ".." segments. realpath returns NULL if the path
    // doesn't exist, which we map to 404.
    char real_buf[PATH_MAX];
    if (!realpath(full.c_str(), real_buf)) {
        r.errno_status = 404;
        return r;
    }

    // Resolve doc_root too, once would be enough but cheap here. In a real
    // server we'd cache this at startup.
    char root_real[PATH_MAX];
    if (!realpath(std::string(doc_root).c_str(), root_real)) {
        r.errno_status = 500;
        return r;
    }

    // Containment check: realpath of target MUST start with realpath(doc_root) + '/'.
    // The trailing slash matters: "/var/wwwroot" should NOT match "/var/www".
    size_t root_len = std::strlen(root_real);
    if (std::strncmp(real_buf, root_real, root_len) != 0 ||
        (real_buf[root_len] != '/' && real_buf[root_len] != '\0')) {
        r.errno_status = 403;
        return r;
    }

    // Open with O_CLOEXEC so a future fork() doesn't leak the fd.
    int fd = ::open(real_buf, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        r.errno_status = (errno == EACCES) ? 403 : 404;
        return r;
    }
    UniqueFd file(fd);

    struct stat st{};
    if (::fstat(file.get(), &st) < 0) {
        r.errno_status = 500;
        return r;
    }
    // Refuse non-regular files (directories, devices, sockets, fifos).
    if (!S_ISREG(st.st_mode)) {
        r.errno_status = 403;
        return r;
    }

    r.fd = std::move(file);
    r.size = st.st_size;
    r.mime = mime_for(rel);
    return r;
}

} // namespace pulse::http
