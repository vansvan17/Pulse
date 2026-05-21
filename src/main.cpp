// pulse/main.cpp — server entry point.
//
// Multi-loop model:
//   By default we run a single event loop. With --workers N, we fork N-1
//   children after binding-and-listening; each child opens its OWN listening
//   socket on the same port via SO_REUSEPORT (so we re-create the listener
//   per-process, not inherit). The parent runs the Nth loop.
//
//   We chose fork+SO_REUSEPORT over pthread because:
//     - no shared state in the data path = no locks, no false sharing
//     - per-process accounting is easier to observe with `top`, `perf`
//     - crashes are isolated to one worker
//
//   For now we use a single loop until we've validated correctness. The
//   --workers flag is plumbed but stubbed in this initial scaffold.

#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pulse/event_loop.hpp"
#include "pulse/log.hpp"

namespace {

pulse::EventLoop* g_loop = nullptr;

void handle_signal(int sig) {
    if (g_loop) g_loop->stop();
    (void)sig;
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   // intentionally NOT SA_RESTART — we want epoll_wait
                        // to return with EINTR so we can re-check stopping_.
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  -p, --port PORT          Listen port (default 8080)\n"
        "  -d, --doc-root DIR       Document root (default ./www)\n"
        "  -t, --idle-ms MS         Idle timeout in ms (default 30000)\n"
        "  -w, --workers N          Number of worker processes (default 1)\n"
        "  -v, --verbose            Enable debug logging\n"
        "  -h, --help               Show this help\n", prog);
}

} // namespace

int main(int argc, char** argv) {
    pulse::EventLoopConfig cfg;
    cfg.port = 8080;
    cfg.doc_root = "./www";
    int workers = 1;

    static struct option opts[] = {
        {"port",      required_argument, nullptr, 'p'},
        {"doc-root",  required_argument, nullptr, 'd'},
        {"idle-ms",   required_argument, nullptr, 't'},
        {"workers",   required_argument, nullptr, 'w'},
        {"verbose",   no_argument,       nullptr, 'v'},
        {"help",      no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "p:d:t:w:vh", opts, nullptr)) != -1) {
        switch (c) {
            case 'p': cfg.port = static_cast<uint16_t>(std::atoi(optarg)); break;
            case 'd': cfg.doc_root = optarg; break;
            case 't': cfg.idle_timeout = std::chrono::milliseconds(std::atoi(optarg)); break;
            case 'w': workers = std::atoi(optarg); break;
            case 'v': pulse::log::set_level(pulse::log::Level::kDebug); break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 2;
        }
    }

    if (workers < 1) workers = 1;

    install_signal_handlers();

    // Fork workers - 1 children. Each constructs its own EventLoop (with its
    // own SO_REUSEPORT listener) and runs independently.
    for (int i = 1; i < workers; ++i) {
        pid_t pid = ::fork();
        if (pid < 0) {
            std::fprintf(stderr, "fork failed: %s\n", std::strerror(errno));
            return 1;
        }
        if (pid == 0) {
            // child: break out of the fork loop and run the loop
            workers = 1;
            break;
        }
        // parent: continue forking
    }

    try {
        pulse::EventLoop loop(cfg);
        g_loop = &loop;
        loop.run();
    } catch (const std::exception& e) {
        pulse::log::error("fatal").kv("err", e.what());
        return 1;
    }

    return 0;
}
